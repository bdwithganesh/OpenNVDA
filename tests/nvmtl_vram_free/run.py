#!/usr/bin/env python3
"""Compile the actual nvVramFree function and check every free order.

GPU locks are no-ops here: this checks allocator invariants, not concurrency.
The live Metal coalescing test separately checks real VA reuse and data.
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
candidates = [
    root / "drivers/NVMTLDriver/NVMTLGsp.m",
    root / "drivers/NVMTLDriver/NVMTLGsp.m",
    root / "work/gpu-reset-fix-src/drivers/NVMTLDriver/NVMTLGsp.m",
    root / "work/gpu-reset-fix-src/drivers/NVMTLDriver/NVMTLGsp.m",
]
default = next((p for p in candidates if p.exists()), candidates[0])
source = Path(sys.argv[1]) if len(sys.argv) > 1 else default
text = source.read_text()
start = text.index("void nvVramFree(uint64_t va, uint64_t bytes) {")
end = text.index("\nuint8_t *nvStageCpu", start)
function = text[start:end]
build = root / "work/nvmtl-vram-free-host"
build.mkdir(parents=True, exist_ok=True)
harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t va, size; } NVRange;
static NVRange gVFree[2048];
static unsigned gNVFree;
static int gHeapLock;
static void os_unfair_lock_lock(int *p) { (void)p; }
static void os_unfair_lock_unlock(int *p) { (void)p; }
FUNCTION
static unsigned cases, failed;
static void check_order(const unsigned order[6]) {
    uint64_t starts[6], base=0x2a00000000ULL, total=0;
    memset(gVFree,0,sizeof gVFree); gNVFree=0;
    for (unsigned i=0;i<6;i++) { starts[i]=base+total; total+=(i+1)*65536ULL; }
    for (unsigned k=0;k<6;k++) {
        unsigned i=order[k]; nvVramFree(starts[i],(i+1)*65536ULL);
        uint64_t free_sum=0;
        for (unsigned a=0;a<gNVFree;a++) {
            free_sum+=gVFree[a].size;
            for (unsigned b=a+1;b<gNVFree;b++)
                if (gVFree[a].va<gVFree[b].va+gVFree[b].size &&
                    gVFree[b].va<gVFree[a].va+gVFree[a].size) { failed++; return; }
        }
        uint64_t want=0; for (unsigned j=0;j<=k;j++) want+=(order[j]+1)*65536ULL;
        if (free_sum!=want) { failed++; return; }
    }
    if (gNVFree!=1 || gVFree[0].va!=base || gVFree[0].size!=total) failed++;
    cases++;
}
static void enumerate(unsigned order[6],unsigned k,unsigned mask) {
    if (k==6) { check_order(order); return; }
    for (unsigned i=0;i<6;i++) if (!(mask&(1u<<i))) { order[k]=i; enumerate(order,k+1,mask|(1u<<i)); }
}
int main(void) {
    unsigned order[6]; enumerate(order,0,0);
    printf("actual nvVramFree: %u permutations, %u failed (no lost bytes, no overlap, one final span)\n",cases,failed);
    return failed!=0;
}
'''
(build / "check.c").write_text(harness.replace("FUNCTION", function))
subprocess.run(["clang", "-Wall", "-Werror", "-fsanitize=undefined", str(build / "check.c"), "-o", str(build / "check")], check=True)
raise SystemExit(subprocess.run([str(build / "check")]).returncode)
