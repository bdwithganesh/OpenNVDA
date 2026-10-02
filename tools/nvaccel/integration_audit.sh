#!/bin/sh
# Integration audit of the GPU stack on the target, since boot (or the last
# N minutes): is WindowServer on the Metal compositor, what did any process
# hit that we do not implement, what failed, what crashed. Run after every
# deploy and after every login test; the list must go to zero.
#   sh integration_audit.sh [minutes]      (needs sudo for other processes' logs)
# Expects in ~/nvbuild: metal_coverage, fam (device capability dump), runsuite.sh.
M=${1:-}
cd ~/nvbuild || exit 1
if [ -n "$M" ]; then SINCE="--last ${M}m"; else
  B=$(sysctl -n kern.boottime | sed -E 's/.*sec = ([0-9]+).*/\1/')
  SINCE="--start $(date -r "$B" '+%Y-%m-%d %H:%M:%S')"
fi
L=/tmp/audit-log.txt
# shellcheck disable=SC2086
sudo log show $SINCE --info --style compact --predicate \
  'eventMessage CONTAINS "NVMTL" OR eventMessage CONTAINS "NVAccel" OR eventMessage CONTAINS "NVGsp" OR eventMessage CONTAINS "compositor activated" OR eventMessage CONTAINS "Failed to create MetalDevice" OR eventMessage CONTAINS "GPU reset"' \
  2>/dev/null > $L
count() { grep -cE "$1" $L; }
uniq_of() { grep -E "$1" $L | sed -E 's/^.{24}//; s/\[[0-9]+:[0-9a-f]+\]//; s/0x[0-9a-f]+/#/g' | cut -c1-170 | sort | uniq -c | sort -rn | head -${2:-15}; }

echo "== WindowServer"
WS=$(pgrep -x WindowServer)
sudo log show $SINCE --info --style compact --predicate "processID == $WS AND eventMessage CONTAINS \"compositor activated\"" 2>/dev/null |
  tail -1 | sed -E 's/^.{24}//'
ioreg -rc NVGspControl -l -w0 | grep -o "\[pid $WS [^]]*\]" | head -1 || echo "  no GPU memory"
echo "== device"
[ -x ./fam ] && perl -e 'alarm 30; exec @ARGV' ./fam 2>/dev/null | grep -v "^icb\|argenc" | head -3
[ -x ./metal_coverage ] && perl -e 'alarm 60; exec @ARGV' ./metal_coverage 2>/dev/null | grep -E "^MISSING|^NOOBJ|MISSING," | tail -20
echo "== unimplemented selectors (by any process)"
uniq_of 'unimplemented -\[' 30
echo "== shader compile failures"
uniq_of 'AIR\) failed|no NAK for' 20
echo "== pipelines / textures / formats refused"
uniq_of 'unsupported|refused|not supported yet|not applied yet' 20
echo "== launch / memory / GPU failures"
uniq_of 'launch failed|memAlloc failed|GPU unavailable|ops dropped|execSegments failed|GPU reset|MMU|fault' 20
echo "== kext stubs reached"
uniq_of 'stub\)|stub:|not implemented' 15
echo "== WindowServer errors (CoreDisplay / IOPresentment / SkyLight)"
# shellcheck disable=SC2086
sudo log show $SINCE --style compact --predicate "processID == $WS AND messageType == error" 2>/dev/null |
  grep -E "CoreDisplay|IOPresentment|SkyLight|Metal" | sed -E 's/^.{24}//; s/\[[0-9]+:[0-9a-f]+\]//; s/0x[0-9a-f]+/#/g' |
  cut -c1-170 | sort | uniq -c | sort -rn | head -15
echo "== crash reports since boot"
find ~/Library/Logs/DiagnosticReports /Library/Logs/DiagnosticReports -newer /var/run/com.apple.WindowServer.didRunThisBoot \
  -name "*.ips" 2>/dev/null | sed 's#.*/##' | sort | head -30
echo "== GPU counters"
ioreg -rc NVGspControl -l -w0 | grep -oE '"NVGspControl-(rc-count|mmu-fault-events|autoreset-count)" = [0-9]+'
ioreg -rc NVGspControl -l -w0 | grep -o '"NVGspControl-mem-by-client" = "slots [^,]*'
