#!/bin/sh
# Regression for the MPS visible-function auto-link (NVMTLDriver 0.8.87).
# Proves that prefixPrimary_f, which is not an MTLFunction, still links from
# the module that defines it and removes nakc's "reads as zero" notes.
# Needs a cached MPSNDArray metallib and the extracted module; skips cleanly
# when this machine does not have them.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAKC=${NAKC:-/usr/local/libexec/nakc}
LIB=${MPS_METALLIB:-/var/tmp/nvmtl-cache/5862d05640b133f598413eb1f19bc561.metallib}
MOD=${PREFIX_MODULE:-/var/tmp/nvmtl-cache/247b95d27da2d275f23fa287cbaead59.air}
FN=${FUNCTION:-ndArrayPooling_1dpass_vertical_tmpOut}
[ -x "$NAKC" ] || { echo "SKIP: no nakc at $NAKC"; exit 0; }
[ -f "$LIB" ] || { echo "SKIP: no MPS metallib at $LIB (cache cleaned)"; exit 0; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
if [ ! -f "$MOD" ]; then
    # The app-supplied visible-function module is cached by nvAirFor; reuse it
    # first (the MPS default metallibs do not expose prefixPrimary_f).
    for f in /var/tmp/nvmtl-cache/*.air; do
        [ -f "$f" ] || continue
        if grep -aq "prefixPrimary_f" "$f"; then MOD="$f"; break; fi
    done
fi
if [ ! -f "$MOD" ]; then
    cc -O2 -I "$ROOT/drivers/nvidia-metal-drivers-macos/NVMTLDriver/air"         "$ROOT/tools/nakc/find_visible_module.c"         "$ROOT/drivers/nvidia-metal-drivers-macos/NVMTLDriver/air/metallib.c"         "$ROOT/drivers/nvidia-metal-drivers-macos/NVMTLDriver/air/airir.c"         "$ROOT/drivers/nvidia-metal-drivers-macos/NVMTLDriver/air/bitstream.c"         -o "$T/find_visible_module" || { echo "SKIP: could not build finder"; exit 0; }
    "$T/find_visible_module" "$LIB" prefixPrimary_f "$T/prefix.air" || { echo "SKIP: prefixPrimary_f not in $LIB or cache"; exit 0; }
    MOD="$T/prefix.air"
fi
"$NAKC" --air "$LIB" "$FN" "$T/no.nak" 16 16 1 tg0=2176 >"$T/no.log" 2>&1
"$NAKC" --air "$LIB" "$FN" "$T/yes.nak" 16 16 1 tg0=2176 \
    "link=prefixPrimary_f@$MOD" >"$T/yes.log" 2>&1
no=$(grep -c 'is not linked' "$T/no.log" || true)
yes=$(grep -c 'is not linked' "$T/yes.log" || true)
echo "unlinked notes=$no linked notes=$yes"
[ "$no" -gt 0 ] && [ "$yes" -eq 0 ] || { echo "FAIL: auto-link did not remove notes"; exit 1; }
echo "PASS: prefixPrimary_f module links and removes the zero-call fallback"
