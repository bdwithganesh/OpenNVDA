#!/bin/sh
# Boot-time native615 display bring-up (LaunchDaemon org.local.nvdisp615).
LOG=/Library/NativeDriverLab/display/boot-$(date +%s).log
exec >>"$LOG" 2>&1
ts() { perl -MTime::HiRes=clock_gettime,CLOCK_MONOTONIC -e 'printf "%s at uptime %.2f s\n", $ARGV[0], clock_gettime(CLOCK_MONOTONIC)' "$1"; }
echo "nvdisp615 start $(date)"
ts start
n=0
until /usr/sbin/ioreg -r -c NVGspControl -l | grep -q 'native615-ready" = Yes'; do
    n=$((n+1)); [ $n -ge 2400 ] && { echo "runtime not ready after 10 min"; exit 1; }
    sleep 0.25   # 2026-11-19: was 5 s, which left the monitor dark for up to 5 s after GPU ready
done
ts runtime-ready
# 2026-11-19: the first-Metal-client block runs in the background, in parallel with disp_all; disp_all waits for
# the maintenance channel itself, right before it takes the panel from the boot screen.
(
  # 2026-11-16: the maintenance channel (CE copy) is built with the first Metal
  # client, so wait for accelGo (the NVAccelerator service appears only after
  # it) BEFORE vram_test. Running vram_test earlier got a nil MTLDevice and
  # segfaulted, leaving the framebuffer copy dead (black screen).
  n=0
  until /usr/sbin/ioreg -r -c NVAccelerator -d1 | grep -q NVAccelerator; do
      n=$((n+1)); [ $n -ge 720 ] && break
      sleep 0.25
  done
  ts accelerator
  # 2026-11-19: vram_test (a 384 MiB copy benchmark, 4.3 s) was only here to be the first Metal client, which builds
  # the maintenance channel that the CE framebuffer copy needs. mtl_first (device + one blit, 0.6 s) does the same; it is
  # skipped when the channel is already live. The old 2 s settle sleep is gone too.
  if /usr/sbin/ioreg -r -c NVGspControl -l | grep -q '"NVGspControl-mmu615-live" = Yes'; then
    echo "maintenance channel live; vram_test skipped"
  else
    # 2026-11-19: a private allocation opens the client (and the channel) without the 3 s cold shader compile
    sudo -u dhruveen /Users/dhruveen/nvbuild/vramtest/mtl_first open
    ts client-open
  fi
  if /usr/sbin/ioreg -r -c NVGspControl -l | grep -q '"NVGspControl-mmu615-live" = Yes'; then
    echo "maintenance channel live"
  else
    ok=0
    for try in 1 2 3 4 5; do
        out=$(sudo -u dhruveen /Users/dhruveen/nvbuild/vramtest/mtl_first 2>&1); rc=$?
        echo "mtl_first try $try rc $rc: $(echo "$out" | tail -1)"
        [ $rc -eq 0 ] && ok=1 && break
        sleep 5
    done
    [ $ok -eq 1 ] || echo "mtl_first FAILED; framebuffer copy may be off"
    ts first-metal-client
    /usr/sbin/ioreg -r -c NVGspControl -l | grep -oE '"NVGspControl-client615-create-ms" = [0-9]+'
  fi
) &
first=$!
ts disp_all-start
rc=1
for try in 1 2 3 4 5; do
    out=$(/usr/bin/python3 /Users/dhruveen/nvbuild/d615/disp_all.py desktop 2>&1); rc=$?
    echo "$out"
    case "$out" in *"display probe failed"*) echo "probe refused, retry $try"; sleep 10;; *) break;; esac
done
ts head-lit
wait $first
echo "nvdisp615 done $(date) rc $rc"
