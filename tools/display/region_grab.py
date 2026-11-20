# region_grab.py X Y W H OUT [ystep]: read a rect of the live VRAM scanout (BGRA, live scan-out width) into a PNG
import json, subprocess, struct, sys, zlib
D = '/Users/dhruveen/nvbuild/d615/d615'
def prop(k):
    o = subprocess.run(['/usr/sbin/ioreg', '-l', '-w0', '-r', '-c', 'NVGspControl'], capture_output=True, text=True).stdout
    return next((int(l.split('= ')[1]) for l in o.splitlines() if '"NVGspControl-display615-%s"' % k in l), 0)
# 2026-11-20: the live scan-out (the scaled buffer in a "looks like" mode), else the native front
FULL = prop('scanout-phys') or 0x3dc000000; PITCH = (prop('mode-w') or 3840) * 4
x, y, w, h = (int(v) for v in sys.argv[1:5]); out = sys.argv[5]; ys = int(sys.argv[6]) if len(sys.argv) > 6 else 1
rows = []
for yy in range(y, y + h, ys):
    words = []
    for x0 in range(x, x + w, 960):
        n = min(960, x + w - x0)
        r = json.loads(subprocess.run([D, 'vrd', hex(FULL + yy * PITCH + x0 * 4), str(n)], capture_output=True, text=True).stdout)
        words += [int(v, 16) for v in r['words']]
    rows.append(b''.join(bytes(((v >> 16) & 255, (v >> 8) & 255, v & 255)) for v in words))
raw = b''.join(b'\x00' + r for r in rows)
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, len(rows), 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
print('wrote', out, w, len(rows))
