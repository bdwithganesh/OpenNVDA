#!/bin/sh
# 2026-11-16: instant replug action. The kernel poll (NVGspControl
# hotplug-poll-mask) detects the DP sink over AUX; on 0 -> 0x200 run the
# proven existing-object retrain (disp_replug.py: manual DP, SOR assign, sink
# D0, 4xHBR2 training, SST stream) which takes ~0.1 s. Runs after the boot
# bring-up (copy thread on) so it never races disp_all's allocations.
# 2026-11-17: kernel 0.180.152 publishes the debounced connect state as -mask
# (it was the raw sample, so one failed AUX read retrained a live link).
# No-monitor boot: disp_all.py stops at the DP link and leaves
# /Library/NativeDriverLab/display/state/disp615-partial.json; once a sink answers, finish the bring-up with
# disp_all.py --resume (retried at most every 30 s, or at once on a new plug).
LOG=/Library/NativeDriverLab/display/hotplug.log
D615=/Users/dhruveen/nvbuild/d615
exec >> "$LOG" 2>&1
echo "hotplug-watch start $(date)"
mask() { /usr/sbin/ioreg -l -w0 -r -c NVGspControl 2>/dev/null | grep 'hotplug-poll-mask' | head -1 | sed 's/.*= //'; }
# 0.180.199+: the mask holds the bit of the DP display ID the kernel polls (0x2000 = 8192 on DP connector 0), so any nonzero value means a sink.
sink() { [ -n "$1" ] && [ "$1" != "0" ]; }
copyon() { /usr/sbin/ioreg -l -w0 -r -c NVGspControl 2>/dev/null | grep -q 'display615-copy-on" = Yes'; }
n=0; last=0; pm=$(mask)
until copyon; do
    m=$(mask)
    if [ -e /Library/NativeDriverLab/display/state/disp615-partial.json ] && ! /usr/bin/pgrep -f nvdisp615-boot.sh >/dev/null && sink "$m"; then
        now=$(date +%s)
        if ! sink "$pm" || [ $((now - last)) -ge 30 ]; then
            echo "$(date) sink present (mask $pm -> $m), resuming display bring-up"
            sleep 0.6
            /usr/bin/python3 "$D615/disp_all.py" desktop --resume
            last=$(date +%s)
        fi
    fi
    pm=$m
    n=$((n+1))
    if [ ! -e /Library/NativeDriverLab/display/state/disp615-partial.json ] && [ $n -ge 1200 ]; then echo "copy never started"; exit 1; fi
    sleep 0.5
done
# 2026-11-18: WindowServer's gamma reaches the head OLUT only with a core
# update; reload it whenever the kernel's gamma-set count moves (and once now).
gsets() { /usr/sbin/ioreg -l -w0 -r -c NVGspControl 2>/dev/null | grep '"NVGspControl-gamma-sets"' | head -1 | sed 's/.*= //'; }
prev=$(mask)
pg=$(gsets)
/usr/bin/python3 "$D615/olut_kick.py"
# 2026-11-18: double-buffered scan-out (NVGspControl 0.180.181+): the kernel
# copies into the hidden buffer 32 MiB below the scan-out surface and flips
# window 0 to it at a vblank, so the head never scans a half-copied frame.
# disp_all.py's fbcopy (op 7) turns it off, so only the boot bring-up needs this.
/usr/bin/python3 - <<'PY'
import json, subprocess
st = json.load(open('/Library/NativeDriverLab/display/state/disp615.json'))
back = st['surface_offset'] - 0x2000000
d = '/Users/dhruveen/nvbuild/d615/d615'
print('dbl reserve', subprocess.run([d, 'reserve', '32', hex(back)], capture_output=True, text=True).stdout.strip(),
      'enable', subprocess.run([d, 'dbl', hex(back), hex(st['wnd_pb']), '0x690000'], capture_output=True, text=True).stdout.strip())
PY
echo "watching mask=$prev gamma-sets=$pg"
while :; do
    g=$(gsets)
    if [ "$g" != "$pg" ]; then
        echo "$(date) gamma-sets $pg -> $g"
        /usr/bin/python3 "$D615/olut_kick.py"
        pg=$g
    fi
    m=$(mask)
    if [ "$m" != "$prev" ]; then
        echo "$(date) mask $prev -> $m"
        if sink "$m"; then
            sleep 0.6
            /usr/bin/python3 "$D615/disp_replug.py"
        fi
        prev=$m
    fi
    sleep 0.25
done
