#!/bin/sh
# Hardware-free checks: the real driver/core code against mocks and recorded GSP 615.78.08 replies.
# Nothing is installed or loaded. Checks that parse NVIDIA firmware images (boot, booter, falcon,
# fwsec, package, vbios) need your own copy of those files and are skipped here.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
OUT=work/host-checks; F=tests/fixtures
mkdir -p "$OUT"
fail=0
args() {
    case "$1" in
        nvgsp_client_gr_615_check|nvgsp_gr_blob_615_check) echo "$F/gr-context-info-ad103-615.78.08.bin" ;;
        nvgsp_compute_packet_615_check) echo "$F/compute-qmd-ad103-615.78.08.bin $F/compute-write32-sm89-20261114.nak" ;;
        nvgsp_fault_615_check) echo "$F/gmc-mmu-fault-notice-615.78.08.bin" ;;
        nvgsp_owned_recovery_615_check) echo "$F/cross-vas-pde-fault-615.78.08-60.bin" ;;
    esac
}
for src in tests/*_check.cpp; do
    name=$(basename "$src" .cpp)
    case "$name" in
        nvgsp_boot_check|nvgsp_booter_check|nvgsp_booter_falcon_check|nvgsp_falcon_check|\
        nvgsp_fwsec_check|nvgsp_package_check|nvgsp_vbios_fwsec_check) echo "$name: SKIP (needs NVIDIA firmware)"; continue ;;
        nvgsp_owned_recovery_615_check) echo "$name: SKIP (needs a read-only fault record capture)"; continue ;;
    esac
    # shellcheck disable=SC2046
    if "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-deprecated-declarations -Itests "$src" -o "$OUT/$name" \
           > "$OUT/$name.log" 2>&1 && "$OUT/$name" $(args "$name") >> "$OUT/$name.log" 2>&1; then
        echo "$name: PASS"
    else
        echo "$name: FAIL"; tail -20 "$OUT/$name.log"; fail=1
    fi
done
for t in tests/test_*.py; do
    if python3 "$t" > "$OUT/$(basename "$t").log" 2>&1; then echo "$(basename "$t" .py): PASS"
    else echo "$(basename "$t" .py): FAIL"; tail -20 "$OUT/$(basename "$t").log"; fail=1; fi
done
exit $fail
