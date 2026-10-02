#!/bin/sh
# Hardware-free checks. Nothing is installed or loaded.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
mkdir -p work/host-checks
python3 - <<'PY'
import subprocess

checks = [
    'nvgsp_cursor_offset', 'nvgsp_cursor_arm', 'nvgsp_cursor_move', 'nvgsp_lut_identity',
    'nvgsp_vram_dword_read', 'nvdisplay_window_probe', 'nvdisplay_cursor_probe',
    'nvdisplay_cursor_setup', 'nvdisplay_crc_probe', 'nvdisplay_output_probe',
    'nvgsp_context_clear', 'nvaccel_stamp_watchdog', 'nvgsp_rpc_ownership',
    'nvgsp_lifecycle_gate', 'nvgsp_reset_channel_fallback', 'nvgsp_stamp_reuse',
]
for name in checks:
    result = subprocess.run(['python3', 'tests/test_' + name + '.py'],
                            capture_output=True, text=True, timeout=120)
    print(name + ': ' + ('PASS' if result.returncode == 0 else 'FAIL'), flush=True)
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(result.returncode)
PY
for check in nvgsp_display_lut nvgsp_gr_context nvgsp_residency nvgsp_rpc_reply nvgsp_arena_map nvgsp_arena_pages nvgsp_vram_heap nvgsp_window_surface nvdisplay_edid; do
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror "tests/${check}_check.cpp" -o "work/host-checks/$check"
    "work/host-checks/$check"
done
