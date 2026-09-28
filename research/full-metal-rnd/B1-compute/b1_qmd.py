#!/usr/bin/env python3
"""B1 first compute dispatch - REAL QMDV03_00 layout (host-side, SASS placeholder).

Sources (all in full-metal-rnd/_dl, no guesswork):
- QMD field bit positions: mesa/src/nouveau/headers/nvidia/classes/clc9c0qmd.h
  (NVC9C0_QMDV03_00_*, 275 defines, total 2048 bits = 64 dwords = 256 B)
- Init recipe: mesa/src/nouveau/compiler/nak/qmd.rs
  Qmd3_0 (AD103 >= AMPERE_COMPUTE_A) = [u32; 64];
  qmd_init!(QMDV03_00, major=3, minor=0) + SM_GLOBAL_CACHING_ENABLE=true
- Dispatch flow: nvk_cmd_dispatch.c nvk_cmd_upload_qmd → nak_fill_qmd(dev, info, qmd_info)

Word map (word = bit/32):
- w4:  bit6 SM_GLOBAL_CACHING_ENABLE | bits[5:0] QMD_GROUP_ID
- w11: bit26 API_VISIBLE_CALL_LIMIT=NO_CHECK(1) | bit30 SAMPLER_INDEX=INDEPENDENTLY(0)
- w12: CTA_RASTER_WIDTH (grid.x) | w13[15:0] HEIGHT (grid.y) | w14[15:0] DEPTH (grid.z)
- w17[17:0]: SHARED_MEMORY_SIZE
- w18: [7:4] QMD_MAJOR=3 | [3:0] QMD_VERSION=0 | [31:16] CTA_THREAD_DIM0 (block.x)
- w19: [15:0] DIM1 (block.y) | [31:16] DIM2 (block.z)
- w20[16:8]: REGISTER_COUNT_V (num_gprs, shader-dependent)
- w23[31:27]: BARRIER_COUNT
- w48: PROGRAM_ADDRESS_LOWER (shader VA bits 31:0) | w49[16:0] UPPER (bits 47:32)
- OUTER/INNER PUT/GET (w0-w3): 0 = fresh queue (NAK zero-inits)
- SM_DISABLE masks (w21/w22): 0 = all SMs enabled

Still needed for REAL dispatch: SASS bytes (nvcc -arch=sm_89, user machine) +
num_gprs/smem from shader info (nvdisasm/cubin header). Placeholder below = 1 CTA x 1 thread.

Run: python3 b1_qmd.py --selftest
"""
import struct
import sys

COMPUTE_CLASS = 0xC9C0
SUBCH_COMPUTE = 1
SEM_MAGIC = 0xC0FFEE35
QMD_DWORDS = 64

PLACEHOLDER_SASS = bytes(range(16)) * 4  # NOT sm_89, this gets replaced by the nvcc cubin .text


def build_qmd(grid=(1, 1, 1), block=(1, 1, 1), shader_va=0, num_gprs=8,
              smem_bytes=0, barrier_count=0) -> bytes:
    """Real Qmd3_0 init + dispatch fields (NAK qmd_init! + fill_qmd mirror)."""
    q = [0] * QMD_DWORDS
    # same as qmd_init!: major=3, minor=0, NO_CHECK, INDEPENDENTLY(0),
    # caching on
    q[4] |= (1 << 6)                                    # SM_GLOBAL_CACHING_ENABLE
    q[11] |= (1 << 26)                                  # API_VISIBLE_CALL_LIMIT=NO_CHECK
    q[18] |= (3 << 4) | 0                               # QMD_MAJOR=3, QMD_VERSION=0
    # dispatch dims
    q[12] = grid[0] & 0xFFFFFFFF
    q[13] |= grid[1] & 0xFFFF
    q[14] |= grid[2] & 0xFFFF
    q[18] |= (block[0] & 0xFFFF) << 16
    q[19] |= (block[1] & 0xFFFF) | ((block[2] & 0xFFFF) << 16)
    # shader info
    q[17] |= smem_bytes & 0x3FFFF
    q[20] |= (num_gprs & 0x1FF) << 8                    # REGISTER_COUNT_V
    q[23] |= (barrier_count & 0x1F) << 27               # BARRIER_COUNT
    q[48] = shader_va & 0xFFFFFFFF                      # PROGRAM_ADDRESS_LOWER
    q[49] |= (shader_va >> 32) & 0xFFFF                 # PROGRAM_ADDRESS_UPPER
    return struct.pack("<%dI" % QMD_DWORDS, *q)


def gpfifo_push(qmd_va: int, qmd_size: int, sem_va: int) -> list:
    """Method words: [subch select, QMD addr/size, LAUNCH, SEM_RELEASE magic]."""
    return [
        0x20000000 | SUBCH_COMPUTE,
        0x00000100, qmd_va & 0xFFFFFFFF,
        0x00000104, (qmd_va >> 32) & 0xFFFFFFFF,
        0x00000108, qmd_size,
        0x00000200, 0x1,
        0x00000300, sem_va & 0xFFFFFFFF,
        0x00000304, SEM_MAGIC,
    ]


def selftest() -> int:
    fails = 0

    def ck(c, m):
        nonlocal fails
        print(("PASS " if c else "FAIL ") + m)
        if not c:
            fails += 1

    q = build_qmd(grid=(1, 1, 1), block=(1, 1, 1), shader_va=0x1_1000_0000,
                  num_gprs=8)
    w = struct.unpack("<64I", q)
    ck(len(q) == 256, "QMD 64 dwords = 256B (nak Qmd3_0)")
    ck(w[4] == (1 << 6), "w4 caching-enable, group 0")
    ck(w[11] == (1 << 26), "w11 NO_CHECK")
    ck((w[18] >> 4) & 0xF == 3 and w[18] & 0xF == 0, "w18 major=3 minor=0")
    ck(w[12] == 1 and (w[13] & 0xFFFF) == 1 and (w[14] & 0xFFFF) == 1,
       "grid 1x1x1 words 12/13/14")
    ck((w[18] >> 16) == 1 and (w[19] & 0xFFFF) == 1 and (w[19] >> 16) == 1,
       "block 1x1x1 words 18/19")
    ck((w[20] >> 8) & 0x1FF == 8, "w20 num_gprs=8")
    ck(w[48] == 0x10000000 and (w[49] & 0xFFFF) == 0x1, "w48/49 shader VA")
    ck(all(v == 0 for v in w[0:4]), "w0-3 queue ptrs zero (fresh)")
    ck(all(v == 0 for v in w[21:23]), "w21/22 SM mask zero (all enabled)")
    pb = gpfifo_push(0x1_1000_0000, 256, 0x1_2000_0000)
    ck(pb[-1] == SEM_MAGIC, "sem magic present")
    print("NEEDS-SASS: nvcc -arch=sm_89 -cubin inc.cu; num_gprs/smem .cubin header se")
    print("SELFTEST", "OK" if fails == 0 else f"{fails} FAILURES")
    return fails


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(1 if selftest() else 0)
    print(__doc__)
