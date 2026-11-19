#!/bin/sh
# autowarm_auxguard.sh (root): add the AuxKC guard to autowarm.sh (backup kept) and install auxrebuild.sh next to it.
set -eu
D=/Library/NativeDriverLab/autogpu
grep -q aux-reboots "$D/autowarm.sh" && { echo "already patched"; exit 0; }
cp -p "$D/autowarm.sh" "$D/autowarm.sh.before-auxguard-$(date +%s)"
cp /Users/dhruveen/nvbuild/perf/auxrebuild.sh "$D/auxrebuild.sh"; chown root:wheel "$D/auxrebuild.sh"; chmod 755 "$D/auxrebuild.sh"
python3 - "$D/autowarm.sh" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
anchor='  exit 0\nfi\nrm -f "$D/vpci-reboots"\n'
assert s.count(anchor)==1
block='''  # 2026-11-19: kernelmanagerd rewrote the AuxKC without NVAccelerator (a load request for another kext, 16:30):
  # every boot then had no GPU and a black monitor. Rebuild it from /Library/Extensions (kexts untouched, so their
  # approvals hold) and reboot, at most once in a row.
  if ioreg -r -c NVGspControl -d 1 2>/dev/null | grep -q NVGspControl &&
     ! strings -a /Library/KernelCollections/AuxiliaryKernelExtensions.kc | grep -q org.local.macosdevicelab.NVAccelerator; then
    n=$(cat "$D/aux-reboots" 2>/dev/null || echo 0)
    if [ "$n" -lt 1 ] && sh "$D/auxrebuild.sh"; then
      echo $((n+1)) > "$D/aux-reboots"; sync
      echo "AuxKC had no NVAccelerator: rebuilt; reboot"
      /sbin/shutdown -r now
    else
      echo "AuxKC has no NVAccelerator; rebuild not attempted or failed (count $n)"
    fi
  fi
'''
s=s.replace(anchor, block+anchor+'rm -f "$D/aux-reboots"\n')
open(p,'w').write(s)
PY
sh -n "$D/autowarm.sh" && echo "syntax ok"
grep -n "aux-reboots\|auxrebuild" "$D/autowarm.sh"
