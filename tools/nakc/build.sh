#!/bin/zsh
# Builds nakc (Mesa NAK SPIR-V -> sm_89 SASS compiler) on the arm64 Mac.
# Prereqs (26 Sep 2026 setup, no Homebrew bottles on macOS 14):
#   rustup (minimal) + rustfmt, cargo install bindgen-cli cbindgen,
#   python venv upstream/mesa-venv: meson ninja pkgconf mako pyyaml packaging cmake,
#   glslang built from source into upstream/tools-bin,
#   libdrm 2.4.125 headers + upstream/pc/libdrm{,_nouveau}.pc (headers only).
set -e
ROOT=${0:A:h:h:h}
UP=$ROOT/upstream
M=$UP/mesa-26.0.8
export PATH=$UP/mesa-venv/bin:$HOME/.cargo/bin:$UP/tools-bin/bin:$PATH
export PKG_CONFIG_PATH=$UP/pc

if [[ ! -d $M ]]; then
  curl -sfL https://archive.mesa3d.org/mesa-26.0.8.tar.xz | tar xJ -C $UP
  (cd $M && patch -p1 < $ROOT/tools/nakc/mesa-26.0.8-macos-nak.patch)
fi
if [[ ! -d $M/build-nak ]]; then
  (cd $M && meson setup build-nak -Dplatforms= -Dglx=disabled -Degl=disabled \
     -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
     -Dgallium-drivers= -Dvulkan-drivers=nouveau -Dllvm=disabled \
     -Dvalgrind=disabled -Dlibunwind=disabled -Dbuildtype=release)
fi
LIBS=(src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a
      src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a
      src/compiler/libcompiler.a src/nouveau/headers/libnvidia_headers_c.a
      src/util/libmesa_util.a src/util/libmesa_util_simd.a
      src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a
      src/compiler/rust/libcompiler_c_helpers.a)
ninja -C $M/build-nak $LIBS
cd $M/build-nak
FLAGS=$(python3 - <<'EOF'
import json, shlex
cc = json.load(open('compile_commands.json'))
e = [x for x in cc if x['file'].endswith('nak_nir.c')][0]
print(' '.join(a for a in shlex.split(e['command'])[1:]
               if a.startswith(('-I', '-D', '-std', '-f', '-W')) and not a.startswith('-Werror')))
EOF
)
cc -O1 ${=FLAGS} -I../src/nouveau/compiler -Isrc/compiler/spirv \
   -c $ROOT/tools/nakc/nakc.c -o nakc.o
cc -o $UP/tools-bin/bin/nakc nakc.o $LIBS -lz -lpthread -lc++ \
   -framework CoreFoundation -framework Security
echo "built $UP/tools-bin/bin/nakc"
