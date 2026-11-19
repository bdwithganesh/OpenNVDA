#!/usr/bin/env python3
# Whole verified native615 display bring-up in one run (kernel 0.180.136+,
# root): display common + root, instance memory, core channel, DP link on
# display 0x200 (4x HBR2), head 0 3840x2160@60 with OLUT and hardware cursor,
# window 0 with a full-screen surface, cursor PIO channel.
#   disp_all.py [vram|desktop|sysfb] [--resume]  window 0 source: vram = test
#     gradient; desktop = 0.180.138 sysmem framebuffer CE-copied into the VRAM
#     surface at ~60 Hz; sysfb = direct PCI scan-out (refused by the hardware, 0x1d)
#   --resume  2026-11-17: continue a bring-up that stopped at the DP link (no
#     monitor at boot): reuse the display root, instance memory and core
#     channel from /tmp/disp615-partial.json and run the DP link onwards.
# State goes to /tmp/disp615.json for later tools.
import array, json, os, struct, subprocess, sys, time

D = '/Users/dhruveen/nvbuild/d615/d615'
DEV, ROOT, CORE, WND, CURS = 0xc0d00080, 0xc0d0c770, 0xc0d0c77d, 0xc0d0c67e, 0xc0d0c67a
COMMON = 0xc0d00073
CLIENT = 0xc0d00001
H_INST, H_COREPB, H_WNDPB, H_CURSURF = 0xc0d0d040, 0xc0d0d041, 0xc0d0d042, 0xc0d0d045
ISO, CURS_DMA, OLUT_DMA, ILUT_DMA, SYS_DMA = 0xc0d0d001, 0xc0d0d010, 0xc0d0d006, 0xc0d0d007, 0xc0d0d008   # cursor 0xc0d0d002 collided with OLUT in RAMHT
DISPLAY = 0x200
FULL, LUT = 0x3dc000000, 0x3de000000     # top of the kernel diagnostic VRAM range
OLUT_OFF, LUT_LIMIT = 0x2100, 0x4fff     # head OLUT table inside the LUT surface (256-aligned)
W, H = 3840, 2160
CURS_USER = 0x690000 + 72 * 0x1000
args = [a for a in sys.argv[1:] if not a.startswith('--')]
source = args[0] if args else 'vram'
RESUME = '--resume' in sys.argv
PARTIAL = '/tmp/disp615-partial.json'
# One-shot test hook: pretend no sink answered the DP step (no-monitor boot).
FAKE_NODP = '/Library/NativeDriverLab/display/fake-nodp'


def d615(*args):
    a = [x if isinstance(x, str) else hex(x) for x in args]
    out = subprocess.run([D] + a, capture_output=True, text=True).stdout.strip()
    try:
        return json.loads(out)
    except ValueError:
        return {'raw': out}


def ok(r):
    return r.get('kr') == '0x0' and r.get('rpc') == '0x0' and r.get('status', '0x0') == '0x0'


def need(r, what):
    if not ok(r):
        sys.exit('%s failed: %s' % (what, json.dumps(r)[:300]))
    return r


def rd(addr, n=1):
    return [int(w, 16) for w in d615('rd', addr, n).get('words', [])]


def wr(addr, *words):
    d615('wr', addr, b''.join(struct.pack('<I', w) for w in words).hex())


def vram(handle, size, align):
    p = bytearray(128)
    struct.pack_into('<III', p, 0, CLIENT, 6, 0x100)
    struct.pack_into('<I', p, 24, 0x800000)
    struct.pack_into('<QQ', p, 64, size, align)
    r = need(d615('alloc', DEV, handle, 0x40, p.hex()), 'vram 0x%x' % handle)
    return struct.unpack_from('<Q', bytes.fromhex(r['params']), 80)[0]


def ctrl(cmd, params):
    r = d615('ctrl', COMMON, cmd, bytes(params).hex())
    return ok(r), bytes.fromhex(r.get('params', ''))


def aux(addr, write=None, size=1):
    p = bytearray(48)
    struct.pack_into('<IIxxxxII', p, 0, 0, DISPLAY, 0x8 if write is not None else 0x9, addr)
    if write is not None:
        p[20:20 + len(write)] = write
        size = len(write)
    struct.pack_into('<I', p, 36, size - 1)
    for _ in range(10):
        good, r = ctrl(0x731341, p)
        if good:
            return r[20:20 + size]
        time.sleep(0.01)
    return None


def pbparams(phys, hclass, valid=1):
    return struct.pack('<IIQQIIIB3xIIII', 2 if valid else 0, 0, phys, 0xfff if valid else 0, 0, hclass, 0,
                       valid, 1 if valid else 0, 0, 0, 0)


def channel(handle, hclass, pb, nparams):
    if pb is not None:
        d615('vwr', pb, '00' * 256)
    need(d615('ictrl', 0x20800a58, pbparams(pb or 0, hclass, pb is not None).hex()), 'pushbuffer %x' % hclass)
    need(d615('alloc', ROOT, handle, hclass, '00' * nparams), 'channel %x' % hclass)


def push(user, pb, pairs, label):
    words = []
    for m, v in pairs:
        words += [(1 << 18) | m, v]
    blob = b''.join(struct.pack('<I', w) for w in words)
    base = rd(user)[0]
    if base + len(blob) + 4 > 4096:
        d615('vwr', pb + base, struct.pack('<I', 0x20000000).hex())
        base = 0
    d615('vwr', pb + base, blob.hex())
    put = base + len(blob)
    wr(user, put)
    for _ in range(60):
        if rd(user + 4)[0] == put:
            break
        time.sleep(0.05)
    time.sleep(0.3)
    print('%-28s put/get %s' % (label, [hex(x) for x in rd(user, 2)]))


def rmhash(handle, chn):
    return (((handle >> 0) & 0x3ff) ^ ((handle >> 10) & 0x3ff) ^ ((handle >> 20) & 0x3ff) ^
            (((CLIENT & 0xff) << 2) | (handle >> 30)) ^ (((chn & 0xf) << 6) | ((CLIENT >> 8) & 0x3f)) ^
            ((chn >> 4) & 0x7))


def nvhash(handle, chn):
    h, x = handle, 0
    while h:
        x ^= h & 0x3ff
        h >>= 10
    return x ^ (chn << 6)


def ctxdma(inst, handle, at, chn, base, limit, target=1):
    # word0: TARGET_NODE (1 NVM, 2 PCI) | ACCESS read/write | KIND pitch
    d615('vwr', inst + at, struct.pack('<8I', target | 4, base >> 8, base >> 40, limit >> 8, limit >> 40, 0, 0, 0).hex())
    entry = struct.pack('<II', handle, (chn << 25) | (CLIENT & 0x3fff) | ((at >> 5) << 14))
    for slot in sorted({rmhash(handle, chn), nvhash(handle, chn)}):
        d615('vwr', inst + slot * 8, entry.hex())


def lut_surface():
    # 2026-11-17: nvkms layout (NVEvoLutDataRec): the window ILUT at 0 takes FP16
    # entries, the head OLUT at OLUT_OFF takes UNORM16 entries (nvkms-evo3.c
    # EvoSetupIdentity{Base,Output}LutC5). One FP16 table for both made the OLUT
    # read FP16 bit patterns as UNORM16: washed-out panel, correct VRAM/VNC.
    def table(vals):
        t = bytearray(32)                       # 4-entry VSS header
        for v in vals + vals[-1:]:              # 1025 entries, last repeated
            t += struct.pack('<HHHH', v, v, v, 0)
        return t
    ilut = table([struct.unpack('<H', struct.pack('<e', i / 1023))[0] for i in range(1024)])
    olut = table([i << 6 for i in range(1024)])
    return bytes(ilut.ljust(OLUT_OFF, b'\0') + olut)


t0 = time.time()
if RESUME:
    # the display root, instance memory and core channel already exist; the
    # allocations below would fail on the duplicate handles
    try:
        partial = json.load(open(PARTIAL))
    except (OSError, ValueError):
        sys.exit('resume: no partial bring-up state')
    if os.path.exists('/tmp/disp615.json'):
        sys.exit('resume: bring-up already complete')
    inst, corepb = partial['inst'], partial['core_pb']
    print('resume inst 0x%x core pb 0x%x' % (inst, corepb))
else:
    # 1. display objects and memory
    probe = subprocess.run(['/Users/dhruveen/nvbuild/dispprobe/disp_probe'], capture_output=True, text=True).stdout.strip()
    print(probe)
    # 2026-11-16: without the display common object every DP control fails (0x57)
    # and the later core/window methods wedge the GSP: stop here instead
    if not probe.startswith('{"kr":"0x0"'):
        sys.exit('display probe failed')
    print('reserve display VRAM', d615('reserve', 34, FULL))   # FULL (32 MiB) + LUT, 0.180.138
    # 2026-11-19: nvdisp615-boot.sh starts this in parallel with the first Metal client (which builds the
    # maintenance channel the CE frame copy needs). Everything above leaves the panel alone; wait here, before the
    # display root and the DP link take the panel from the boot screen, so it stays up until the first frame is ready.
    w0 = time.time()
    while time.time() - w0 < 30 and b'"NVGspControl-mmu615-live" = Yes' not in subprocess.run(
            ['/usr/sbin/ioreg', '-r', '-c', 'NVGspControl', '-l'], capture_output=True).stdout:
        time.sleep(0.05)
    print('maintenance channel wait %.2f s' % (time.time() - w0))
    need(d615('alloc', DEV, ROOT, 0xc770), 'display root')
    inst = vram(H_INST, 0x10000, 0x10000)
    d615('vfile', inst, '/dev/null')
    open('/tmp/zero.bin', 'wb').write(bytes(0x10000))
    d615('vfile', inst, '/tmp/zero.bin')
    need(d615('ictrl', 0x20800a49, struct.pack('<QQII', inst, 0x10000, 2, 2).hex()), 'write inst mem')
    corepb = vram(H_COREPB, 0x1000, 0x1000)
    channel(CORE, 0xc77d, corepb, 40)
    # 2026-11-17: resumable from here (hotplug-watch.sh runs --resume on plug)
    json.dump({'inst': inst, 'core_pb': corepb, 'source': source}, open(PARTIAL, 'w'), indent=1)
# 2. DP link
print('manual dp', ctrl(0x731365, bytes(4))[0])
p = bytearray(80); struct.pack_into('<I', p, 4, DISPLAY)
good, r = ctrl(0x731152, p)
print('assign sor', good, r[24:28].hex())
print('sink D0', aux(0x600, write=b'\x01') is not None)
time.sleep(0.05)
for t in range(3):
    p = bytearray(28); struct.pack_into('<IIII', p, 0, 0, DISPLAY, 0x2083, 0x1404)
    good, r = ctrl(0x731343, p)
    if good and struct.unpack_from('<I', r, 16)[0] == 0:
        break
    time.sleep(0.2)
print('train', good, 'lanes', (aux(0x202, size=3) or b'').hex())
p = bytearray(84)
struct.pack_into('<IIII', p, 0, 0, 0, 0, 1); p[16] = 1
struct.pack_into('<II', p, 24, 584, 15379); p[68] = 1
struct.pack_into('<II', p, 72, 64, 20)
streamed = ctrl(0x731362, p)[0]
print('stream', streamed)
if not RESUME and os.path.exists(FAKE_NODP):
    os.remove(FAKE_NODP)
    good = False
    print('fake-nodp: treating the DP link as down (test hook, removed)')
if not (good and streamed):
    sys.exit('DP link not up (train %s, stream %s)' % (good, streamed))
# 3. surfaces and context DMAs
open('/tmp/lut.bin', 'wb').write(lut_surface())
d615('vfile', LUT, '/tmp/lut.bin')
curs = vram(H_CURSURF, 256 * 256 * 4, 0x10000)
img = array.array('I', [0] * (256 * 256))
for y in range(64):
    for x in range(64):
        if x <= y * 0.62 and y < 56 or (abs(x - y * 0.35 - 6) < 4 and 30 < y < 64):
            edge = x == 0 or x >= y * 0.62 - 1.5 or y >= 55
            img[y * 256 + x] = 0xff000000 if edge else 0xffffffff
open('/tmp/cursor.bin', 'wb').write(img.tobytes())
d615('vfile', curs, '/tmp/cursor.bin')
ctxdma(inst, ISO, 0x2000, 1, 0, 0x3ffffffff)
ctxdma(inst, CURS_DMA, 0x2020, 0, curs, curs + 0x3ffff)
ctxdma(inst, OLUT_DMA, 0x20c0, 0, LUT, LUT + LUT_LIMIT)
ctxdma(inst, ILUT_DMA, 0x20e0, 1, LUT, LUT + LUT_LIMIT)
surf_dma, surf_off, pitch = ISO, FULL, W * 4
if source == 'desktop':
    r = d615('sysfb', 32)
    print('sysfb', r)
    if r.get('kr') != '0x0':
        sys.exit('sysfb failed')
elif source == 'sysfb':
    r = d615('sysfb', 32)
    print('sysfb', r)
    phys = int(r.get('phys', '0x0'), 16)
    if r.get('kr') != '0x0' or not phys:
        sys.exit('sysfb failed')
    ctxdma(inst, SYS_DMA, 0x2040, 1, phys, phys + (32 << 20) - 1, target=2)
    surf_dma, surf_off = SYS_DMA, 0
else:
    row = array.array('I', [0] * W)
    with open('/tmp/full.bin', 'wb') as f:
        for y in range(H):
            for x in range(W):
                r_, g_ = x * 255 // W, y * 255 // H
                b_ = 255 - r_
                if (x // 240 + y // 240) % 2 == 0:
                    r_, g_, b_ = r_ // 2, g_ // 2, b_ // 2
                if x < 16 or y < 16 or x >= W - 16 or y >= H - 16:
                    r_ = g_ = b_ = 255
                row[x] = 0xff000000 | r_ << 16 | g_ << 8 | b_
            f.write(row.tobytes())
    d615('vfile', FULL, '/tmp/full.bin')
# 4. core: window owners, then head 0 state while inactive, then the SOR
init = []
for i in range(8):
    b = 0x1000 + i * 0x80
    init += [(b, i >> 1), (b + 4, 0x7), (b + 8, 0), (b + 0xc, 0x04000400), (b + 0x10, 0x10110f16)]
END = [(0x218, 0), (0x21c, 0), (0x200, 1)]
push(0x680000, corepb, init + END, 'core window owners')
HEAD = [(0x2004, 0xfc000058), (0x200c, 0x1fc8bfd0), (0x2014, 0x11), (0x2020, DISPLAY),
        (0x2028, 0x1fc8bfd0), (0x202c, 0x04000400), (0x2030, 0x1014),
        (0x204c, 0x08700f00), (0x2058, 0x08700f00),
        (0x2064, 0x08ae0fa0), (0x2068, 0x0004001f), (0x206c, 0x003a006f), (0x2070, 0x08aa0f6f),
        (0x2218, 0x0003003b), (0x2220, 0x000000ff), (0x2224, 0x00000000),
        (0x2280, 0x40508), (0x2284, 0xffffffff), (0x2288, OLUT_DMA), (0x228c, OLUT_OFF >> 8),
        (0x2098, 0), (0x2088, CURS_DMA), (0x208c, CURS_DMA), (0x2090, 0), (0x2094, 0),
        (0x209c, 0x800003cf), (0x20a0, 0x72ff)]
push(0x680000, corepb, HEAD + END, 'core head 0 (inactive)')
push(0x680000, corepb, [(0x300, 0x901)] + END, 'core SOR0 attach')
sor = rd(0x688300)[0]
print('armed SOR 0x%x raster 0x%x cursor 0x%x olut 0x%x' % (sor, rd(0x68a064)[0], rd(0x68a09c)[0], rd(0x68a280)[0]))
# 2026-11-19: the first desktop frame goes into the scanout surface BEFORE window 0 points at it. VRAM keeps
# its contents across guest reboots, so arming first showed the previous session's last frame (or garbage) for a
# few frames.
if source == 'desktop':
    print('fb copy once', d615('fbcopy', 1, FULL, pitch, H))
# 5. window 0
wndpb = vram(H_WNDPB, 0x1000, 0x1000)
channel(WND, 0xc67e, wndpb, 40)
wh = W | (H << 16)
push(0x690000, wndpb, [(0x308, 1), (0x224, wh), (0x228, 0), (0x22c, 0xcf), (0x230, pitch >> 6),
                       (0x240, surf_dma), (0x260, surf_off >> 8), (0x290, 0), (0x298, wh), (0x2a4, wh),
                       (0x2a8, 0x11),
                       # Linux pipeline: a BYPASS window is refused (0x2e) while the head OLUT is on
                       (0x2ec, 0x80), (0x2f4, 0x11), (0x440, 0x40508), (0x444, ILUT_DMA), (0x448, 0),
                       (0x400, 0x10000), (0x414, 0x10000), (0x428, 0x10000),
                       (0x460, 0x10000), (0x474, 0x10000), (0x488, 0x10000),
                       (0x5a0, 0x10000), (0x5b4, 0x10000), (0x5c8, 0x10000), (0x5d0, 0x3c000000),
                       (0x370, 0), (0x374, 0), (0x200, 1)], 'window 0 surface')
print('armed window size 0x%x ctxdma 0x%x offset 0x%x' % (rd(0x690a24)[0], rd(0x690a40)[0], rd(0x690a60)[0]))
# 6. cursor channel, centred
channel(CURS, 0xc67a, None, 16)
wr(CURS_USER + 0x208, (H // 2 << 16) | W // 2)
wr(CURS_USER + 0x200, 1)
lines = [d615('ctrl', COMMON, 0x730104, '00' * 20).get('params', '')[16:24] for _ in range(3)]
print('scanline samples', lines, 'except core/wnd', hex(rd(0x611020)[0]), hex(rd(0x61102c)[0]))
if source == 'desktop':
    print('fb copy 60 Hz', d615('fbcopy', 2, FULL, pitch, H))
print('register cursor for NVDisplay', d615('native', curs, CURS_USER))   # 0.180.141
json.dump({'inst': inst, 'core_pb': corepb, 'wnd_pb': wndpb, 'cursor_surface': curs, 'lut': LUT,
           'surface_dma': surf_dma, 'surface_offset': surf_off, 'pitch': pitch, 'source': source,
           'sor_armed': sor}, open('/tmp/disp615.json', 'w'), indent=1)
if os.path.exists(PARTIAL):
    os.remove(PARTIAL)
print('done in %.1f s, source %s' % (time.time() - t0, source))
