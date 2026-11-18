#!/usr/bin/env python3
"""Execute the exact Metal family-advertisement policy used by NVMTLDriver.

The driver's supportsFamily: delegates to NVMTLFamilyGate.h. This test compiles
that header with the host C compiler and checks the full truth table, including
the important rule that a per-process NVMTL_METAL4=1 run does not advertise
Metal 4 to other applications (the family bit needs the conformance receipt).
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "drivers/NVMTLDriver/NVMTLFamilyGate.h"

HARNESS = r'''
#include <stdio.h>
#include "NVMTLFamilyGate.h"

static int failures;
static void expect(const char *what, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", what, got, want);
        failures++;
    }
}

int main(void) {
    NVMTLFamilyGates none = {0, 0, 0};
    NVMTLFamilyGates m3 = {1, 0, 0};
    NVMTLFamilyGates m4proc = {0, 1, 0};
    NVMTLFamilyGates m4full = {0, 1, 1};
    NVMTLFamilyGates m4receiptOnly = {0, 0, 1};

    expect("Mac2 baseline", nvMTLFamilyAllowed(NVMTLFamilyMac2, &none), 1);
    expect("Common1 baseline", nvMTLFamilyAllowed(NVMTLFamilyCommon1, &none), 1);
    expect("Common2 baseline", nvMTLFamilyAllowed(NVMTLFamilyCommon2, &none), 1);
    expect("Common3 baseline", nvMTLFamilyAllowed(NVMTLFamilyCommon3, &none), 1);
    expect("Mac1 baseline (subset of Mac2)", nvMTLFamilyAllowed(NVMTLFamilyMac1, &none), 1);
    expect("Apple9 never", nvMTLFamilyAllowed(NVMTLFamilyApple9, &none), 0);
    expect("Metal3 off by default", nvMTLFamilyAllowed(NVMTLFamilyMetal3, &none), 0);
    expect("Metal3 env opt-in", nvMTLFamilyAllowed(NVMTLFamilyMetal3, &m3), 1);
    expect("Metal3 ignores Metal4 bits", nvMTLFamilyAllowed(NVMTLFamilyMetal3, &m4full), 0);
    expect("Metal4 process-only stays off", nvMTLFamilyAllowed(NVMTLFamilyMetal4, &m4proc), 0);
    expect("Metal4 receipt enables", nvMTLFamilyAllowed(NVMTLFamilyMetal4, &m4full), 1);
    expect("Metal4 receipt without objects stays off",
           nvMTLFamilyAllowed(NVMTLFamilyMetal4, &m4receiptOnly), 0);
    expect("null gates still allow Mac2", nvMTLFamilyAllowed(NVMTLFamilyMac2, 0), 1);
    expect("null gates reject Metal3", nvMTLFamilyAllowed(NVMTLFamilyMetal3, 0), 0);
    expect("name Metal4", nvMTLFamilyName(NVMTLFamilyMetal4)[0] == 'M', 1);
    expect("name unknown", nvMTLFamilyName(42)[0] == 'u', 1);

    if (failures) return 1;
    puts("NVMTL family gate truth table PASS (baseline, Metal3 opt-in, Metal4 receipt)");
    return 0;
}
'''

def main():
    cc = os.environ.get("CC") or ("clang" if os.name == "nt" else "cc")
    with tempfile.TemporaryDirectory(prefix="nvmtl-family-") as tmp:
        p = Path(tmp)
        shutil.copyfile(HEADER, p / HEADER.name)
        (p / "check.c").write_text(HARNESS, encoding="utf-8")
        exe = p / ("check.exe" if os.name == "nt" else "check")
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(p / "check.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


import unittest


class FamilyGatePolicyTest(unittest.TestCase):
    def test_policy(self):
        main()


if __name__ == "__main__":
    main()
