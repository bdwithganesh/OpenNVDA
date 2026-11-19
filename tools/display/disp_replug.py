#!/usr/bin/env python3
# Hotplug retrain on the existing disp_all bring-up (kernel 0.180.150+).
# Re-runs the proven DP sequence (manual DP, SOR assign, sink D0, 4xHBR2
# training, SST stream) and only re-attaches the SOR if it is detached. It
# never allocates objects, never writes the window or the OCSC/head colour
# registers, so it is safe on the live head.
import json, struct, subprocess, sys, time

D = '/Users/dhruveen/nvbuild/d615/d615'
COMMON = 0xc0d00073
DISPLAY = 0x200
PB = 0x3f3c46000
try:                          # 2026-11-17: the core pushbuffer disp_all.py allocated
    _st = json.load(open('/Library/NativeDriverLab/display/state/disp615.json'))
    PB = _st['core_pb']
    DISPLAY = _st.get('display', DISPLAY)   # 2026-11-19: the port disp_all.py drove (any DP connector)
except (OSError, ValueError, KeyError):
    pass
# shortcut: retrains only the port picked at boot (the kernel polls just that one); moving the cable to another
# port needs a full disp_all.py run (reboot). Upgrade with a head re-route when runtime port moves matter.


def d615(*args, quiet=False):
    a = [x if isinstance(x, str) else hex(x) for x in args]
    out = subprocess.run([D] + a, capture_output=True, text=True).stdout.strip()
    try:
        r = json.loads(out)
    except ValueError:
        r = {'raw': out}
    if not quiet:
        print(json.dumps({'args': a[:3], **r})[:240])
    return r


def ctrl(cmd, params):
    r = d615('ctrl', COMMON, cmd, bytes(params).hex(), quiet=True)
    good = r.get('kr') == '0x0' and r.get('status') == '0x0'
    return good, bytes.fromhex(r.get('params', ''))


def aux(addr, write=None, size=1):
    p = bytearray(48)
    struct.pack_into('<II', p, 0, 0, DISPLAY)
    struct.pack_into('<II', p, 12, 0x8 if write is not None else 0x9, addr)
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


def or_protocol(did):
    # NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO: protocol 8 = DP-A, 9 = DP-B, 1/2 = TMDS (HDMI/DVI)
    good, r = ctrl(0x73028b, struct.pack('<II', 0, did) + bytes(48))
    return struct.unpack_from('<I', r, 16)[0] if good and len(r) >= 20 else None


def rd(addr, n=1):
    return [int(w, 16) for w in d615('rd', addr, n, quiet=True).get('words', [])]


def push(user, pb, pairs, label):
    words = []
    for m, v in pairs:
        words += [(1 << 18) | m, v]
    blob = b''.join(struct.pack('<I', w) for w in words)
    base = rd(user)[0]
    if base + len(blob) + 4 > 4096:
        d615('vwr', pb + base, struct.pack('<I', 0x20000000).hex(), quiet=True)
        base = 0
    d615('vwr', pb + base, blob.hex(), quiet=True)
    put = base + len(blob)
    d615('wr', user, struct.pack('<I', put).hex(), quiet=True)
    for _ in range(40):
        if rd(user + 4)[0] == put:
            break
        time.sleep(0.05)
    print('%s put/get %s' % (label, [hex(x) for x in rd(user, 2)]))


t0 = time.time()
print('manual dp', ctrl(0x731365, bytes(4))[0])
p = bytearray(80); struct.pack_into('<I', p, 4, DISPLAY)
good, r = ctrl(0x731152, p)
sors = struct.unpack_from('<4I', r, 24) if len(r) >= 40 else ()
sor = next((i for i, m in enumerate(sors) if m & DISPLAY), 0)
proto = or_protocol(DISPLAY)
if proto not in (8, 9):
    sys.exit('display 0x%x protocol %s is not DisplayPort; nothing to retrain' % (DISPLAY, proto))
armed = 0x1 | proto << 8      # SOR_SET_CONTROL: owner head 0, DP-A/DP-B
print('display 0x%x assign sor %s %s -> SOR %d protocol %d' % (DISPLAY, good, [hex(x) for x in sors], sor, proto))
print('sink D0', aux(0x600, write=b'\x01') is not None)
for t in range(3):
    p = bytearray(28); struct.pack_into('<IIII', p, 0, 0, DISPLAY, 0x2083, 0x1404)
    good, r = ctrl(0x731343, p)
    err = struct.unpack_from('<I', r, 16)[0] if len(r) >= 20 else -1
    print('train try %d ok %s err 0x%x' % (t, good, err))
    if good and not err:
        break
    time.sleep(0.2)
lane = aux(0x202, size=6)
print('lane status', lane.hex() if lane else 'None')
p = bytearray(84)
struct.pack_into('<IIII', p, 0, 0, 0, sor, 1)
p[16] = 1
struct.pack_into('<II', p, 24, 584, 15379)
p[68] = 1
struct.pack_into('<II', p, 72, 64, 20)
print('stream', ctrl(0x731362, p)[0])
sor_state = rd(0x688300 + 0x20 * sor)[0]
print('SOR state', hex(sor_state), 'scanline', hex(rd(0x68a064)[0]))
if sor_state & 0xffffffff != armed:
    push(0x680000, PB, [(0x300 + sor * 0x20, armed), (0x218, 0), (0x21c, 0), (0x200, 1)], 'reattach')
    time.sleep(0.2)
    print('SOR after attach', hex(rd(0x688300 + 0x20 * sor)[0]), 'scanline', hex(rd(0x68a064)[0]))
print('replug done in %.2f s' % (time.time() - t0))
