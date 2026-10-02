#!/bin/sh
# Compile only. No signing, installation, injection or loading.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KEXT=${1:-LabPCIProbe}
SDK=${2:-"$ROOT/upstream-MacKernelSDK"}
EMBED=${3:-}
if [ "$(uname -s)" != Darwin ]; then
  echo 'macOS build host required' >&2
  exit 1
fi
test -f "$SDK/Headers/IOKit/pci/IOPCIDevice.h"
test -f "$SDK/Library/x86_64/libkmod.a"
OUT="$ROOT/build/kext-$KEXT-$(date -u +%Y%m%dT%H%M%SZ)-$$"
BUNDLE="$OUT/$KEXT.kext/Contents"
mkdir -p "$BUNDLE/MacOS"
git -C "$SDK" rev-parse HEAD > "$OUT/sdk-commit.txt"
xcrun clang --version > "$OUT/compiler.txt"
UNITS=$(cd "$ROOT/drivers/$KEXT" && ls *.cpp | sed "s/\.cpp$//")
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
KVER=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$ROOT/drivers/$KEXT/Info.plist" 2>/dev/null || echo unknown)
CXX_EXTRA="$CXX_EXTRA -DKEXT_BUNDLE_VERSION=\"$KVER\""
# Per-kext vendored SDK headers (drivers/<Kext>/sdk), e.g. ZenSMC's Lilu/VirtualSMC SDK.
KEXT_INC=
[ -d "$ROOT/drivers/$KEXT/sdk" ] && KEXT_INC="-isystem $ROOT/drivers/$KEXT/sdk"
for UNIT in $UNITS; do
  xcrun clang++ -arch x86_64 -mmacosx-version-min=10.15 -std=gnu++14 \
    -O2 -fstack-usage \
    -mkernel -fapple-kext -fno-exceptions -fno-rtti -fno-builtin \
    -fno-stack-protector -mno-red-zone -nostdinc -DKERNEL -DKERNEL_PRIVATE \
    -I "$SDK/Headers" -I "$ROOT" $KEXT_INC -Wall -Wextra -Werror $CXX_EXTRA \
    -c "$ROOT/drivers/$KEXT/$UNIT.cpp" -o "$OUT/$UNIT.o" \
    > "$OUT/compile-$UNIT.log" 2>&1 || {
      cat "$OUT/compile-$UNIT.log" >&2
      exit 1
    }
done
if [ "$KEXT" = NVGspControl ]; then
  POLL_STACK=$(awk -F '\t' '$1 ~ /_ZN12NVGspControl16pollStatusLockedEv$/ { print $2 }' \
    "$OUT/NVGspControl.su")
  if [ -z "$POLL_STACK" ] || [ "$POLL_STACK" -ge 8192 ]; then
    echo "NVGspControl::pollStatusLocked stack frame is unsafe: ${POLL_STACK:-unknown} bytes" >&2
    exit 1
  fi
  printf 'NVGspControl::pollStatusLocked stack frame: %s bytes (<8192 required)\n' \
    "$POLL_STACK" > "$OUT/stack-usage-check.txt"
fi
xcrun ld -arch x86_64 -kext -undefined dynamic_lookup \
  -o "$BUNDLE/MacOS/$KEXT" $(for U in $UNITS; do printf '%s ' "$OUT/$U.o"; done) $EXTRA_OBJECT \
  "$SDK/Library/x86_64/libkmod.a" > "$OUT/link.log" 2>&1 || {
    cat "$OUT/link.log" >&2
    exit 1
  }
cp "$ROOT/drivers/$KEXT/Info.plist" "$BUNDLE/Info.plist"
plutil -lint "$BUNDLE/Info.plist" > "$OUT/plist-validation.txt"
xcrun otool -hv "$BUNDLE/MacOS/$KEXT" > "$OUT/macho-header.txt"
xcrun nm -u "$BUNDLE/MacOS/$KEXT" > "$OUT/unresolved-kernel-symbols.txt"
shasum -a 256 "$BUNDLE/MacOS/$KEXT" > "$OUT/sha256.txt"
printf 'Compiled diagnostic kext (NOT loaded or runtime-validated): %s\n' "$OUT"
