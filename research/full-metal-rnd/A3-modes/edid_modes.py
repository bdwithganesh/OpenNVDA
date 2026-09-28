#!/usr/bin/env python3
"""A3 EDID → mode list (read-only, live EDID hex paste karke chalao).

Usage: python3 edid_modes.py '<256-byte EDID hex>'  (ioreg NVGspControl-edid se)
Aj live BenQ PD2705U EDID (256B, ioreg se) default me baked hai.
Output: established timings + detailed timing descriptors (pixel clock, H/V active/blank).
IMP per mode (doc A3) abhi RM C372 IS_MODE_POSSIBLE se karna hai - ye tool sirf candidate list deta hai.
"""
import struct
import sys

LIVE_EDID_HEX = ("00ffffffffffff0009d13980455400000a200104b53c22783f2895a7554ea3260f5054"
"a56b80d1c0b300a9c08180810081c0010101014dd000a0f0703e803020350055502100001a000000ff0038334e30333932303031390a20000000fd00324c87873c010a202020202020000000fc0042656e5120504432373035550a01cc020345f14f5d5e5f6061101f22212004131203012309070783010000e200cf6d030c0020003878200060010203681a00000101283c00e305c301e30f1800e6060501626200565e00a0a0a029503020350055502100001e4d6c80a070703e8030203a0055502100001a00000000000000000000000000000000000000000000f0")

def parse_edid(h: bytes):
    assert len(h) >= 128 and h[:8] == bytes([0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00]), "bad header"
    out = []
    # established + standard timings are skipped (summary only); the
    # detailed descriptors sit at 54, 72, 90 and 108
    for i in range(4):
        d = h[54 + 18 * i:54 + 18 * (i + 1)]
        if d[:2] == b"\x00\x00":
            tag = {0xFF: "serial", 0xFD: "range", 0xFC: "monitor-name"}.get(d[3], f"tag {d[3]:02x}")
            print(f"  [descriptor {i}: {tag}]")
            continue
        pixclk = struct.unpack("<H", d[:2])[0] * 10000
        ha = d[2] | ((d[4] & 0xF0) << 4)
        hb = d[3] | ((d[4] & 0x0F) << 8)
        va = d[5] | ((d[7] & 0xF0) << 4)
        vb = d[6] | ((d[7] & 0x0F) << 8)
        ht, vt = ha + hb, va + vb
        hz = pixclk / (ht * vt) if ht * vt else 0
        out.append((ha, va, round(hz, 2), pixclk))
    return out

def parse_cta(ed: bytes) -> dict:
    """CTA-861 ext (block 128): video/audio/HDR (EOTF) data blocks. Returns summary."""
    info = {}
    if len(ed) < 256 or ed[128] != 0x02 or ed[129] != 0x03:
        return info
    info["cta_rev"] = ed[129]
    info["dtd_start"] = ed[130]
    native = []
    hdr_eotf = []
    i = 132
    while i < 132 + (ed[130] - 4 if ed[130] >= 4 else 0):
        tag = (ed[i] >> 5) & 0x7
        ln = ed[i] & 0x1F
        body = ed[i + 1:i + 1 + ln]
        if tag == 2:  # Video Data Block: VICs, bit7 of the first one marks native
            for b in body:
                vic = b & 0x7F
                if b & 0x80:
                    native.append(vic)
            info["vics"] = [b & 0x7F for b in body]
        elif tag == 7:  # Extended: [ext_tag, ...]
            if body and body[0] == 6:  # HDR Static Metadata block
                info["hdr_eotf"] = body[1] if len(body) > 1 else 0
                # bit0 SDR, bit1 HDR10 (ST2084), bit2 HLG, bit3 Dolby
                # Vision
                hdr_eotf = [n for n, m in (("SDR", 1), ("ST2084", 2), ("HLG", 4)) if info["hdr_eotf"] & m]
                info["hdr_list"] = hdr_eotf
        i += 1 + ln
    info["native_vics"] = native
    return info

if __name__ == "__main__":
    hx = (sys.argv[1] if len(sys.argv) > 1 else LIVE_EDID_HEX).replace(" ", "").replace("<", "").replace(">", "")
    ed = bytes.fromhex(hx)
    print(f"EDID {len(ed)}B, manuf {(ed[8:10]).hex()}, product {struct.unpack('<H', ed[10:12])[0]:04x}")
    print(f"EDID ext blocks: {ed[126]}")
    for ha, va, hz, clk in parse_edid(ed):
        print(f"  {ha}x{va} @ {hz} Hz (pixclk {clk/1e6:.2f} MHz)")
    cta = parse_cta(ed)
    if cta:
        print(f"CTA rev {cta.get('cta_rev')}, VICs {cta.get('vics')}, native {cta.get('native_vics')}")
        print(f"HDR EOTF mask {cta.get('hdr_eotf', 0):#04x} {cta.get('hdr_list', [])} (A4: 10-bit/HDR output)")
