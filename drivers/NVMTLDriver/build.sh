#!/bin/sh
# Build NVMTLDriver.bundle (x86_64). Run on the target (or any macOS with Xcode/CLT).
# sh drivers/NVMTLDriver/build.sh <out dir>
set -e
D=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$D/build}
B="$OUT/NVMTLDriver.bundle/Contents"
mkdir -p "$B/MacOS"
# MTLIOAccelDevice is not in the SDK's Metal.tbd: resolve it at load time
clang -arch x86_64 -bundle -fobjc-arc -O2 -Wall -Werror \
  -undefined dynamic_lookup -framework Foundation -framework Metal -framework IOKit -framework IOSurface -framework CoreVideo \
  -o "$B/MacOS/NVMTLDriver.new" "$D"/*.m
# Plist goes in only after a good link, so a failed build never looks like the new version.
mv "$B/MacOS/NVMTLDriver.new" "$B/MacOS/NVMTLDriver"
cp "$D/Info.plist" "$B/Info.plist"
# the in-process compiler (tools/nakc/build_target.sh builds it next to nakc)
NAKC_LIB=${NAKC_LIB:-/usr/local/libexec/libnakc.dylib}
if [ -f "$NAKC_LIB" ]; then mkdir -p "$B/Resources"; cp "$NAKC_LIB" "$B/Resources/libnakc.dylib"; fi
codesign -s - --force "$OUT/NVMTLDriver.bundle" 2>/dev/null || true
echo "built: $OUT/NVMTLDriver.bundle"
