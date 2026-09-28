#!/usr/bin/env python3
"""A3 IMP (IS_MODE_POSSIBLE) param builder - C372 0xc3720101.

Struct layout from NVIDIA open driver 570.144
(_dl/open-gpu-kernel-modules/.../ctrl/ctrlc372/ctrlc372chnc.h + ctrlc372base.h):
- CMD = NVC372_CTRL_CMD_IS_MODE_POSSIBLE (0xc3720101)
- base = NVC372_CTRL_CMD_BASE_PARAMS { NvU32 subdeviceIndex }
- head[8] NVC372_CTRL_IMP_HEAD (92B each, C packing), window[32] (36B each)
- total NVC372_CTRL_IS_MODE_POSSIBLE_PARAMS = 2048B
- MAX_HEADS=8, MAX_WINDOWS=32, MAX_TILES=8

Fills single-head case from EDID timings (see ../A3-modes/edid_modes.py).
IS_MODE_POSSIBLE is a *test* call (bIsPossible out) - modeset se pehle chalti hai.

Run: python3 imp_params.py --selftest
"""
import struct
import sys

CMD_IS_MODE_POSSIBLE = 0xC3720101
MAX_HEADS, MAX_WINDOWS, MAX_TILES = 8, 32, 8
HEAD_SIZE, WINDOW_SIZE, PARAMS_SIZE = 92, 36, 2048

# BenQ PD2705U 4K60 preset (EDID: 3840x2160@60, pixel clock 533.25 MHz; CTA ht=4400
# vt=2250)
MODE_4K60 = dict(h_active=3840, v_active=2160, h_blank=560, v_blank=90,
                 pixclk_khz=533250)

def pack_head(head_index=0, mode=MODE_4K60) -> bytes:
    ha, va, hb, vb, clk = (mode["h_active"], mode["v_active"], mode["h_blank"],
                           mode["v_blank"], mode["pixclk_khz"])
    b = bytearray()
    b += struct.pack("<B", head_index) + b"\x00\x00\x00"  # + pad to u32
    b += struct.pack("<I", clk)                            # maxPixelClkKHz
    b += struct.pack("<II", ha, va)                        # rasterSize
    b += struct.pack("<II", 0, 0)                          # rasterBlankStart
    b += struct.pack("<II", ha + hb - 1, va + vb - 1)      # rasterBlankEnd
    b += struct.pack("<II", 0, 0)                          # rasterVertBlank2
    b += struct.pack("<IIII", 0, 0, 0, 0)                  # control locks
    b += struct.pack("<II", 1, 1)                          # maxDownscale H/V
    b += struct.pack("<BBBB", 0, 0, 0, 0)                  # taps/upscale/overfetch/ltm
    b += struct.pack("<HH", 0, 0)                          # minFrameIdle
    b += struct.pack("<BBBB", 0, 0, 0, 0)                  # lut/cursor32p/tileMask/dscEn
    b += struct.pack("<H", 0) + b"\x00\x00"                # dscBpp + pad
    b += struct.pack("<II", 0, 0)                          # dsc slice mask/width
    b += struct.pack("<BBBB", 0, 0, 0, 0)                  # yuv420/2head1or/osld/midframe
    assert len(b) == HEAD_SIZE, len(b)
    return bytes(b)

def pack_window(window_index=0, head=0, width=3840) -> bytes:
    b = struct.pack("<IIIIIII", window_index, head, 0x1112, 0x1112, width, 1, 1)
    b += struct.pack("<BBBBBB", 0, 0, 0, 0, 0, 0) + b"\x00\x00"  # taps/flags/lut/tmo/layout + pad
    assert len(b) == WINDOW_SIZE, len(b)
    return b

def build_imp(num_heads=1, num_windows=1, mode=MODE_4K60,
              subdevice=0, test_mclk_khz=0) -> bytes:
    b = bytearray()
    b += struct.pack("<I", subdevice)                      # base
    b += struct.pack("<BB", num_heads, num_windows) + b"\x00\x00"
    for i in range(MAX_HEADS):
        b += pack_head(i, mode) if i < num_heads else b"\x00" * HEAD_SIZE
    for i in range(MAX_WINDOWS):
        b += pack_window(i, 0, mode["h_active"]) if i < num_windows else b"\x00" * WINDOW_SIZE
    b += struct.pack("<II", 0, test_mclk_khz)              # options, testMclkFreqKHz
    b += struct.pack("<B", 0)                              # bIsPossible (out)
    b += b"\x00" * MAX_HEADS                               # bIsOSLDPossible[8] (out)
    b += b"\x00\x00\x00"                                   # pad to u32
    b += struct.pack("<IIIII", 0, 0, 0, 0, 0)              # minVPState/minPState/bw/floor/hubclk
    b += struct.pack("<8I", *([0] * 8))                    # vblankIncrease (out)
    b += struct.pack("<8I", *([0] * 8))                    # wakeUpRgLine (out)
    b += struct.pack("<I", 0)                              # worstCaseMargin (out)
    b += struct.pack("<I", 0)                              # dispClkKHz (out)
    b += struct.pack("<I", 0)                              # numTilingAssignments
    b += b"\x00" * MAX_TILES                               # tilingAssignments
    b += b"\x00" * (MAX_TILES * 2)                         # tileList
    b += b"\x00" * 8                                       # worstCaseDomain (out)
    b += struct.pack("<B", 0)                              # bUseCachedPerfState
    b += b"\x00\x00\x00"                                   # tail pad
    assert len(b) == PARAMS_SIZE, len(b)
    return bytes(b)

def selftest() -> int:
    fails = 0
    def ck(c, m):
        nonlocal fails
        print(("PASS " if c else "FAIL ") + m)
        if not c:
            fails += 1
    ck(CMD_IS_MODE_POSSIBLE == 0xC3720101, "cmd id from header")
    ck(len(pack_head()) == 92, "HEAD 92B C packing")
    ck(len(pack_window()) == 36, "WINDOW 36B C packing")
    p = build_imp()
    ck(len(p) == 2048, "PARAMS 2048B")
    ck(p[4] == 1 and p[5] == 1, "numHeads/numWindows")
    ck(struct.unpack("<I", p[12:16])[0] == 533250, "maxPixelClkKHz = EDID 533.25MHz")
    ck(struct.unpack("<II", p[16:24]) == (3840, 2160), "rasterSize 4K")
    print("SELFTEST", "OK" if fails == 0 else f"{fails} FAILURES")
    return fails

if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(1 if selftest() else 0)
    sys.stdout.buffer.write(build_imp())
