#!/bin/sh
# Build NVMTLDriver.bundle (x86_64). Run on the target (or any macOS with Xcode/CLT).
#   sh drivers/nvidia-metal-drivers-macos/NVMTLDriver/build.sh <out dir>
set -eu
D=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$D/build}
# A bundle without its in-process compiler cannot compile GPU pipelines.
# Validate this required input before creating any output bundle.
NAKC_LIB=${NAKC_LIB:-/usr/local/libexec/libnakc.dylib}
if [ ! -f "$NAKC_LIB" ]; then
  echo "required compiler library missing: $NAKC_LIB (build tools/nakc/build_target.sh first)" >&2
  exit 1
fi
lipo "$NAKC_LIB" -verify_arch x86_64
if ! nm -gU "$NAKC_LIB" | awk '$NF == "_nakc_main" { found=1 } END { exit !found }'; then
  echo "compiler library does not export nakc_main: $NAKC_LIB" >&2
  exit 1
fi
B="$OUT/NVMTLDriver.bundle/Contents"
mkdir -p "$B/MacOS"
# MTLIOAccelDevice is not in the SDK's Metal.tbd: resolve it at load time
clang -arch x86_64 -bundle -fobjc-arc -O2 -Wall -Werror \
  -undefined dynamic_lookup -framework Foundation -framework Metal -framework IOKit -framework IOSurface -framework CoreVideo \
  -o "$B/MacOS/NVMTLDriver.new" "$D"/*.m "$D"/air/metallib.c "$D"/air/airir.c "$D"/air/bitstream.c
# Plist goes in only after a good link, so a failed build never looks like the new version.
mv "$B/MacOS/NVMTLDriver.new" "$B/MacOS/NVMTLDriver"
cp "$D/Info.plist" "$B/Info.plist"
# 0.6.2: the in-process compiler (tools/nakc/build_target.sh builds it next to nakc)
mkdir -p "$B/Resources"
cp "$NAKC_LIB" "$B/Resources/libnakc.dylib"
codesign -s - --force "$B/Resources/libnakc.dylib"
codesign -s - --force "$OUT/NVMTLDriver.bundle"
plutil -lint "$B/Info.plist"
codesign --verify --deep --strict --verbose=2 "$OUT/NVMTLDriver.bundle"
echo "built: $OUT/NVMTLDriver.bundle"
