#!/bin/sh
# Boot-time native615 bring-up (firmware boot, root runtime + warmup, accelGo),
# the same steps as the manual epoch sequence. Root-owned files only.
D=/Library/NativeDriverLab/autogpu
cd "$D" || exit 1
BT=$(sysctl -n kern.boottime | sed 's/^{ sec = \([0-9]*\),.*/\1/')
L="$D/log-$BT.txt"
exec >> "$L" 2>&1
T0=$(date +%s)
ts() { perl -MTime::HiRes=clock_gettime,CLOCK_MONOTONIC -e 'printf "%s at uptime %.2f s\n", $ARGV[0], clock_gettime(CLOCK_MONOTONIC)' "$1"; }   # monotonic: kern.boottime moves when the clock syncs
echo "autowarm start $(date) boot $BT"
ts start
[ -e "$D/disabled" ] && { echo "disabled"; exit 0; }
if [ -e "$D/attempt" ]; then
  echo "previous attempt $(cat "$D/attempt") never finished; skipping this boot"
  mv "$D/attempt" "$D/attempt-unfinished-$BT"; exit 0
fi
# 2026-11-17: versions come from kextver (KextManagerCopyLoadedKextInfo,
# ~25 ms). kextstat is a kmutil shim here: ~1 s per call, and kmutil mounts
# the System volume read-write (authenticated root off), which broke the seal
# and scheduled an APFS snapshot revert on every boot.
i=0; ok=0
while [ $i -lt 120 ]; do
  # 2026-11-19: shell, not python3: one cold python3 start was 1.9 s of every boot. Same four checks.
  k=$(./kextver org.local.macosdevicelab.NVGspControl org.local.macosdevicelab.NVAccelerator org.local.macosdevicelab.NVAccelerator2 org.local.macosdevicelab.NVAccelerator3 org.local.macosdevicelab.NVAccelerator4 org.local.macosdevicelab.NVAccelerator5 org.local.macosdevicelab.NVAccelerator6 org.local.macosdevicelab.NVAccelerator7 org.local.macosdevicelab.NVAccelerator8 org.local.macosdevicelab.NVAccelerator9 org.local.macosdevicelab.NVAccelerator10 org.local.macosdevicelab.NVAccelerator11 org.local.macosdevicelab.NVAccelerator12 org.local.macosdevicelab.NVAccelerator13 org.local.macosdevicelab.NVAccelerator14 org.local.macosdevicelab.NVAccelerator15)
  p=$(ioreg -r -c NVGspControl -l -w0 2>/dev/null)
  if ! printf '%s\n' "$k" | grep -qxF "org.local.macosdevicelab.NVGspControl $(cat "$D/version")"; then echo "AssertionError: kext version"; false
  elif ! printf '%s\n' "$k" | grep -q '^org.local.macosdevicelab.NVAccelerator'; then echo "AssertionError: NVAccelerator"; false
  elif [ -z "$p" ]; then echo "AssertionError: no NVGspControl"; false
  elif printf '%s\n' "$p" | grep -q '"NVGspControl-gmc-init-ready" = Yes'; then echo "AssertionError: already initialized"; false
  elif printf '%s\n' "$p" | grep -q '"NVGspControl-native615-attempted" = Yes'; then echo "AssertionError: native615 attempted"; false
  else echo "guards ok"; true; fi
  if [ $? -eq 0 ]; then ok=1; break; fi
  [ $i = 0 ] && ts first-guard-fail
  i=$((i+1)); sleep 0.25
done
if [ $ok != 1 ]; then
  echo "guards failed after $((i*250)) ms"
  # 2026-11-17: Hyper-V vPCI negotiation stall. MacHyperVSupport 1.0.7's
  # HyperVPCIBridge::start can wait forever for the bus-relations reply
  # (untimed IOLockSleep): the bridge stays !registered, no 10de device
  # appears and NVGspControl never matches. A guest reboot clears it. At most
  # two automatic reboots in a row; the counter resets once the GPU is seen.
  if ! ioreg -r -c NVGspControl -d 1 2>/dev/null | grep -q NVGspControl &&
     ! ioreg -l -w0 2>/dev/null | grep -q '"vendor-id" = <de100000>'; then
    n=$(cat "$D/vpci-reboots" 2>/dev/null || echo 0)
    if [ ! -e "$D/no-vpci-reboot" ] && [ "$n" -lt 2 ]; then
      echo $((n+1)) > "$D/vpci-reboots"; sync
      echo "no NVIDIA PCI function (vPCI stall); reboot $((n+1))/2"
      /sbin/shutdown -r now
    else
      echo "no NVIDIA PCI function; automatic reboot not attempted (count $n)"
    fi
  fi
  # 2026-11-19: kernelmanagerd rewrote the AuxKC without NVAccelerator (a load request for another kext, 16:30):
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
  exit 0
fi
rm -f "$D/vpci-reboots"
ts guards
rm -f "$D/aux-reboots"
echo "$BT" > "$D/attempt"; sync
./nvgsp_load-v2 ad103-gsp-615.78.08-complete.pkg > stage-$BT.log 2>&1 || { echo "stage failed"; rm -f "$D/attempt"; exit 0; }
ts staged
./nvgsp_load-v2 --boot-staged > boot-$BT.log 2>&1 || { echo "firmware boot failed"; rm -f "$D/attempt"; exit 0; }
ts firmware-booted
ok=0
for i in $(seq 1 30); do
  ./nvgsp_load-v2 --status >> status-$BT.log 2>&1 || true   # status polls drive post-init
  p=$(ioreg -r -c NVGspControl -l -w0 2>/dev/null)
  if printf '%s\n' "$p" | grep -qE '"NVGspControl-post-init-phase" = 27$' &&
     ! printf '%s\n' "$p" | grep -qE '"NVGspControl-status-bad-reason" = [1-9]'; then ok=1; break; fi
  sleep 0.25
done
[ $ok = 1 ] || { echo "post-init phase 27 not reached"; rm -f "$D/attempt"; exit 0; }
echo "post-init ready after $(( $(date +%s) - T0 )) s"
./nvgsp_native_runtime615 --bootstrap > bootstrap-$BT.json 2>&1 || { echo "bootstrap failed"; rm -f "$D/attempt"; exit 0; }
ts bootstrap
./nvgsp_native_compute615-41 nvgsp_compute_write32-20261114.nak > compute1-$BT.json 2>&1
ts compute1
./nvgsp_native_compute615-41 nvgsp_compute_write32-20261114.nak > compute2-$BT.json 2>&1
echo "bootstrap+compute done after $(( $(date +%s) - T0 )) s"
ts compute2
./accelgo-mtl1 > accelgo-$BT.json 2>&1
ts accelgo
rm -f "$D/attempt"; sync
python3 -c "
import plistlib,subprocess
p=plistlib.loads(subprocess.check_output(['ioreg','-a','-r','-c','NVGspControl']))[0]
print('ready',p.get('NVGspControl-native615-ready'),'rc',p.get('NVGspControl-rc-count'))"
echo "autowarm done $(date) elapsed $(( $(date +%s) - T0 )) s"
