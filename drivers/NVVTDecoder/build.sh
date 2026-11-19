#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
out=${1:-"$root/build/nvvt"}
mkdir -p "$out"
arch=${ARCH:-x86_64}
cc -arch "$arch" -O2 -Wall -Wextra -Werror -I"$root/drivers/nvidia-metal-drivers-macos/nvdec" -c "$root/drivers/nvidia-metal-drivers-macos/nvdec/h264_parse.c" -o "$out/h264_parse.o"
cc -arch "$arch" -O2 -Wall -Wextra -Werror -I"$root/drivers/nvidia-metal-drivers-macos/nvdec" -c "$root/drivers/nvidia-metal-drivers-macos/nvdec/nvdec_h264.c" -o "$out/nvdec_h264.o"
for f in hevc_parse nvdec_hevc; do cc -arch "$arch" -O2 -Wall -Wextra -Werror -I"$root/drivers/nvidia-metal-drivers-macos/nvdec" -c "$root/drivers/nvidia-metal-drivers-macos/nvdec/$f.c" -o "$out/$f.o"; done
c++ -arch "$arch" -std=c++17 -O2 -Wall -Wextra -Werror -dynamiclib \
    -I"$root/drivers/nvidia-metal-drivers-macos/nvdec" "$root/drivers/nvidia-metal-drivers-macos/NVVTDecoder/NVVTDecoder.cpp" \
    "$out/h264_parse.o" "$out/nvdec_h264.o" "$out/hevc_parse.o" "$out/nvdec_hevc.o" \
    -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
    -framework CoreFoundation -framework IOKit -install_name @rpath/libNVVTDecoder.dylib \
    -o "$out/libNVVTDecoder.dylib"
# plug-in bundle for /Library/Video/Plug-Ins (same code, VT loads it itself)
b="$out/NVVTDecoder.bundle/Contents"
mkdir -p "$b/MacOS"
cp "$root/drivers/nvidia-metal-drivers-macos/NVVTDecoder/Info.plist" "$b/Info.plist"
c++ -arch "$arch" -std=c++17 -O2 -Wall -Wextra -Werror -bundle \
    -I"$root/drivers/nvidia-metal-drivers-macos/nvdec" "$root/drivers/nvidia-metal-drivers-macos/NVVTDecoder/NVVTDecoder.cpp" \
    "$out/h264_parse.o" "$out/nvdec_h264.o" "$out/hevc_parse.o" "$out/nvdec_hevc.o" \
    -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
    -framework CoreFoundation -framework IOKit -o "$b/MacOS/NVVTDecoder"
if [ -f "$root/tools/videotoolbox/vt_decode_test.cpp" ]; then
    c++ -arch "$arch" -std=c++17 -O2 -Wall -Wextra -Werror \
        -I"$root/drivers/nvidia-metal-drivers-macos/nvdec" -I"$root/drivers/nvidia-metal-drivers-macos/NVVTDecoder" \
        "$root/tools/videotoolbox/vt_decode_test.cpp" "$out/h264_parse.o" "$out/nvdec_h264.o" "$out/hevc_parse.o" "$out/nvdec_hevc.o" \
        -L"$out" -lNVVTDecoder -Wl,-rpath,@loader_path \
        -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation \
        -o "$out/vt_decode_test"
fi
