#!/bin/zsh
# Builds x86_64 nakc + libnakc.dylib on an Apple Silicon Mac, against the
# cross-built NAK tree (meson --cross-file x86_64-cross.ini -> build-nak-x86).
# Same recipe as build_target.sh, which needs the Mesa tree on the target.
#   zsh build_cross.sh <mesa-26.0.8 dir> <out dir>
set -e
M=${1:?mesa-26.0.8 dir}
OUT=${2:?out dir}
R=$(cd "$(dirname "$0")/../.." && pwd)
B=$M/build-nak-x86
SRC=$(mktemp -d)
cp $R/tools/nakc/*.c $R/tools/nakc/*.h $R/drivers/NVMTLDriver/air/*.[ch] $SRC/
LIBS=(src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a
      src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a
      src/compiler/libcompiler.a src/nouveau/headers/libnvidia_headers_c.a
      src/util/libmesa_util.a src/util/libmesa_util_simd.a
      src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a
      src/compiler/rust/libcompiler_c_helpers.a)
CC=(clang -arch x86_64 -mmacosx-version-min=14.0)
cd $B
FLAGS=$(python3 - <<'PY'
import json, shlex
cc = json.load(open('compile_commands.json'))
e = [x for x in cc if x['file'].endswith('nak_nir.c')][0]
print(' '.join(a for a in shlex.split(e['command'])[1:]
               if a.startswith(('-I', '-D', '-std', '-f', '-W')) and not a.startswith('-Werror')))
PY
)
mkdir -p $OUT
OBJS=()
for f in $SRC/*.c; do
  $CC -O1 ${=FLAGS} -I../src/nouveau/compiler -Isrc/compiler/spirv -I$SRC -c $f -o $SRC/$(basename $f .c).o
  OBJS+=$SRC/$(basename $f .c).o
done
$CC -o $OUT/nakc $OBJS $LIBS -lz -lpthread -lc++ -framework CoreFoundation -framework Security
$CC -O1 ${=FLAGS} -fvisibility=default -Dmain=nakc_main -I../src/nouveau/compiler -Isrc/compiler/spirv -I$SRC \
    -c $SRC/nakc.c -o $SRC/lib-main.o
LOBJS=(${OBJS:#*/nakc.o} $SRC/lib-main.o)
$CC -dynamiclib -install_name @rpath/libnakc.dylib -o $OUT/libnakc.dylib $LOBJS $LIBS -lz -lpthread -lc++ \
    -framework CoreFoundation -framework Security -Wl,-exported_symbol,_nakc_main
codesign -s - -f $OUT/nakc $OUT/libnakc.dylib 2>/dev/null || true
rm -rf $SRC
echo "built $OUT/nakc $OUT/libnakc.dylib"
