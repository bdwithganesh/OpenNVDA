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
# Head (postcomp) scaler probe: viewport SIZE_OUT 3840x2160 -> 2880x1620 on the core channel only.
regs = lambda: 'maxout 0x%x bounds 0x%x ctl 0x%x vp_in 0x%x vp_out 0x%x except core %s' % (
    tuple(rd(0x688000 + m)[0] for m in (0x202c, 0x2030, 0x2014, 0x204c, 0x2058)) + ([hex(x) for x in rd(0x611020, 3)],))
print('before', regs())
FIVEK = len(sys.argv) > 1 and sys.argv[1] == '5k'
if FIVEK:
    # 5K path: window 0 scans a 5120x2880 surface unscaled, the head viewport takes 5120x2880 in and the postcomp
    # scaler outputs 3840x2160. No 5K surface yet: the window reads the current one at a 5120 pitch (garbage).
    back = ioreg('display615-dblbuf'); print('dbl stop', d615('dbl', 0, 0, 0)); time.sleep(0.3)
    W5, H5 = 5120 << 0 | 2880 << 16, 3840 | 2160 << 16
    try:
        push(0x680000, CPB, [(0x100c, 0x04000400), (0x1010, 0x10110000 | ((5120 + 14) + 8)),
                             (0x202c, 0x08000800), (0x204c, W5), (0x2058, H5)] + END, 'core 5k viewport in, 4k out')
        push(0x690000, WPB, [(0x224, W5), (0x230, 5120 * 4 >> 6), (0x298, W5), (0x2a4, W5), (0x200, 1)], 'wnd0 5120x2880')
        time.sleep(0.3)
        print('5k', regs(), 'wnd size 0x%x in 0x%x out 0x%x pitch 0x%x except wnd %s' % (
            tuple(rd(0x690800 + m)[0] for m in (0x224, 0x298, 0x2a4, 0x230)) + ([hex(x) for x in rd(0x61102c, 3)],)))
        time.sleep(3)
    finally:
        push(0x690000, WPB, [(0x224, H5), (0x230, 3840 * 4 >> 6), (0x298, H5), (0x2a4, H5), (0x200, 1)], 'wnd0 restore')
        push(0x680000, CPB, [(0x204c, H5), (0x2058, H5), (0x202c, 0x04000400), (0x1010, 0x10110f16)] + END, 'core restore')
        print('after', regs(), 'wnd size 0x%x' % rd(0x690a24)[0])
        if back and back != '0':
            print('dbl re-enable', d615('dbl', int(back), WPB, 0x690000))
    sys.exit(0)
try:
    push(0x680000, CPB, [(0x202c, 0x08000800)] + END, 'core head max output scale 2.0')
    push(0x680000, CPB, [(0x2058, 2880 | 1620 << 16)] + END, 'core viewport out 2880x1620')
    time.sleep(0.3); print('scaled', regs()); time.sleep(3)
finally:
    push(0x680000, CPB, [(0x2058, 3840 | 2160 << 16), (0x202c, 0x04000400)] + END, 'core head restore')
    print('after', regs())
