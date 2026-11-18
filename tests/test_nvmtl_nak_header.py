#!/usr/bin/env python3
"""Host test for the untrusted .nak header bounds check.

The compiler writes and re-reads .nak files from /var/tmp. A corrupt or
truncated file used to reach -[NSData subdataWithRange:] with a header size
larger than the file, which raises an Objective-C exception. nvNakLayoutOK is
pure C so this test compiles and executes the exact helper used by
NVMTLCompiler.m.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "drivers/NVMTLDriver/NVMTLCompiler.m"

PREFIX = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
typedef uint64_t NSUInteger;
'''

SUFFIX = r'''
static int failures;
static void check(const char *what, int got, int want) {
    if (got != want) { fprintf(stderr, "FAIL %s got %d want %d\n", what, got, want); failures++; }
}
int main(void) {
    check("header only", nvNakLayoutOK(64, 0, 0), 1);
    check("code fits", nvNakLayoutOK(68, 4, 0), 1);
    check("code and sph fit", nvNakLayoutOK(72, 4, 4), 1);
    check("short file", nvNakLayoutOK(63, 0, 0), 0);
    check("code past end", nvNakLayoutOK(64, 1, 0), 0);
    check("sph past end", nvNakLayoutOK(64, 0, 1), 0);
    check("sph and code past end", nvNakLayoutOK(71, 4, 4), 0);
    check("huge sph", nvNakLayoutOK(128, 0, UINT64_MAX), 0);
    check("huge code", nvNakLayoutOK(128, UINT64_MAX, 0), 0);
    if (failures) return 1;
    puts("nak header bounds PASS");
    return 0;
}
'''


def extract(source, name):
    start = source.index(name)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


class NakHeaderTest(unittest.TestCase):
    def test_layout_helper(self):
        source = COMPILER.read_text(encoding="utf-8")
        code = PREFIX + extract(source, "static bool nvNakLayoutOK") + "\n" + SUFFIX
        cc = os.environ.get("CC") or ("clang" if os.name == "nt" else "cc")
        with tempfile.TemporaryDirectory(prefix="nvmtl-nak-") as tmp:
            p = Path(tmp) / "check.c"
            p.write_text(code, encoding="utf-8")
            exe = p.with_suffix(".exe" if os.name == "nt" else "")
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(p), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
