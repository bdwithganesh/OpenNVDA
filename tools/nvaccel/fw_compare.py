#!/usr/bin/env python3
"""Pixel compare of fw_test outputs from two machines (reference = Apple GPU).
   fw_compare.py <reference dir> <test dir>"""
import os, sys
ref, tst = sys.argv[1], sys.argv[2]
worst = 0
for name in sorted(os.listdir(ref)):
    if not name.endswith('.rgba') or not os.path.exists(os.path.join(tst, name)):
        continue
    def load(p):
        d = open(p, 'rb').read(); h, px = d.split(b'\n', 1); w, hh, f = h.split(); return int(w), int(hh), px
    w, h, a = load(os.path.join(ref, name)); w2, h2, b = load(os.path.join(tst, name))
    if (w, h) != (w2, h2) or len(a) != len(b):
        print(f'{name:18} size differs'); worst = 255; continue
    diffs = [abs(x - y) for x, y in zip(a, b)]
    big = sum(1 for i in range(0, len(diffs), 4) if max(diffs[i:i + 3]) > 16)
    mx = max(diffs); mean = sum(diffs) / len(diffs)
    worst = max(worst, mx)
    print(f'{name:18} max {mx:3d}  mean {mean:6.3f}  pixels off by >16: {big} of {w*h}')
sys.exit(0)
