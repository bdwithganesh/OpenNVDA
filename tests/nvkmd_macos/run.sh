#!/bin/sh
# Build and run the nvkmd_macos mock-kext host test (Linux; no GPU, no kext).
#   tests/nvkmd_macos/run.sh /path/to/mesa-26.0.8   (mesa-26.0.8-macos-nvk.patch applied)
# The repo copy of drivers/nvk-macos/nvkmd_macos.c is what gets tested, not the
# one inside the Mesa tree. Needs python3 + mako (Vulkan header generators).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
MESA=$(CDPATH= cd -- "${1:?usage: run.sh <patched mesa-26.0.8 tree>}" && pwd)
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/nvkmd-mock.XXXXXX")}
CC=${CC:-cc}
GEN="$OUT/gen"
mkdir -p "$GEN"
XML="$MESA/src/vulkan/registry/vk.xml"
U="$MESA/src/vulkan/util"
python3 "$U/vk_entrypoints_gen.py" --xml "$XML" --proto --weak --prefix nvk --beta false \
  --out-h "$GEN/nvk_entrypoints.h" --out-c "$GEN/nvk_entrypoints.c"
python3 "$U/vk_dispatch_table_gen.py" --xml "$XML" --beta false \
  --out-h "$GEN/vk_dispatch_table.h" --out-c "$GEN/vk_dispatch_table.c"
python3 "$U/vk_extensions_gen.py" --xml "$XML" \
  --out-h "$GEN/vk_extensions.h" --out-c "$GEN/vk_extensions.c"
python3 "$U/vk_physical_device_features_gen.py" --xml "$XML" --beta false \
  --out-h "$GEN/vk_physical_device_features.h" --out-c "$GEN/vk_physical_device_features.c"
python3 "$U/vk_physical_device_properties_gen.py" --xml "$XML" --beta false \
  --out-h "$GEN/vk_physical_device_properties.h" --out-c "$GEN/vk_physical_device_properties.c"
python3 "$U/gen_enum_to_str.py" --xml "$XML" --beta false \
  --out-c "$GEN/vk_enum_to_str.c" --out-h "$GEN/vk_enum_to_str.h" --out-d "$GEN/vk_enum_defines.h"

CFLAGS="-std=gnu11 -g -O1 -Wall -Wno-unused-function -Werror=implicit-function-declaration \
  -Werror=incompatible-pointer-types -Werror=int-conversion \
  -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC -DHAVE_TIMESPEC_GET -D_GNU_SOURCE \
  -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -DPACKAGE_VERSION=\"26.0.8\" \
  -I$ROOT/tests/nvkmd_macos/mock -I$GEN -I$MESA/include -I$MESA/src -I$MESA/src/util \
  -I$MESA/src/vulkan/runtime -I$MESA/src/vulkan/util -I$MESA/src/nouveau/vulkan \
  -I$MESA/src/nouveau -I$MESA/src/nouveau/headers -I$MESA/src/nouveau/headers/nvidia/classes \
  -I$MESA/src/nouveau/winsys -I$MESA/src/compiler -I$MESA/src/compiler/nir \
  -I$MESA/src/nouveau/compiler -I$MESA/src/gallium/include"
OBJS=
for SRC in "$ROOT/drivers/nvk-macos/nvkmd_macos.c" \
           "$ROOT/tests/nvkmd_macos/nvkmd_macos_mock_check.c" \
           "$MESA/src/nouveau/vulkan/nvkmd/nvkmd.c" "$MESA/src/util/vma.c" \
           "$MESA/src/util/simple_mtx.c" "$MESA/src/util/u_call_once.c" \
           "$MESA/src/util/os_time.c" "$MESA/src/c11/impl/threads_posix.c" \
           "$MESA/src/c11/impl/time.c"; do
  O="$OUT/$(basename "$SRC" .c).o"
  # shellcheck disable=SC2086
  $CC $CFLAGS -c "$SRC" -o "$O"
  OBJS="$OBJS $O"
done
# shellcheck disable=SC2086
$CC -o "$OUT/nvkmd_macos_mock_check" $OBJS -lpthread
"$OUT/nvkmd_macos_mock_check"
