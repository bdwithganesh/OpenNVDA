#!/bin/zsh
# Builds nakc on the target (x86_64) against the NVK Mesa tree in ~/nvk.
#   zsh build_target.sh <nakc sources dir> [out]
# The sources dir holds nakc.c and the air/ reader (drivers/NVMTLDriver/air).
set -e
SRC=${1:?sources dir}
OUT=${2:-$HOME/nvbuild/nakc}
M=$HOME/nvk/mesa-26.0.8
B=$M/build
LIBS=(src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a
      src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a
      src/compiler/libcompiler.a src/nouveau/headers/libnvidia_headers_c.a
      src/util/libmesa_util.a src/util/libmesa_util_simd.a
      src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a
      src/compiler/rust/libcompiler_c_helpers.a)
export PATH=$HOME/.cargo/bin:$PATH
cd $B
FLAGS=$(python3 - <<'PY'
import json, shlex
cc = json.load(open('compile_commands.json'))
e = [x for x in cc if x['file'].endswith('nak_nir.c')][0]
print(' '.join(a for a in shlex.split(e['command'])[1:]
               if a.startswith(('-I', '-D', '-std', '-f', '-W')) and not a.startswith('-Werror')))
PY
)
OBJS=()
for f in $SRC/*.c; do
  cc -O1 ${=FLAGS} -I../src/nouveau/compiler -Isrc/compiler/spirv -I$SRC -c $f -o /tmp/nakc-$(basename $f .c).o
  OBJS+=/tmp/nakc-$(basename $f .c).o
done
cc -o $OUT $OBJS $LIBS -lz -lpthread -lc++ -framework CoreFoundation -framework Security
echo "built $OUT"
# the same compiler as a library (main -> nakc_main) for NVMTLDriver to run
# in-process, where sandboxed clients cannot launch nakc
cc -O1 ${=FLAGS} -fvisibility=default -Dmain=nakc_main -I../src/nouveau/compiler -Isrc/compiler/spirv -I$SRC -c $SRC/nakc.c -o /tmp/nakc-lib-main.o
LOBJS=(${OBJS:#*/nakc-nakc.o} /tmp/nakc-lib-main.o)
cc -dynamiclib -install_name @rpath/libnakc.dylib -o ${OUT:h}/libnakc.dylib $LOBJS $LIBS -lz -lpthread -lc++ \
   -framework CoreFoundation -framework Security -Wl,-exported_symbol,_nakc_main
codesign -s - -f ${OUT:h}/libnakc.dylib 2>/dev/null || true
echo "built ${OUT:h}/libnakc.dylib"
