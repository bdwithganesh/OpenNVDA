"""CTS interruption/recovery checks with a fake deqp process; no GPU required."""
import csv
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


RUNNER = Path(__file__).resolve().parents[1] / 'tools/nvk-cts/run_cts.py'
spec = importlib.util.spec_from_file_location('cts_runner', RUNNER)
cts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cts)

CASES = ['dEQP-VK.api.smoke.one', 'dEQP-VK.draw.offset_minus_one', 'dEQP-VK.api.smoke.three']
FAKE = '''import os, sys, time
from pathlib import Path
cases = %r
if '--deqp-runmode=txt-caselist' in sys.argv:
    if os.environ.get('FAKE_MODE') == 'list-fail':
        sys.exit(7)
    Path('dEQP-VK-cases.txt').write_text(''.join('TEST: ' + c + '\\n' for c in cases))
    sys.exit(0)
selected = next(a.split('=', 1)[1] for a in sys.argv if a.startswith('--deqp-caselist-file='))
mode = os.environ.get('FAKE_MODE', 'pass')
if mode == 'startup-hang':
    time.sleep(60)
for c in Path(selected).read_text().splitlines():
    with open('executed.txt', 'a') as f:
        f.write(c + '\\n')
    print("Test case '" + c + "'..", flush=True)
    if c == cases[1] and mode == 'crash':
        sys.exit(9)
    if c == cases[1] and mode == 'hang':
        Path('child.pid').write_text(str(os.getpid()))
        time.sleep(60)
    print('  Pass (OK)', end='' if mode == 'no-newline' else '\\n', flush=True)
''' % CASES


class CTSRunnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.deqp = self.root / 'deqp-vk'
        self.deqp.write_text('#!' + sys.executable + '\n' + FAKE)
        self.deqp.chmod(0o755)
        self.out = self.root / 'results.csv'

    def tearDown(self):
        self.tmp.cleanup()

    def command(self, *args):
        return [sys.executable, str(RUNNER), '--deqp', str(self.deqp),
                '--icd', 'fake.json', '--pattern', 'dEQP-VK.*', '--out', str(self.out), *args]

    def run_cli(self, *args, mode='pass'):
        return subprocess.run(self.command(*args), env=dict(os.environ, FAKE_MODE=mode),
                              capture_output=True, text=True, timeout=10)

    def rows(self):
        return cts.load_results(self.out)

    def test_stop_at_crash_preserves_pass_and_notrun(self):
        result = self.run_cli('--stop-on-crash', mode='crash')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual([r[0] for r in self.rows().values()], ['Pass', 'Crash', 'NotRun'])
        self.assertEqual((self.root / 'executed.txt').read_text().splitlines(), CASES[:2])

    def test_resume_skips_completed_and_preserves_unselected_history(self):
        self.run_cli('--stop-on-crash', mode='crash')
        result = self.run_cli('--resume', '--exclude', '*offset_minus_one*')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'executed.txt').read_text().splitlines(), CASES)
        self.assertEqual([r[0] for r in self.rows().values()], ['Pass', 'Crash', 'Pass'])

    def test_exclude_is_applied_before_limit(self):
        result = self.run_cli('--exclude', '*offset_minus_one*', '--max-cases', '2')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(self.rows()), [CASES[0], CASES[2]])

    def test_timeout_before_first_case_is_timeout(self):
        result = self.run_cli('--timeout', '1', '--stop-on-crash', mode='startup-hang')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual([r[0] for r in self.rows().values()], ['Timeout', 'NotRun', 'NotRun'])

    def test_sigterm_flushes_results_and_reaps_child(self):
        p = subprocess.Popen(self.command('--timeout', '30'),
                             env=dict(os.environ, FAKE_MODE='hang'),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while not (self.root / 'child.pid').exists():
                if p.poll() is not None or time.monotonic() > deadline:
                    self.fail('fake deqp did not reach its second case')
                time.sleep(0.02)
            # Read the CSV while the batch is still running.
            self.assertEqual(self.rows()[CASES[0]][0], 'Pass')
            child = int((self.root / 'child.pid').read_text())
            p.send_signal(signal.SIGTERM)
            _, err = p.communicate(timeout=5)
            self.assertEqual(p.returncode, 130, err)
            self.assertEqual([r[0] for r in self.rows().values()], ['Pass', 'NotRun', 'NotRun'])
            with self.assertRaises(ProcessLookupError):
                os.kill(child, 0)
            self.assertEqual(list(self.root.glob('tmp*.txt')), [])
        finally:
            if p.poll() is None:
                p.kill()
                p.communicate()

    def test_final_result_without_newline(self):
        results = {}
        consumed, hung = cts.run_batch(str(self.deqp), dict(os.environ, FAKE_MODE='no-newline'),
                                        str(self.root), [CASES[0]], 1, results)
        self.assertEqual((consumed, hung), (1, False))
        self.assertEqual(results[CASES[0]][0], 'Pass')

    def test_duplicate_journal_updates_compact_on_resume(self):
        journal = cts.ResultJournal(self.out)
        journal.record(CASES[0], ('Crash', 'exit 9'))
        journal.record(CASES[0], ('Crash', 'exit 9; GR lost'))
        journal.close()
        result = self.run_cli('--resume')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.rows()[CASES[0]], ('Crash', 'exit 9; GR lost'))
        with self.out.open(newline='') as f:
            self.assertEqual(len(list(csv.DictReader(f))), 3)

    def test_invalid_resume_csv_is_not_overwritten(self):
        self.out.write_text('case,result,detail\nbroken,Pass\n')
        original = self.out.read_bytes()
        result = self.run_cli('--resume')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.out.read_bytes(), original)

    def test_failed_case_listing_does_not_reuse_stale_list(self):
        (self.root / 'dEQP-VK-cases.txt').write_text('TEST: ' + CASES[0] + '\n')
        result = self.run_cli(mode='list-fail')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.out.exists())
        self.assertFalse((self.root / 'executed.txt').exists())


if __name__ == '__main__':
    unittest.main()
