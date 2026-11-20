# Register-level probe: can the head postcomp scaler take W x H (smaller = upscale, larger = downscale) to the
# native 3840x2160? Window 0 keeps scanning the native front surface (read only). Everything is restored.
# HAZARD: 6720x3780 (1.75x) wedges GSP-RM until a reboot; stay at or below 6016x3384. Upscale is refused.
import json, struct, subprocess, sys, time
D = '/Users/dhruveen/nvbuild/d615/d615'
st = json.load(open('/Library/NativeDriverLab/display/state/disp615.json'))
WPB, CPB = st['wnd_pb'], st['core_pb']
NW, NH = 3840, 2160
def d615(*a):
    out = subprocess.run([D] + [x if isinstance(x, str) else hex(x) for x in a], capture_output=True, text=True).stdout.strip()
    try: return json.loads(out)
    except ValueError: return {'raw': out}
def rd(addr, n=1): return [int(w, 16) for w in d615('rd', addr, n).get('words', [])]
def ioreg(key):
    o = subprocess.run(['/usr/sbin/ioreg', '-l', '-w0', '-r', '-c', 'NVGspControl'], capture_output=True, text=True).stdout
    return next((l.split('= ')[1] for l in o.splitlines() if '"NVGspControl-%s"' % key in l), None)
def push(user, pb, pairs):
    blob = b''.join(struct.pack('<II', (1 << 18) | m, v) for m, v in pairs)
    base = rd(user)[0]
    if base + len(blob) + 4 > 4096:
        d615('vwr', pb + base, struct.pack('<I', 0x20000000).hex()); base = 0
    d615('vwr', pb + base, blob.hex()); put = base + len(blob)
    d615('wr', user, struct.pack('<I', put).hex())
    for _ in range(40):
        if rd(user + 4)[0] == put: return True
        time.sleep(0.05)
    return False
END = [(0x218, 0), (0x21c, 0), (0x200, 1)]
N = NW | NH << 16
exc = lambda: [hex(x) for x in rd(0x611020, 3) + rd(0x61102c, 3)]
back = ioreg('display615-dblbuf')
print('dbl stop', d615('dbl', 0, 0, 0)); time.sleep(0.3)
base_exc = exc(); print('exceptions before', base_exc)
try:
    for arg in sys.argv[1:]:
        w, h, hub = (arg.split('x') + ['0x1014'])[:3]
        w, h, hub = int(w), int(h), int(hub, 16)
        sh, sv = max(0x400, (w * 1024 + NW - 1) // NW), max(0x400, (h * 1024 + NH - 1) // NH)
        W = w | h << 16
        ok1 = push(0x680000, CPB, [(0x1010, 0x10110000 | (w + 22)), (0x2030, hub), (0x202c, sh | sv << 16),
                                   (0x204c, W), (0x2058, N)] + END)
        ok2 = push(0x690000, WPB, [(0x224, W), (0x230, w * 4 >> 6), (0x298, W), (0x2a4, W), (0x200, 1)])
        time.sleep(0.4)
        armed_in, armed_w = rd(0x68a04c)[0], rd(0x690a24)[0]
        e = exc()
        print('%5dx%-5d hub 0x%05x maxout 0x%08x: fetched %s/%s armed vp_in 0x%08x wnd 0x%08x %s exceptions %s' % (
            w, h, hub, sh | sv << 16, ok1, ok2, armed_in, armed_w, 'ARMED' if armed_in == W and armed_w == W else 'REFUSED', e))
        push(0x690000, WPB, [(0x224, N), (0x230, NW * 4 >> 6), (0x298, N), (0x2a4, N), (0x200, 1)])
        push(0x680000, CPB, [(0x204c, N), (0x2058, N), (0x202c, 0x04000400), (0x2030, 0x1014), (0x1010, 0x10110f16)] + END)
        time.sleep(0.3)
finally:
    push(0x690000, WPB, [(0x224, N), (0x230, NW * 4 >> 6), (0x298, N), (0x2a4, N), (0x200, 1)])
    push(0x680000, CPB, [(0x204c, N), (0x2058, N), (0x202c, 0x04000400), (0x2030, 0x1014), (0x1010, 0x10110f16)] + END)
    print('restored vp_in 0x%x wnd 0x%x off 0x%x' % (rd(0x68a04c)[0], rd(0x690a24)[0], rd(0x690a60)[0]))
    if back and back != '0':
        print('dbl re-enable', d615('dbl', int(back), WPB, 0x690000))
