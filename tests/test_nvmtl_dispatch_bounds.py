#!/usr/bin/env python3
"""Execute the real dispatch/index validation helpers from the driver source.

Extracts the pure-C helpers (no ObjC/IOKit) and compiles them against a tiny
mock, so the 30.02/30.11 bounds logic is host-tested instead of reviewed.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / "drivers/NVMTLDriver/NVMTLDevice.m"
M4 = ROOT / "drivers/NVMTLDriver/NVMTL4.m"

PREFIX = r'''
#include <stdio.h>
#include <stdint.h>
typedef int BOOL;
#define NO 0
#define YES 1
typedef uint64_t NSUInteger;
#define NSUIntegerMax ((NSUInteger)-1)
typedef struct { NSUInteger width, height, depth; } MTLSize;
typedef enum { MTLIndexTypeUInt16 = 0, MTLIndexTypeUInt32 = 1 } MTLIndexType;
static MTLSize MTLSizeMake(NSUInteger w, NSUInteger h, NSUInteger d) {
    MTLSize s = {w, h, d}; return s;
}
'''

SUFFIX = r'''
static int failures;
static void check(const char *what, int got, int want) {
    if (got != want) { fprintf(stderr, "FAIL %s got %d want %d\n", what, got, want); failures++; }
}
int main(void) {
    check("64x64 grid 8x8", nvDispatchSizeOK(MTLSizeMake(64,64,1), MTLSizeMake(8,8,1)), 1);
    check("zero grid", nvDispatchSizeOK(MTLSizeMake(0,64,1), MTLSizeMake(8,8,1)), 0);
    check("zero block", nvDispatchSizeOK(MTLSizeMake(64,64,1), MTLSizeMake(0,8,1)), 0);
    check("2048 threads", nvDispatchSizeOK(MTLSizeMake(64,64,1), MTLSizeMake(1024,2,1)), 0);
    check("1025 axis", nvDispatchSizeOK(MTLSizeMake(64,64,1), MTLSizeMake(1025,1,1)), 0);
    check("grid 2^32", nvDispatchSizeOK(MTLSizeMake(0x100000000ULL,1,1), MTLSizeMake(1,1,1)), 0);
    check("grid rounds past 2^32", nvDispatchSizeOK(MTLSizeMake(0xffffffffULL,1,1), MTLSizeMake(2,1,1)), 0);
    check("grid exact max with block 1", nvDispatchSizeOK(MTLSizeMake(0xffffffffULL,1,1), MTLSizeMake(1,1,1)), 1);
    check("1024 threads", nvDispatchSizeOK(MTLSizeMake(64,64,1), MTLSizeMake(1024,1,1)), 1);

    check("m4 size normal", nvM4SizeUsable(MTLSizeMake(4,5,1)), 1);
    check("m4 size zero", nvM4SizeUsable(MTLSizeMake(0,5,1)), 0);
    check("m4 size 2^32", nvM4SizeUsable(MTLSizeMake(0x100000000ULL,1,1)), 0);

    check("3 u16 in 6", nvM4IndexCountFits(3, MTLIndexTypeUInt16, 6), 1);
    check("4 u16 in 6", nvM4IndexCountFits(4, MTLIndexTypeUInt16, 6), 0);
    check("3 u32 in 12", nvM4IndexCountFits(3, MTLIndexTypeUInt32, 12), 1);
    check("3 u32 in 11", nvM4IndexCountFits(3, MTLIndexTypeUInt32, 11), 0);
    check("unknown length allows", nvM4IndexCountFits(300, MTLIndexTypeUInt16, 0), 1);

    check("3 u16 in buffer", nvM4IndexRangeFits(3, MTLIndexTypeUInt16, 6, 10, 0), 1);
    check("declared length too short", nvM4IndexRangeFits(4, MTLIndexTypeUInt16, 6, 10, 0), 0);
    check("offset runs past buffer", nvM4IndexRangeFits(3, MTLIndexTypeUInt16, 6, 10, 8), 0);
    check("offset exactly at end", nvM4IndexRangeFits(1, MTLIndexTypeUInt16, 2, 10, 10), 0);
    check("unknown length uses buffer", nvM4IndexRangeFits(3, MTLIndexTypeUInt16, 0, 6, 0), 1);
    check("unknown length past buffer", nvM4IndexRangeFits(3, MTLIndexTypeUInt16, 0, 5, 0), 0);
    check("count overflow", nvM4IndexRangeFits(NSUIntegerMax, MTLIndexTypeUInt32, 0, 16, 0), 0);
    check("bad index type", nvM4IndexRangeFits(3, (MTLIndexType)7, 0, 16, 0), 0);
    if (failures) return 1;
    puts("dispatch/index validation PASS");
    return 0;
}
'''


def extract(source, name):
    start = source.index(name)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


class DispatchBoundsTest(unittest.TestCase):
    def test_bounds_logic(self):
        device = DEVICE.read_text(encoding="utf-8")
        m4 = M4.read_text(encoding="utf-8")
        code = PREFIX
        code += extract(device, "static BOOL nvDispatchSizeOK")
        code += "\n"
        code += extract(m4, "static BOOL nvM4SizeUsable")
        code += "\n"
        code += extract(m4, "static NSUInteger nvM4IndexSize")
        code += "\n"
        code += extract(m4, "static BOOL nvM4IndexCountFits")
        code += "\n"
        code += extract(m4, "static BOOL nvM4IndexRangeFits")
        code += "\n"
        code += SUFFIX
        cc = os.environ.get("CC") or ("clang" if os.name == "nt" else "cc")
        with tempfile.TemporaryDirectory(prefix="nvmtl-dispatch-") as tmp:
            p = Path(tmp) / "check.c"
            p.write_text(code, encoding="utf-8")
            exe = p.with_suffix(".exe" if os.name == "nt" else "")
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(p), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
