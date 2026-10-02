#!/usr/bin/env python3
"""Compare ca_compare scenes: M1 reference dir vs RTX dir.
Per scene: mean abs error, % pixels off by > 8, max error; writes
<rtx>/<scene>-diff.png (difference x4) and <rtx>/<scene>-side.png (M1 | RTX).
Uses macOS sips-free CoreGraphics via PyObjC-less route: reads PNG with
the 'png' decoding in Pillow if present, else falls back to /usr/bin/sips + raw.
"""
import os, subprocess, sys, struct, zlib

def read_png(path):
    # minimal PNG reader (8-bit RGBA/RGB, non-interlaced) - what NSBitmapImageRep writes
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and inter == 0, (depth, inter)
            ch = {6: 4, 2: 3}[ctype]
        elif typ == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * ch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, ch, out

def write_png(path, w, h, rgb):
    rows = b''.join(b'\x00' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))

def main():
    ref, dut = sys.argv[1], sys.argv[2]
    worst = 0
    for name in sorted(f for f in os.listdir(ref) if f.endswith('.png') and '-' not in f):
        w, h, ch, a = read_png(os.path.join(ref, name))
        w2, h2, ch2, b = read_png(os.path.join(dut, name))
        if (w, h) != (w2, h2):
            print(f'{name}: size {w}x{h} vs {w2}x{h2}'); continue
        diff = bytearray(w * h * 3); side = bytearray(2 * w * h * 3)
        tot = off = mx = 0
        for i in range(w * h):
            e = 0
            for k in range(3):
                va, vb = a[i * ch + k], b[i * ch2 + k]
                d = abs(va - vb); e = max(e, d); tot += d
                diff[i * 3 + k] = min(255, d * 4)
                y, x = divmod(i, w)
                side[(y * 2 * w + x) * 3 + k] = va
                side[(y * 2 * w + w + x) * 3 + k] = vb
            mx = max(mx, e); off += e > 8
        pct = 100.0 * off / (w * h)
        worst = max(worst, pct)
        print(f'{name[:-4]:8s} mean {tot / (w * h * 3):6.2f}  off>8 {pct:6.2f}%  max {mx}')
        write_png(os.path.join(dut, name[:-4] + '-diff.png'), w, h, diff)
        write_png(os.path.join(dut, name[:-4] + '-side.png'), 2 * w, h, side)
    return 0

if __name__ == '__main__':
    sys.exit(main())
