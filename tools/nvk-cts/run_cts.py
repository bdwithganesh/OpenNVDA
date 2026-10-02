#!/usr/bin/env python3
"""Resumable Vulkan CTS runner for NVK on macOS (RTX 4080 via NVGspControl).

Runs deqp-vk over a case list and keeps going when a case crashes the
process or hangs the GPU: the case is recorded as Crash/Timeout, the GPU is
optionally reset (`nvrun --reset`), and the run resumes after that case.

  sudo python3 tools/nvk-cts/run_cts.py --deqp ~/nvk/cts/build/.../deqp-vk \\
      --icd ~/nvk/mesa-26.0.8/build/src/nouveau/vulkan/nouveau_devenv_icd.x86_64.json \\
      --pattern 'dEQP-VK.api.smoke.*' --pattern 'dEQP-VK.compute.pipeline.basic.*' \\
      [--reset-cmd '~/nvrun/nvrun --reset'] [--out results.csv]

Case selection: --pattern (fnmatch on full names, repeatable) and/or
--caselist FILE (one name per line). Without either, the SMOKE set below.
Writes each result immediately to CSV (case,result,detail), so a stopped run
keeps its evidence. --resume skips recorded cases; --exclude omits matching
cases. --stop-on-crash stops before attempting GPU recovery or another batch.
"""
import argparse
import csv
import fnmatch
import os
import select
import signal
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict

SMOKE = [
    'dEQP-VK.info.*',
    'dEQP-VK.api.smoke.*',
    'dEQP-VK.api.info.*',
    'dEQP-VK.memory.allocation.basic.*',
    'dEQP-VK.api.buffer.*',
    'dEQP-VK.api.copy_and_blit.core.buffer_to_buffer.*',
    'dEQP-VK.compute.pipeline.basic.*',
    'dEQP-VK.synchronization.basic.*',
    'dEQP-VK.renderpass.suballocation.simple.*',
    'dEQP-VK.draw.renderpass.simple_draw.*',
    'dEQP-VK.wsi.display.*',
]

RESULTS = ('Pass', 'Fail', 'QualityWarning', 'CompatibilityWarning', 'NotSupported',
           'ResourceError', 'InternalError', 'Crash', 'Timeout', 'Waiver')


def list_cases(deqp, env, workdir):
    """All case names via --deqp-runmode=txt-caselist (TEST: lines)."""
    subprocess.run([deqp, '--deqp-runmode=txt-caselist'], cwd=workdir, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    path = os.path.join(workdir, 'dEQP-VK-cases.txt')
    names = []
    with open(path) as f:
        for line in f:
            if line.startswith('TEST: '):
                names.append(line[6:].strip())
    return names


class ResultJournal:
    """Append and flush every result, including changes to crash details."""
    def __init__(self, path, append=False):
        self.file = open(path, 'a' if append else 'w', newline='')
        self.writer = csv.writer(self.file)
        if not append or self.file.tell() == 0:
            self.writer.writerow(['case', 'result', 'detail'])
            self.file.flush()

    def record(self, case, result):
        self.writer.writerow([case, *result])
        self.file.flush()

    def close(self):
        self.file.close()


def load_results(path):
    """Last row wins when a crash detail was updated or a case retried."""
    results = {}
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != ['case', 'result', 'detail']:
            raise ValueError(f'{path}: expected case,result,detail CSV header')
        for row in reader:
            if row['result'] not in RESULTS + ('NotRun',) or row['detail'] is None:
                raise ValueError(f'{path}: invalid or incomplete result row')
            results[row['case']] = (row['result'], row['detail'])
    return results


def write_results(path, cases, results):
    """Atomically compact the journal into one row per selected case."""
    fd, tmp = tempfile.mkstemp(prefix='.nvk-cts-', dir=os.path.dirname(os.path.abspath(path)))
    try:
        with os.fdopen(fd, 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['case', 'result', 'detail'])
            for case in cases:
                writer.writerow([case, *results.get(case, ('NotRun', ''))])
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def run_batch(deqp, env, workdir, cases, timeout_s, results, record=None):
    """Run `cases` in one deqp-vk process. Returns the index of the first case
    that did not get a result (crash/hang), or len(cases) when all finished."""
    fd, listfile = tempfile.mkstemp(suffix='.txt', dir=workdir)
    with os.fdopen(fd, 'w') as f:
        f.write('\n'.join(cases) + '\n')
    cmd = [deqp, f'--deqp-caselist-file={listfile}', '--deqp-log-images=disable',
           '--deqp-log-shader-sources=disable', '--deqp-log-filename=/dev/null',
           '--deqp-watchdog=disable']
    p = None
    current = None
    last = time.monotonic()
    hung = False
    pending = b''
    def save(case, result):
        results[case] = result
        if record:
            record(case, result)

    def parse(raw):
        nonlocal current
        line = raw.decode('utf-8', 'replace')
        if line.startswith("Test case '"):
            current = line.split("'")[1]
        elif current and line.startswith('  '):
            word = line.strip().split(' ', 1)
            if word[0] in RESULTS:
                save(current, (word[0], word[1].strip('()') if len(word) > 1 else ''))
                current = None

    try:
        p = subprocess.Popen(cmd, cwd=workdir, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        fdo = p.stdout.fileno()
        while True:
            r, _, _ = select.select([fdo], [], [], min(1.0, timeout_s))
            if r:
                chunk = os.read(fdo, 65536)       # unbuffered: select stays truthful
                if not chunk:
                    break
                last = time.monotonic()
                pending += chunk
                *lines, pending = pending.split(b'\n')
                for raw in lines:
                    parse(raw)
            elif time.monotonic() - last > timeout_s:
                hung = True
                p.kill()
                break
        if pending:
            parse(pending)                       # final line need not have a newline
        p.wait()
    finally:
        if p is not None:
            if p.poll() is None:
                p.kill()
            p.wait()
            p.stdout.close()
        os.unlink(listfile)
    if current is not None:
        save(current, ('Timeout' if hung else 'Crash',
                       f'no result after {timeout_s}s' if hung else f'exit {p.returncode}'))
        return cases.index(current) + 1, hung
    # The process can also die between cases (or before the first one); then
    # blame the first case of this batch that has no result and go on after it.
    for i, c in enumerate(cases):
        if c not in results:
            save(c, ('Timeout' if hung else 'Crash',
                     f'no result after {timeout_s}s before a case' if hung else
                     f'exit {p.returncode} before a result'))
            return i + 1, hung
    return len(cases), False


def gsp_prop(name):
    """One NVGspControl-<name> ioreg value as text ('' when not there)."""
    out = subprocess.run(['ioreg', '-r', '-c', 'NVGspControl', '-l', '-w0'],
                         capture_output=True, text=True).stdout
    key = f'"NVGspControl-{name}" = '
    for line in out.splitlines():
        if key in line:
            return line.split(key, 1)[1].strip().strip('"')
    return ''


def wait_gr(reset_cmd, limit_s=45):
    """After a crash the GR channel may be dead (RC). The kext auto-resets at
    most once per 30 s, so wait for gr-persistent, else reset ourselves."""
    t = time.monotonic()
    while time.monotonic() - t < limit_s:
        if gsp_prop('gr-persistent') == 'Yes':
            return True
        time.sleep(2)
    if reset_cmd:
        subprocess.run(os.path.expanduser(reset_cmd), shell=True)
        time.sleep(5)
    return gsp_prop('gr-persistent') == 'Yes'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--deqp', required=True)
    ap.add_argument('--icd', required=True, help='NVK ICD json (VK_ICD_FILENAMES)')
    ap.add_argument('--pattern', action='append', default=[])
    ap.add_argument('--caselist')
    ap.add_argument('--exclude', action='append', default=[], help='omit matching case names (repeatable fnmatch)')
    ap.add_argument('--resume', action='store_true', help='skip results already in --out (except NotRun)')
    ap.add_argument('--stop-on-crash', action='store_true', help='stop after Crash/Timeout, without recovery')
    ap.add_argument('--timeout', type=int, default=60, help='seconds without output = hang')
    ap.add_argument('--reset-cmd', help='run after a hang, e.g. "~/nvrun/nvrun --reset"')
    ap.add_argument('--out', default='nvk-cts-results.csv')
    ap.add_argument('--max-cases', type=int, default=0)
    a = ap.parse_args()
    if a.timeout <= 0 or a.max_cases < 0:
        ap.error('--timeout must be positive and --max-cases must be nonnegative')

    deqp = os.path.abspath(os.path.expanduser(a.deqp))
    workdir = os.path.dirname(deqp)           # deqp-vk finds its data next to itself
    env = dict(os.environ, VK_ICD_FILENAMES=os.path.expanduser(a.icd))

    if a.caselist:
        with open(a.caselist) as f:
            cases = [l.strip() for l in f if l.strip() and not l.lstrip().startswith('#')]
        # deqp runs a caselist in its own tree order; resuming after a crash
        # assumes our order is the same, or cases get skipped as NotRun.
        rank = {c: i for i, c in enumerate(list_cases(deqp, env, workdir))}
        cases.sort(key=lambda c: rank.get(c, len(rank)))
    else:
        patterns = a.pattern or SMOKE
        allc = list_cases(deqp, env, workdir)
        cases = [c for c in allc if any(fnmatch.fnmatchcase(c, p) for p in patterns)]
    cases = list(dict.fromkeys(c for c in cases if not any(
        fnmatch.fnmatchcase(c, pattern) for pattern in a.exclude)))
    if a.max_cases:
        cases = cases[:a.max_cases]
    print(f'{len(cases)} cases', flush=True)

    results = load_results(a.out) if a.resume and os.path.exists(a.out) else {}
    # Keep rows from earlier selections when resuming with a narrower pattern.
    output_cases = list(dict.fromkeys([*results, *cases]))
    results = {c: r for c, r in results.items() if r[0] != 'NotRun'}
    pending_cases = [c for c in cases if c not in results]
    journal = ResultJournal(a.out, append=a.resume and os.path.exists(a.out))
    start = 0
    t0 = time.time()
    interrupted = False
    def terminate(signum, frame):
        raise KeyboardInterrupt
    previous_term = signal.signal(signal.SIGTERM, terminate)
    try:
        while start < len(pending_cases):
            consumed, hung = run_batch(deqp, env, workdir, pending_cases[start:], a.timeout,
                                       results, journal.record)
            start += consumed
            if consumed and a.stop_on_crash and results[pending_cases[start - 1]][0] in ('Crash', 'Timeout'):
                print(f'stopping after {pending_cases[start - 1]}', flush=True)
                break
            if hung and a.reset_cmd:
                print(f'GPU hang near {pending_cases[start - 1]}: {a.reset_cmd}', flush=True)
                subprocess.run(os.path.expanduser(a.reset_cmd), shell=True)
            elif consumed == 0:
                break
            elif results.get(pending_cases[start - 1], ('',))[0] == 'Crash':
                c = pending_cases[start - 1]
                time.sleep(2)   # let an automatic GPU reset (RC) start first
                if gsp_prop('gr-persistent') != 'Yes':
                    xid = gsp_prop('os-error-last')
                    results[c] = ('Crash', f'{results[c][1]}; GR lost: {xid}')
                    journal.record(c, results[c])
                    print(f'GR lost at {c}: {xid}', flush=True)
                    if not wait_gr(a.reset_cmd):
                        print('GR did not come back, stopping', flush=True)
                        break
                elif start < len(pending_cases):
                    print(f'crash at {c}, resuming', flush=True)
    except KeyboardInterrupt:
        interrupted = True
        print('interrupted; completed results saved', flush=True)
    finally:
        signal.signal(signal.SIGTERM, previous_term)
        journal.close()
        write_results(a.out, output_cases, results)

    results = {c: results.get(c, ('NotRun', '')) for c in cases}

    total = Counter(r for r, _ in results.values())
    groups = defaultdict(Counter)
    for c, (r, _) in results.items():
        groups['.'.join(c.split('.')[:3])][r] += 1
    print(f'\n{len(cases)} cases in {time.time() - t0:.0f} s: ' +
          ', '.join(f'{k} {v}' for k, v in total.most_common()))
    for g in sorted(groups):
        print(f'  {g}: ' + ', '.join(f'{k} {v}' for k, v in groups[g].most_common()))
    bad = [(c, r, d) for c, (r, d) in results.items() if r in ('Fail', 'Crash', 'Timeout')]
    for c, r, d in bad[:40]:
        print(f'  {r:8s} {c}  {d}')
    print(f'results: {a.out}')
    return 130 if interrupted else 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
