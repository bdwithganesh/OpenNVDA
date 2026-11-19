#!/usr/bin/env python3
"""portmap.py (root): read-only map of the GPU's display ports from GSP-RM (NV04_DISPLAY_COMMON controls):
supported / connected display IDs, the output resource (SOR index + protocol) and the physical connector type.
Layouts: open-gpu-kernel-modules ctrl0073system.h / ctrl0073specific.h (MIT)."""
import json, struct, subprocess, sys

D = '/Users/dhruveen/nvbuild/d615/d615'
COMMON = 0xc0d00073
CONN = {0x46: 'DisplayPort', 0x47: 'DP (internal)', 0x48: 'mini-DP', 0x61: 'HDMI-A', 0x63: 'mini-HDMI',
        0x71: 'USB-C', 0x74: 'USB-C (UHBR)', 0x30: 'DVI-I', 0x31: 'DVI-D', 0x00: 'VGA'}
PROTO = {1: 'TMDS-A (HDMI/DVI)', 2: 'TMDS-B (HDMI/DVI)', 5: 'dual TMDS', 8: 'DP-A', 9: 'DP-B'}


def ctrl(cmd, params):
    out = subprocess.run([D, 'ctrl', hex(COMMON), hex(cmd), bytes(params).hex()], capture_output=True, text=True).stdout
    try:
        r = json.loads(out)
    except ValueError:
        return None
    if r.get('kr') != '0x0' or r.get('rpc', '0x0') != '0x0' or r.get('status', '0x0') != '0x0':
        return None
    return bytes.fromhex(r.get('params', ''))


sup = ctrl(0x730107, bytes(12))
if sup is None:
    sys.exit('GET_SUPPORTED failed (display common object missing? run after the display bring-up)')
mask = struct.unpack_from('<I', sup, 4)[0]
cs = ctrl(0x730108, struct.pack('<IIII', 0, 0, mask, 0))
connected = struct.unpack_from('<I', cs, 8)[0] if cs else 0
ports = []
for bit in range(32):
    did = 1 << bit
    if not mask & did:
        continue
    oi = ctrl(0x73028b, struct.pack('<II', 0, did) + bytes(48))
    cd = ctrl(0x730250, struct.pack('<II', 0, did) + bytes(64))
    p = {'displayId': hex(did), 'connected': bool(connected & did)}
    if oi:
        idx, typ, proto = struct.unpack_from('<III', oi, 8)
        loc, dcb = struct.unpack_from('<I', oi, 28)[0], struct.unpack_from('<I', oi, 36)[0]
        p.update(sor=idx if typ == 2 else None, protocol=PROTO.get(proto, hex(proto)), dcbIndex=dcb,
                 litByVbios=bool(oi[48]), location='board' if loc else 'chip')
    if cd:
        n = struct.unpack_from('<I', cd, 16)[0]
        p['connector'] = [CONN.get(struct.unpack_from('<I', cd, 20 + 12 * i + 4)[0],
                                   hex(struct.unpack_from('<I', cd, 20 + 12 * i + 4)[0])) for i in range(min(n, 4))]
        p['connectorIndex'] = [struct.unpack_from('<I', cd, 20 + 12 * i)[0] for i in range(min(n, 4))]
    ports.append(p)
if '--json' in sys.argv:
    print(json.dumps({'supported': hex(mask), 'connected': hex(connected), 'ports': ports}, indent=1))
else:
    print('supported 0x%x connected 0x%x' % (mask, connected))
    for p in ports:
        print('  %-6s %-9s conn %-18s idx %-6s SOR %-4s %-18s dcb %s%s' % (
            p['displayId'], 'CONNECTED' if p['connected'] else '-', ','.join(p.get('connector', ['?'])),
            p.get('connectorIndex'), p.get('sor'), p.get('protocol', '?'), p.get('dcbIndex'),
            ' (lit by VBIOS)' if p.get('litByVbios') else ''))
