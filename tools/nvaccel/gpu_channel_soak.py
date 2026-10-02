#!/usr/bin/env python3
"""Target-side mixed channel soak; preserve every result and stop on GPU changes.

Run from ~/nvbuild: python3 tools/gpu_channel_soak.py --seconds 600 --out DIR
This script never resets the GPU or changes the installed driver/NVRAM.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


def snapshot():
    raw = subprocess.check_output(
        ["ioreg", "-rc", "NVGspControl", "-l", "-w0"], text=True, timeout=15)
    values = dict(re.findall(r'"(NVGspControl-[^"]+)" = (.*)', raw))
    state = {key: values.get("NVGspControl-" + key, "0")
             for key in ("rc-count", "gpu-reset-count", "reset-busy")}
    state["gr-persistent"] = values.get("NVGspControl-gr-persistent", "MISSING")
    state["gpu-events-sha256"] = hashlib.sha256(
        values.get("NVGspControl-gpu-events", "").encode()).hexdigest()
    return raw, state


def capture_gsp_logs(out, record):
    """Read firmware buffers after a failure, before recovery restages them."""
    started = time.monotonic()
    _, before = snapshot()
    for index in (2, 1, 4, 0, 3):  # RM first, then interrupt/kernel/boot logs
        logfile = out / ("trigger-gsp-log%d.bin" % index)
        try:
            result = subprocess.run(["sudo", "-n", "./nvgsp_load", "--gsplog",
                                     str(index), str(logfile)], capture_output=True,
                                    text=True, timeout=1)
            record({"gsp_log": index, "rc": result.returncode,
                    "bytes": logfile.stat().st_size if logfile.exists() else 0,
                    "output": (result.stdout + result.stderr).strip()})
        except (OSError, subprocess.TimeoutExpired) as exc:
            record({"gsp_log": index, "capture_error": str(exc)})
    _, after = snapshot()
    record({"gsp_log_capture": {"before": before, "after": after,
            "same_reset_generation": before["gpu-reset-count"] == after["gpu-reset-count"],
            "elapsed": round(time.monotonic() - started, 3)}})


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=int, default=600)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--no-sgemm", action="store_true")
    ap.add_argument("--no-vk", action="store_true")
    ap.add_argument("--ctxsw-only", action="store_true")
    ap.add_argument("--ctxsw-count", type=int, default=3,
                    help="parallel checked context clients (1..48); >32 probes channel fallback")
    ap.add_argument("--ctxsw-seconds", type=int, default=5)
    ap.add_argument("--overhead-dispatches", type=int, default=1000,
                    help="dispatches in each overhead compute batch (1..1000)")
    ap.add_argument("--capture-gsp-logs", action="store_true",
                    help="read five firmware log buffers on the first GPU-state change")
    ap.add_argument("--omit", action="append", default=[])
    args = ap.parse_args()
    if args.seconds <= 0:
        ap.error("--seconds must be positive")
    if not 1 <= args.ctxsw_count <= 48:
        ap.error("--ctxsw-count must be in 1..48")
    if not 1 <= args.ctxsw_seconds <= 60:
        ap.error("--ctxsw-seconds must be in 1..60")
    if not 1 <= args.overhead_dispatches <= 1000:
        ap.error("--overhead-dispatches must be in 1..1000")
    test_timeout = max(60, args.ctxsw_seconds + 15)
    args.out.mkdir(parents=True, exist_ok=False)
    records = []
    children = []
    failed = False
    waves = 0

    def record(item):
        records.append(item)
        with (args.out / "results.jsonl").open("a") as f:
            f.write(json.dumps(item) + "\n")
        print(json.dumps(item), flush=True)

    def stop(proc, sig):
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            pass
        except PermissionError:
            # macOS can deny signalling a protected child from an unprivileged
            # harness. Use the lab's passwordless sudo, never an interactive prompt.
            result = subprocess.run(["sudo", "-n", sys.executable, "-c",
                            "import os,sys\ntry: os.killpg(int(sys.argv[1]),int(sys.argv[2]))\n"
                            "except ProcessLookupError: pass\n",
                            str(proc.pid), str(int(sig))], timeout=5)
            if result.returncode:
                record({"signal_pending": proc.pid, "signal": int(sig)})

    try:
        accelerator = subprocess.check_output(
            ["ioreg", "-rc", "NVAccelerator", "-l", "-w0"], text=True, timeout=15)
        (args.out / "accelerator.ioreg").write_text(accelerator)
        if '"NVAccelerator-native" = Yes' not in accelerator:
            raise RuntimeError("native Metal accelerator is not ready")
        raw, base = snapshot()
        (args.out / "before.ioreg").write_text(raw)
        record({"baseline": base, "overhead_dispatches": args.overhead_dispatches})
        if base["gr-persistent"] != "Yes" or base["reset-busy"] not in ("0", "No"):
            raise RuntimeError("GPU not ready")
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            waves += 1
            specs = [("ctxsw%d" % n, ["./metal_ctxsw_test",
                      str(waves * max(10, args.ctxsw_count + 1) + n), str(args.ctxsw_seconds)], None)
                     for n in range(1, args.ctxsw_count + 1)]
            specs += [(name, ["./" + name] + extra, None) for name, extra in (
                ("metal_overhead_bench", []), ("metal_mix_test", []),
                ("metal_3d_test", ["gfx.metallib"]), ("metal_blurchain_test", []),
                ("cl_more", []))]
            if not args.no_sgemm:
                specs.append(("metal_sgemm", ["./metal_sgemm"], None))
            if not args.no_vk:
                specs.append(("vkc", [str(Path.home() / "nvk-tahoe/vkc")], {
                    "VK_ICD_FILENAMES": str(Path.home() / "nvk-tahoe/lib/nouveau_icd.x86_64.json"),
                    "DYLD_LIBRARY_PATH": str(Path.home() / "nvk-tahoe/lib")}))
            if args.ctxsw_only:
                specs = [s for s in specs if s[0].startswith("ctxsw")]
            specs = [s for s in specs if s[0] not in args.omit]
            if not specs:
                raise RuntimeError("no tests selected")
            children = []
            for name, cmd, extra_env in specs:
                if name == "metal_overhead_bench":
                    extra_env = dict(extra_env or {}, OVH_BATCH_DISPATCHES=str(args.overhead_dispatches))
                log = args.out / ("wave-%03d-%s.log" % (waves, name))
                stream = log.open("w")
                try:
                    proc = subprocess.Popen(cmd, stdout=stream, stderr=subprocess.STDOUT,
                                            cwd=str(Path.home() / "nvk-tahoe") if name == "vkc" else None,
                                            env=dict(os.environ, **(extra_env or {})),
                                            start_new_session=True)
                finally:
                    stream.close()
                children.append((name, proc, log, time.monotonic()))
            changed = None
            while any(proc.poll() is None for _, proc, _, _ in children):
                time.sleep(0.5)
                raw, state = snapshot()
                if state != base:
                    changed = state
                    (args.out / "trigger.ioreg").write_text(raw)
                    (args.out / "event.ioreg").write_text(raw)
                    record({"wave": waves, "gpu_change_detected": state,
                            "changed_keys": [k for k in state if state[k] != base[k]]})
                    if args.capture_gsp_logs:
                        capture_gsp_logs(args.out, record)
                    break
                if any(proc.poll() is None and time.monotonic() - started > test_timeout
                       for _, proc, _, started in children):
                    record({"wave": waves, "error": "test exceeded %d seconds" % test_timeout})
                    failed = True
                    break
            for name, proc, log, started in children:
                if proc.poll() is None:
                    stop(proc, signal.SIGTERM)
                    try:
                        proc.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        stop(proc, signal.SIGKILL)
                rc = proc.wait(timeout=15)
                output = log.read_text(errors="replace")
                ok = rc == 0
                if name.startswith("ctxsw"):
                    match = re.search(r': (\d+) rounds, (\d+) wrong.*: PASS', output)
                    ok = ok and match is not None and int(match[1]) > 0 and int(match[2]) == 0
                elif name == "metal_overhead_bench":
                    ok = (ok and "NVIDIA" in output and "(null)" not in output and
                          "overhead command buffers and compute output: PASS" in output)
                elif name == "metal_sgemm":
                    ok = ok and "sgemm command buffers and sampled output: PASS (5 checks, 0 failed)" in output
                record({"wave": waves, "test": name, "pid": proc.pid, "rc": rc,
                        "ok": ok, "log": log.name,
                        "elapsed": round(time.monotonic() - started, 2)})
                failed |= not ok
            children = []
            raw, state = snapshot()
            if changed is not None or state != base:
                (args.out / "event.ioreg").write_text(raw)
                record({"wave": waves, "gpu_changed": changed or state})
                failed = True
            if failed:
                break
    except Exception as exc:
        record({"error": repr(exc)})
        failed = True
    finally:
        for _, proc, _, _ in children:
            if proc.poll() is None:
                try:
                    stop(proc, signal.SIGKILL)
                    proc.wait(timeout=15)
                except Exception as exc:
                    record({"cleanup_pending": proc.pid, "error": repr(exc)})
                    failed = True
        try:
            raw, state = snapshot()
            (args.out / "after.ioreg").write_text(raw)
        except Exception as exc:
            record({"snapshot_error": repr(exc)})
            failed = True
        record({"done": True, "waves": waves, "failed": failed,
                "tests": sum("test" in r for r in records),
                "passed": sum(r.get("ok", False) for r in records)})
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
