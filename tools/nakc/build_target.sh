#!/bin/zsh
# Builds nakc on the target (x86_64) against the NVK Mesa tree in ~/nvk.
#   zsh build_target.sh <nakc sources dir> [out]
# The sources dir holds nakc.c, air_nir.c and tess_gen.c. AIR parsers come
# from the current driver tree, rather than a hand-assembled temporary folder.
set -eu
ROOT=${0:A:h:h:h}
SRC=${1:?sources dir}
SRC=${SRC:A}
OUT=${2:-$HOME/nvbuild/nakc}
OUT=${OUT:A}
M=${MESA_SRC:-$HOME/nvk/mesa-26.0.8}
B=${MESA_BUILD:-$M/build}
AIR=${AIR_SRC:-$ROOT/drivers/nvidia-metal-drivers-macos/NVMTLDriver/air}
mkdir -p ${OUT:h}
OBJDIR=$(mktemp -d ${OUT:h}/.nakc-objects.XXXXXX)
LIBS=(src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a
      src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a
      src/compiler/libcompiler.a src/nouveau/headers/libnvidia_headers_c.a
      src/util/libmesa_util.a src/util/libmesa_util_simd.a
      src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a
      src/compiler/rust/libcompiler_c_helpers.a)
export PATH=$HOME/.cargo/bin:$PATH
cd $B
for library in $LIBS; do test -f $library; done
FLAGS=$(python3 - <<'PY'
import json, shlex
cc = json.load(open('compile_commands.json'))
e = [x for x in cc if x['file'].endswith('nak_nir.c')][0]
print(' '.join(a for a in shlex.split(e['command'])[1:]
               if a.startswith(('-I', '-D', '-std', '-f', '-W')) and not a.startswith('-Werror')))
PY
)
OBJS=()
# find_visible_module.c is a separate executable with its own main.
SOURCES=($SRC/nakc.c $SRC/air_nir.c $SRC/tess_gen.c
         $AIR/metallib.c $AIR/airir.c $AIR/bitstream.c)
for f in $SOURCES; do
  test -f $f
  object=$OBJDIR/$(basename $f .c).o
  cc -O1 ${=FLAGS} -I../src/nouveau/compiler -Isrc/compiler/spirv -I$SRC -I$AIR -c $f -o $object
  OBJS+=$object
done
cc -o $OUT $OBJS $LIBS -lz -lpthread -lc++ -framework CoreFoundation -framework Security
echo "built $OUT"
# the same compiler as a library (main -> nakc_main) for NVMTLDriver to run
# in-process, where sandboxed clients cannot launch nakc
cc -O1 ${=FLAGS} -fvisibility=default -Dmain=nakc_main -I../src/nouveau/compiler -Isrc/compiler/spirv -I$SRC -I$AIR -c $SRC/nakc.c -o $OBJDIR/nakc-lib-main.o
LOBJS=(${OBJS:#*/nakc.o} $OBJDIR/nakc-lib-main.o)
cc -dynamiclib -install_name @rpath/libnakc.dylib -o ${OUT:h}/libnakc.dylib $LOBJS $LIBS -lz -lpthread -lc++ \
   -framework CoreFoundation -framework Security -Wl,-exported_symbol,_nakc_main
codesign -s - -f ${OUT:h}/libnakc.dylib
codesign --verify --strict --verbose=2 ${OUT:h}/libnakc.dylib
shasum -a 256 $OUT ${OUT:h}/libnakc.dylib > ${OUT:h}/nakc-sha256.txt
echo "built ${OUT:h}/libnakc.dylib"
