#!/usr/bin/env python3
"""KDK symbolizer - Sonoma 14.8.9 (23J631) target panics/spins ke liye.

KDK: /Library/Developer/KDKs/KDK_14.8.9_23J631.kdk (read-only use, copy nahi)
- kernel UUID 3CF18F50-FBD8-36B9-8EB8-70F566D2934A (x86_64), version string target-identical:
  Darwin 23.6.0 xnu-10063.141.1.713.39~1/RELEASE_X86_64
- IOGraphicsFamily.kext + .dSYM present (S3 wedge path ke symbols resolve hote hain)

Usage:
  python3 kdk_symbols.py sym <kext-or-kernel> <hexaddr>...   # addr -> symbol (file:line)
  python3 kdk_symbols.py nm <kext-or-kernel> <substring>     # search a symbol -> offsets
  python3 kdk_symbols.py --selftest

Panic PCs ke liye load address chahiye (kextstat slide ya panic-log ra). File offsets
(nm wale) seedha atos me `-l 0x0` ke saath chalte hain.
"""
import subprocess
import sys

KDK = "/Library/Developer/KDKs/KDK_14.8.9_23J631.kdk"
BIN = {
    "kernel": f"{KDK}/System/Library/Kernels/kernel.dSYM/Contents/Resources/DWARF/kernel",
    "IOGraphicsFamily": f"{KDK}/System/Library/Extensions/IOGraphicsFamily.kext.dSYM/Contents/Resources/DWARF/IOGraphicsFamily",
}
RAW = {
    "IOGraphicsFamily": f"{KDK}/System/Library/Extensions/IOGraphicsFamily.kext/IOGraphicsFamily",
}


def atos(which: str, addrs: list, load="0x0") -> list:
    dsym = BIN[which]
    a = ["xcrun", "atos", "-o", dsym, "-arch", "x86_64", "-l", load] + addrs
    r = subprocess.run(a, capture_output=True, text=True, timeout=120)
    return r.stdout.strip().splitlines()


def nm_search(which: str, sub: str) -> list:
    r = subprocess.run(["nm", "-a", RAW[which]], capture_output=True,
                       text=True, timeout=120)
    return [l for l in r.stdout.splitlines() if sub.lower() in l.lower()]


def selftest() -> int:
    fails = 0

    def ck(c, m):
        nonlocal fails
        print(("PASS " if c else "FAIL ") + m)
        if not c:
            fails += 1

    out = atos("IOGraphicsFamily", ["0x17578", "0xa14a", "0x1a386"])
    ck(any("handleEvent" in l and "IOFramebuffer.cpp" in l for l in out), f"handleEvent -> {out[0] if out else '?'}")
    ck(any("sleepGate" in l for l in out), f"sleepGate -> {out[1] if len(out) > 1 else '?'}")
    ck(any("extAcknowledgeNotification" in l for l in out), "extAckNotification resolves")
    hits = nm_search("IOGraphicsFamily", "sleepGate")
    ck(any("000000000000a14a" in h for h in hits), "nm sleepGate @ a14a")
    print("SELFTEST", "OK" if fails == 0 else f"{fails} FAILURES")
    return fails


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(1 if selftest() else 0)
    if len(sys.argv) < 3 or sys.argv[1] not in ("sym", "nm"):
        print(__doc__)
        sys.exit(2)
    if sys.argv[1] == "sym":
        print("\n".join(atos(sys.argv[2], sys.argv[3:])))
    else:
        print("\n".join(nm_search(sys.argv[2], sys.argv[3])))
