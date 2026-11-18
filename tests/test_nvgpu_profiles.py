#!/usr/bin/env python3
"""Compile/execute the NVIDIA profile registry truth table."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "drivers/NVGspControl/NVGPUProfiles.h"

HARNESS = r'''
#include <cstdio>
#include "NVGPUProfiles.h"
static int fails;
static void ck(const char *n, bool got, bool want) {
    if (got != want) { std::fprintf(stderr, "FAIL %s got %d want %d\n", n, got, want); ++fails; }
}
int main() {
    ck("4080 supported", nvGPUProfileSupported(0x2704), true);
    ck("4090 recognized", nvGPUProfileFor(0x2684) != nullptr, true);
    ck("4090 not supported", nvGPUProfileSupported(0x2684), false);
    ck("3090 not supported", nvGPUProfileSupported(0x2204), false);
    ck("3070 not supported", nvGPUProfileSupported(0x2484), false);
    ck("2080Ti not supported", nvGPUProfileSupported(0x1e04), false);
    ck("unknown rejected", nvGPUProfileFor(0xffff) == nullptr, true);
    ck("family AD103", std::string(nvGPUFamilyName(0x2704)) == "AD103", true);
    ck("family unknown", std::string(nvGPUFamilyName(0xffff)) == "unknown", true);
    if (fails) return 1;
    std::puts("NVIDIA profile registry PASS");
    return 0;
}
'''


class ProfilesTest(unittest.TestCase):
    def test_registry(self):
        cxx = os.environ.get("CXX") or ("clang++" if os.name == "nt" else "c++")
        with tempfile.TemporaryDirectory(prefix="nvgpu-profile-") as tmp:
            p = Path(tmp)
            (p / "NVGPUProfiles.h").write_text(HEADER.read_text(encoding="utf-8"), encoding="utf-8")
            (p / "check.cpp").write_text("#include <string>\n" + HARNESS, encoding="utf-8")
            exe = p / ("check.exe" if os.name == "nt" else "check")
            subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(p / "check.cpp"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
