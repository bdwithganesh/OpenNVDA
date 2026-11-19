#!/bin/sh
# flipcpu.sh (root): kernel_task CPU and pipe flips/s while glass_win animates (display pipe copy cost)
P=/Users/dhruveen/nvbuild/perf
c() { ioreg -l -w0 | sed -n 's/.*"NVAccelerator-flip-copies" = \([0-9]*\).*/\1/p' | head -1; }
launchctl asuser 501 /usr/bin/caffeinate -u -t 20 &
launchctl asuser 501 $P/${APP:-ca_spin} ${ARG:-15} > /tmp/gw.out 2>&1 &
sleep 3
a=$(c)
top -l 2 -s 8 -stats pid,command,cpu -o cpu -n 6 | tail -7
b=$(c)
echo "flips in ~8s: $((b-a))"
pkill -x ${APP:-ca_spin}; tail -3 /tmp/gw.out; wait 2>/dev/null
echo "idle:"; top -l 2 -s 4 -stats pid,command,cpu -o cpu -n 4 | tail -5
