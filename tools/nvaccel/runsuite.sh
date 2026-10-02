#!/bin/sh
# One test at a time, 30 s each. Abort on any new RC OR reset: rc-count
# alone is reset-local and can return to the baseline during an autoreset.
set -u
mkdir -p "$HOME/nvbuild/logs"
cd "$HOME/nvbuild" || exit 1
L="$HOME/nvbuild/suite.log"
: > "$L"
guard() {
  ioreg -rc NVGspControl -l -w0 | awk '
    /"NVGspControl-rc-count" =/ { rc=$NF }
    /"NVGspControl-gpu-reset-count" =/ { resets=$NF }
    /"NVGspControl-reset-busy" =/ { busy=$NF }
    /"NVGspControl-gr-persistent" =/ { ready=$NF }
    END { print (rc == "" ? 0 : rc), (resets == "" ? 0 : resets), (busy == "" ? "No" : busy), ready }'
}
base=$(guard)
case "$base" in *' No Yes') ;;
  *) echo "ABORT unhealthy/missing GPU state: $base" >> "$L"; exit 1;;
esac
failed=0
for t in "$@"; do
  case "$t" in *[!a-zA-Z0-9_]*) echo "ABORT invalid test name: $t" >> "$L"; exit 2;; esac
  s=$(date +%s)
  # Required inputs are part of the invocation, not usage-only passes.
  case "$t" in
    metal_3d_test) set -- gfx.metallib;;
    metal_air_test) set -- ak.metallib;;
    metal_linked_test) set -- vf.metallib vfimpl.metallib;;
    metal_stagein_test) set -- si.metallib;;
    *) set --;;
  esac
  perl -e 'alarm 30; exec @ARGV; die "exec failed: $!"' "./$t" "$@" > "$HOME/nvbuild/logs/$t.out" 2>&1
  rc=$?
  e=$(( $(date +%s) - s ))
  last=$(grep -E 'PASS|FAIL|error|Error' "$HOME/nvbuild/logs/$t.out" | tail -1 | cut -c1-120)
  echo "$t rc=$rc ${e}s :: $last" >> "$L"
  [ "$rc" -eq 0 ] || failed=1
  n=$(guard)
  if [ "$n" != "$base" ]; then
    echo "ABORT GPU state changed after $t: before=$base after=$n" >> "$L"
    exit 1
  fi
done
echo "DONE failures=$failed" >> "$L"
exit "$failed"
