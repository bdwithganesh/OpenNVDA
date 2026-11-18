#!/usr/bin/env python3
"""The GPU kexts must report the same version from kmod_info and Info.plist.

The non-GPU test deliberately excludes the GPU lane; that is how three stale
KMOD_EXPLICIT_DECL strings survived (0.9.6 / 0.1.7 / 0.6.1 while the bundles
said 0.180.11 / 0.5.20 / 0.9.9).
"""
import plistlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GPU_BASE = ROOT / "drivers"
GPU_KEXTS = ["NVGspControl", "NVAccelerator", "NVDisplay"]


class GpuKextVersionTests(unittest.TestCase):
    def test_kmod_version_matches_info_plist(self):
        for name in GPU_KEXTS:
            with self.subTest(kext=name):
                d = GPU_BASE / name
                with open(d / "Info.plist", "rb") as f:
                    p = plistlib.load(f)
                short = p["CFBundleShortVersionString"]
                self.assertEqual(short, p["CFBundleVersion"])
                self.assertRegex(short, r"^\d+\.\d+\.\d+$")
                m = re.search(r'KMOD_EXPLICIT_DECL\(([^,]+),\s*"([^"]+)"', (d / "module.cpp").read_text())
                self.assertIsNotNone(m, "module.cpp has no KMOD_EXPLICIT_DECL")
                self.assertEqual(m.group(1).strip(), p["CFBundleIdentifier"])
                self.assertEqual(m.group(2), short,
                                 "kmod_info version differs from CFBundleShortVersionString")


if __name__ == "__main__":
    unittest.main()
