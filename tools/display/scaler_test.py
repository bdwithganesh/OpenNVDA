#!/usr/bin/env python3
# Window-scaler feasibility (live head, reverted at the end): window 0 SIZE_OUT 3840x2160 -> 2880x1620 (a 1.333
# downscale, the same ratio as 5120x2880 -> 3840x2160). Double buffering is paused so the kernel does not push
# to the window channel at the same time.
import json, struct, subprocess, sys, time
D = '/Users/dhruveen/nvbuild/d615/d615'
st = json.load(open('/Library/NativeDriverLab/display/state/disp615.json'))
WPB, CPB = st['wnd_pb'], st['core_pb']
def d615(*a):
    out = subprocess.run([D] + [x if isinstance(x, str) else hex(x) for x in a], capture_output=True, text=True).stdout.strip()
    try: return json.loads(out)
    except ValueError: return {'raw': out}
def rd(addr, n=1): return [int(w, 16) for w in d615('rd', addr, n).get('words', [])]
def ioreg(key):
    o = subprocess.run(['/usr/sbin/ioreg', '-l', '-w0', '-r', '-c', 'NVGspControl'], capture_output=True, text=True).stdout
    return next((l.split('= ')[1] for l in o.splitlines() if '"NVGspControl-%s"' % key in l), None)
def push(user, pb, pairs, label):
    blob = b''.join(struct.pack('<II', (1 << 18) | m, v) for m, v in pairs)
    base = rd(user)[0]
    if base + len(blob) + 4 > 4096:
        d615('vwr', pb + base, struct.pack('<I', 0x20000000).hex()); base = 0
    d615('vwr', pb + base, blob.hex()); put = base + len(blob)
    d615('wr', user, struct.pack('<I', put).hex())
    for _ in range(40):
        if rd(user + 4)[0] == put: break
        time.sleep(0.05)
    print('%-28s put/get %s except core/wnd %s' % (label, [hex(x) for x in rd(user, 2)], [hex(rd(0x611020)[0]), hex(rd(0x61102c)[0])]))
END = [(0x218, 0), (0x21c, 0), (0x200, 1)]
# nvkms (Ada): maxPixelsFetchedPerLine = (((viewport_in_w + 14) * maxHScale + 1023) >> 10) + 8; taps 2 -> 1, 5 -> 4
TAPS = int(sys.argv[1]) if len(sys.argv) > 1 else 2
FETCH = (((3840 + 14) * 0x800 + 1023) >> 10) + 8
BOUNDS = 0x10010000 | (1 if TAPS == 2 else 4) << 20 | FETCH
CTRL = 0x11 if TAPS == 2 else 0x44
UP = len(sys.argv) > 2 and sys.argv[2] == 'up'   # upscale: SIZE_IN 2880x1620 -> SIZE_OUT 3840x2160
if UP:
    BOUNDS = 0x10010000 | (1 if TAPS == 2 else 4) << 20 | 1 << 24 | (((3840 + 14) * 0x400 + 1023) >> 10) + 8
print('taps', TAPS, 'bounds 0x%x ctrl 0x%x' % (BOUNDS, CTRL))
back = ioreg('display615-dblbuf'); print('dblbuf', back)
print('dbl stop', d615('dbl', 0, 0, 0))
for _ in range(40):
    if ioreg('display615-dblbuf') in ('0', None): break
    time.sleep(0.05)
print('dblbuf now', ioreg('display615-dblbuf'), 'front offset armed 0x%x' % rd(0x690a60)[0])
print('armed before: scale 0x%x bounds 0x%x size_in 0x%x size_out 0x%x taps 0x%x' % tuple(rd(a)[0] for a in (0x68900c, 0x689010, 0x690a98, 0x690aa4, 0x690aa8)))
try:
    push(0x680000, CPB, [(0x100c, 0x04000400 if UP else 0x08000800), (0x1010, BOUNDS)] + END, 'core wnd0 bounds 0x%x' % BOUNDS)
    # 5-tap input-scaler coefficients (nvkms InitTaps5ScalerCoefficientsC5 values; index = ratio<<6 | phase<<2 | i)
    C5 = [0, 0, -16, 144, 0, -5, 5, 0, 0, -9, 11, 0, -1, -12, 18, -1, -1, -15, 25, -1, -1, -18, 33, -2, -2, -20, 42, -3, -2, -21, 51, -3, -3, -22, 60, -5, -3, -22, 70, -6, -4, -22, 81, -7, -4, -22, 91, -9, -5, -21, 102, -10, -5, -20, 113, -12, -5, -19, 125, -13, -6, -18, 136, -15, 3, 60, 20, 108, 3, 57, 63, 4, 2, 54, 66, 4, 2, 51, 69, 5, 2, 48, 72, 6, 1, 45, 75, 7, 1, 43, 78, 7, 1, 40, 81, 8, 1, 37, 84, 9, 0, 35, 88, 10, 0, 33, 91, 12, 0, 30, 94, 13, 0, 28, 97, 14, 0, 26, 99, 16, 0, 24, 102, 17, 0, 22, 105, 19, 4, 62, 23, 105, 4, 59, 64, 5, 3, 56, 67, 6, 3, 53, 70, 7, 2, 51, 73, 8, 2, 48, 76, 8, 2, 45, 79, 9, 1, 43, 81, 10, 1, 40, 84, 12, 1, 38, 87, 13, 1, 36, 90, 14, 0, 34, 92, 15, 0, 31, 95, 17, 0, 29, 97, 18, 0, 27, 100, 20, 0, 25, 102, 22]
    coeff = [(0x2ac, (C5[i] & 0x3ff) | i << 12) for i in range(len(C5))]
    for k in (range(0, len(coeff), 64) if 'coeff' in sys.argv else ()):
        push(0x690000, WPB, coeff[k:k + 64], 'wnd0 scaler coeff %d' % k)
        print('   coeff except words', [hex(x) for x in rd(0x61102c, 3)])
    push(0x690000, WPB, ([(0x2a8, CTRL), (0x298, 2880 | 1620 << 16), (0x200, 1)] if UP else [(0x2a8, CTRL), (0x2a4, 2880 | 1620 << 16), (0x200, 1)]), 'wnd0 scaled')
    time.sleep(0.3)
    print('armed scaled: scale 0x%x bounds 0x%x size_in 0x%x size_out 0x%x ctrl 0x%x except %s' % (tuple(rd(a)[0] for a in (0x68900c, 0x689010, 0x690a98, 0x690aa4, 0x690aa8)) + ([hex(x) for x in rd(0x61102c, 3)],)))
    time.sleep(3)
finally:
    push(0x690000, WPB, [(0x2a8, 0x11), (0x298, 3840 | 2160 << 16), (0x2a4, 3840 | 2160 << 16), (0x200, 1)], 'wnd0 size restore')
    push(0x680000, CPB, [(0x100c, 0x04000400), (0x1010, 0x10110f16)] + END, 'core wnd0 scale restore')
    print('armed after: scale 0x%x size_out 0x%x' % (rd(0x68900c)[0], rd(0x690aa4)[0]))
    if back and back != '0':
        print('dbl re-enable', d615('dbl', int(back), WPB, 0x690000))
    print('dblbuf end', ioreg('display615-dblbuf'))
