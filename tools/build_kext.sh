#!/bin/sh
# Compile only. No signing, installation, injection or loading.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KEXT_ARG=${1:-LabPCIProbe}
SDK=${2:-"$ROOT/upstream-MacKernelSDK"}
EMBED=${3:-}
# A kext may be named directly (the old layout) or live under
# drivers/non-nvidia/<category>/. Resolve by relative path first, then by
# basename so existing build commands keep working.
KEXT_DIR=
if [ -d "$ROOT/drivers/$KEXT_ARG" ]; then
  KEXT_DIR="$ROOT/drivers/$KEXT_ARG"
elif [ -d "$ROOT/drivers/nvidia-metal-drivers-macos/$KEXT_ARG" ]; then
  KEXT_DIR="$ROOT/drivers/nvidia-metal-drivers-macos/$KEXT_ARG"
elif [ -d "$ROOT/drivers/non-nvidia/$KEXT_ARG" ]; then
  KEXT_DIR="$ROOT/drivers/non-nvidia/$KEXT_ARG"
else
  KEXT_NAME=$(basename "$KEXT_ARG")
  KEXT_DIR=$(find "$ROOT/drivers" -type d -name "$KEXT_NAME" 2>/dev/null | head -n 1)
fi
if [ -z "$KEXT_DIR" ]; then
  echo "kext not found: $KEXT_ARG" >&2
  exit 1
fi
KEXT_NAME=$(basename "$KEXT_DIR")
if [ "$(uname -s)" != Darwin ]; then
  echo 'macOS build host required' >&2
  exit 1
fi
test -f "$SDK/Headers/IOKit/pci/IOPCIDevice.h"
test -f "$SDK/Library/x86_64/libkmod.a"
OUT="$ROOT/build/kext-$KEXT_NAME-$(date -u +%Y%m%dT%H%M%SZ)-$$"
BUNDLE="$OUT/$KEXT_NAME.kext/Contents"
mkdir -p "$BUNDLE/MacOS"
git -C "$SDK" rev-parse HEAD > "$OUT/sdk-commit.txt"
xcrun clang --version > "$OUT/compiler.txt"
UNITS=$(cd "$KEXT_DIR" && ls *.cpp | sed "s/\.cpp$//")
CXX_EXTRA=
EXTRA_OBJECT=
if [ -n "$EMBED" ]; then
  case "$EMBED" in /*) ;; *) EMBED="$ROOT/$EMBED" ;; esac
  test -f "$EMBED"
  CXX_EXTRA=-DNVGSP_EMBEDDED_PACKAGE
  cat > "$OUT/embedded.S" <<EOF
.section __DATA,__nvgspfw,regular
.p2align 12
.globl _nvgsp_embedded_start
_nvgsp_embedded_start:
.incbin "$EMBED"
.globl _nvgsp_embedded_end
_nvgsp_embedded_end:
EOF
  xcrun clang -arch x86_64 -mmacosx-version-min=10.15 -c "$OUT/embedded.S" -o "$OUT/embedded.o"
  EXTRA_OBJECT="$OUT/embedded.o"
fi
# The bundle version as KEXT_BUNDLE_VERSION, so registry version properties follow Info.plist
KVER=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$KEXT_DIR/Info.plist" 2>/dev/null || echo unknown)
CXX_EXTRA="$CXX_EXTRA -DKEXT_BUNDLE_VERSION=\"$KVER\""
# Per-kext vendored SDK headers ($KEXT_DIR/sdk), e.g. ZenSMC's Lilu/VirtualSMC SDK.
KEXT_INC=
[ -d "$KEXT_DIR/sdk" ] && KEXT_INC="-isystem $KEXT_DIR/sdk"
for UNIT in $UNITS; do
  xcrun clang++ -arch x86_64 -mmacosx-version-min=10.15 -std=gnu++14 \
    -O2 -fstack-usage \
    -mkernel -fapple-kext -fno-exceptions -fno-rtti -fno-builtin \
    -fno-stack-protector -fno-stack-check -mno-red-zone -nostdinc -DKERNEL -DKERNEL_PRIVATE \
    -I "$SDK/Headers" -I "$ROOT" $KEXT_INC -Wall -Wextra -Werror $CXX_EXTRA \
    -c "$KEXT_DIR/$UNIT.cpp" -o "$OUT/$UNIT.o" \
    > "$OUT/compile-$UNIT.log" 2>&1 || {
      cat "$OUT/compile-$UNIT.log" >&2
      exit 1
    }
done
if [ "$KEXT_NAME" = NVGspControl ]; then
  POLL_STACK=$(awk -F '\t' '$1 ~ /_ZN12NVGspControl16pollStatusLockedEv$/ { print $2 }' \
    "$OUT/NVGspControl.su")
  if [ -z "$POLL_STACK" ] || [ "$POLL_STACK" -ge 8192 ]; then
    echo "NVGspControl::pollStatusLocked stack frame is unsafe: ${POLL_STACK:-unknown} bytes" >&2
    exit 1
  fi
  printf 'NVGspControl::pollStatusLocked stack frame: %s bytes (<8192 required)\n' \
    "$POLL_STACK" > "$OUT/stack-usage-check.txt"
  NATIVE_STACK=$(awk -F '\t' '$1 ~ /_ZN12NVGspControl19initializeNative615Ev$/ { print $2 }' \
    "$OUT/NVGspControl.su")
  RPC_STACK=$(awk -F '\t' '$1 ~ /_ZN12NVGspControl11userRpcImpl/ { print $2 }' \
    "$OUT/NVGspControl.su" | head -n 1)
  if [ -z "$NATIVE_STACK" ] || [ "$NATIVE_STACK" -ge 2048 ] || \
     [ -z "$RPC_STACK" ] || [ "$RPC_STACK" -ge 1024 ] || \
     [ $((POLL_STACK + NATIVE_STACK + RPC_STACK + 1024)) -ge 12288 ]; then
    echo "Native615 bootstrap/RPC stack budget exceeded: ${NATIVE_STACK:-unknown}/${RPC_STACK:-unknown}" >&2
    exit 1
  fi
  printf 'Native615 bootstrap frame: %s bytes (<2048); RPC frame: %s bytes (<1024); bootstrap+RPC+poll+1024 helper allowance: %s bytes (<12288)\n' \
    "$NATIVE_STACK" "$RPC_STACK" "$((POLL_STACK + NATIVE_STACK + RPC_STACK + 1024))" \
    >> "$OUT/stack-usage-check.txt"
  COMPUTE_STACK=$(awk -F '\t' '$1 ~ /_ZN12NVGspControl16nativeCompute615/ { print $2 }' \
    "$OUT/NVGspControl.su" | head -n 1)
  if [ -z "$COMPUTE_STACK" ] || [ "$COMPUTE_STACK" -ge 2048 ]; then
    echo "Native615 compute stack budget exceeded: ${COMPUTE_STACK:-unknown}" >&2
    exit 1
  fi
  printf 'Native615 compute frame: %s bytes (<2048)\n' "$COMPUTE_STACK" >> "$OUT/stack-usage-check.txt"
  ISOLATION_STACK=$(awk -F '\t' '$1 ~ /_ZN12NVGspControl15runIsolation615Ev$/ { print $2 }' \
    "$OUT/NVGspControl.su")
  if [ -z "$ISOLATION_STACK" ] || [ "$ISOLATION_STACK" -ge 2048 ] || \
     [ $((POLL_STACK + ISOLATION_STACK + RPC_STACK + 1024)) -ge 12288 ]; then
    echo "Native615 isolation stack budget exceeded: ${ISOLATION_STACK:-unknown}" >&2
    exit 1
  fi
  printf 'Native615 isolation frame: %s bytes (<2048); isolation+RPC+poll+1024 helper allowance: %s bytes (<12288)\n' \
    "$ISOLATION_STACK" "$((POLL_STACK + ISOLATION_STACK + RPC_STACK + 1024))" >> "$OUT/stack-usage-check.txt"
  RECOVERY_STACK=$(awk -F '\t' '$1 ~ /_ZN12NVGspControl20recoverOwnedFault615Ev$/ { print $2 }' "$OUT/NVGspControl.su")
  if [ -z "$RECOVERY_STACK" ] || [ "$RECOVERY_STACK" -ge 2048 ] || \
     [ $((POLL_STACK + RECOVERY_STACK + RPC_STACK + 1024)) -ge 12288 ]; then
    echo "Native615 recovery stack budget exceeded: ${RECOVERY_STACK:-unknown}" >&2
    exit 1
  fi
  printf 'Native615 recovery frame: %s bytes (<2048); recovery+RPC+poll+1024 helper allowance: %s bytes (<12288)\n' \
    "$RECOVERY_STACK" "$((POLL_STACK + RECOVERY_STACK + RPC_STACK + 1024))" >> "$OUT/stack-usage-check.txt"
  CLIENT_STACK=$(awk -F '\t' '$1 ~ /_ZN12NVGspControl14runClientGr615E[a-z]$/ { print $2 }' "$OUT/NVGspControl.su")
  if [ -z "$CLIENT_STACK" ] || [ "$CLIENT_STACK" -ge 2048 ] || \
     [ $((POLL_STACK + CLIENT_STACK + RPC_STACK + 1024)) -ge 12288 ]; then
    echo "Native615 client GR stack budget exceeded: ${CLIENT_STACK:-unknown}" >&2
    exit 1
  fi
  printf 'Native615 client GR frame: %s bytes (<2048); client+RPC+poll+1024 helper allowance: %s bytes (<12288)\n' \
    "$CLIENT_STACK" "$((POLL_STACK + CLIENT_STACK + RPC_STACK + 1024))" >> "$OUT/stack-usage-check.txt"
fi
xcrun ld -arch x86_64 -kext -undefined dynamic_lookup \
  -o "$BUNDLE/MacOS/$KEXT_NAME" $(for U in $UNITS; do printf '%s ' "$OUT/$U.o"; done) $EXTRA_OBJECT \
  "$SDK/Library/x86_64/libkmod.a" > "$OUT/link.log" 2>&1 || {
    cat "$OUT/link.log" >&2
    exit 1
  }
cp "$KEXT_DIR/Info.plist" "$BUNDLE/Info.plist"
plutil -lint "$BUNDLE/Info.plist" > "$OUT/plist-validation.txt"
xcrun otool -hv "$BUNDLE/MacOS/$KEXT_NAME" > "$OUT/macho-header.txt"
xcrun nm -u "$BUNDLE/MacOS/$KEXT_NAME" > "$OUT/unresolved-kernel-symbols.txt"
shasum -a 256 "$BUNDLE/MacOS/$KEXT_NAME" > "$OUT/sha256.txt"
printf 'Compiled diagnostic kext (NOT loaded or runtime-validated): %s\n' "$OUT"
