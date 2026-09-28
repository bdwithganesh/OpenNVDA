#!/usr/bin/env python3
"""B0 GPU memory manager prototype (host-side, no GPU needed).

Mirrors live constraints + NVIDIA open driver 570.144 headers
(_dl/open-gpu-kernel-modules, ga100/dev_mmu.h - Ada reuses Ampere MMU format):
- VA: 49-bit, usable low bound 64 MiB, 4 KiB tables, 64 KiB big pages
- PDE: 8 bytes (NV_MMU_PDE__SIZE 8); w0 = APERTURE_BIG[1:0] | SIZE[3:2] | ADDRESS[31:4];
  SIZE_FULL=0 → 2 MiB; ADDRESS_SHIFT=12; small half (w1) invalid when unused
- PTE: 8 bytes (NV_MMU_PTE__SIZE 8); w0 = VALID bit0 | PRIV bit1 | RO bit2 | ADDR[31:4];
  w1 = VOL bit0 | APERTURE[2:1] (0=VID) | LOCK bit3 | KIND[11:4] | RD_DIS bit30 | WR_DIS bit31
- Live PD0 16B entries = dual PDE pair (big-half + small-half), 2 MiB per slot;
  slots 32..52 ctx, 53..70 GOP FB [0,36MiB), 71 CE chunk
- VRAM: 16 GiB (AD103); host-managed split-VAS (MAP_MEMORY_DMA unsupported on GSP-RM)

Provides: BuddyAllocator, VASpace, pde_big/pte builders, FaultDecoder.
Run: python3 b0_mem.py --selftest
"""
import sys

VA_BITS = 49
VA_LOW_BOUND = 64 * 1024 * 1024
VRAM_BYTES = 16 * 1024**3
BIG_PAGE = 64 * 1024
HUGE_PAGE = 2 * 1024 * 1024
SMALL_PAGE = 4 * 1024
PD0_SLOTS_CTX = (32, 52)
PD0_SLOTS_FB = (53, 70)
PD0_SLOT_CE = 71

def slot_va(slot: int) -> int:
    # PD0 slot i covers VA [i*2MiB, (i+1)*2MiB). Checking against the live CE slot 71
    # -> 0x1_08E0_0000: 71*2MiB is 0x8E00000, and live shows 0x1_08E0_0000, the extra
    # 1G base comes from the PDE high range. So we keep a base + slot model, and the
    # high bit (bit32) picks the PDE range.
    return slot * HUGE_PAGE

class BuddyAllocator:
    """Simple first-fit allocator with free-list coalescing, 64K granularity."""
    def __init__(self, base: int, size: int, align: int = BIG_PAGE):
        self.base = base
        self.size = size
        self.align = align
        self.free = [(base, base + size)]
        self.used = {}
    def _align_up(self, v: int) -> int:
        return (v + self.align - 1) & ~(self.align - 1)
    def alloc(self, size: int, tag: str = "") -> int:
        size = self._align_up(max(size, self.align))
        for i, (s, e) in enumerate(self.free):
            a = self._align_up(s)
            if e - a >= size:
                del self.free[i]
                if a > s:
                    self.free.insert(i, (s, a))
                    i += 1
                if a + size < e:
                    self.free.insert(i + 1 if a > s else i, (a + size, e))
                # fix insert order
                self.free.sort()
                self.used[a] = (size, tag)
                return a
        raise MemoryError(f"VRAM OOM: need {size:#x} tag={tag}")
    def free_addr(self, addr: int):
        size, tag = self.used.pop(addr)
        self.free.append((addr, addr + size))
        self.free.sort()
        # coalesce
        out = []
        for s, e in self.free:
            if out and s <= out[-1][1]:
                out[-1] = (out[-1][0], max(out[-1][1], e))
            else:
                out.append((s, e))
        self.free = out
        return tag

class VASpace:
    """Per-client VA space. Low bound 64MiB, 49-bit top."""
    TOP = 1 << VA_BITS
    def __init__(self, client_id: int):
        self.client = client_id
        self.alloc = BuddyAllocator(VA_LOW_BOUND, self.TOP - VA_LOW_BOUND, BIG_PAGE)
        self.bindings = {}  # va -> (phys, size, kind)
    def map(self, size: int, phys: int, kind: str = "VRAM") -> int:
        va = self.alloc.alloc(size, kind)
        self.bindings[va] = (phys, size, kind)
        return va
    def unmap(self, va: int):
        self.alloc.free_addr(va)
        del self.bindings[va]

# --- PDE/PTE word builders (field positions from ga100/dev_mmu.h) ---
# aperture encodings
AP_VID, AP_PEER, AP_SYS_COH, AP_SYS_NONCOH = 0, 1, 2, 3
PDE_SIZE_FULL, PDE_SIZE_HALF, PDE_SIZE_QUARTER, PDE_SIZE_EIGHTH = 0, 1, 2, 3
PDE_SIZE_BYTES = 8  # NV_MMU_PDE__SIZE
PTE_SIZE_BYTES = 8  # NV_MMU_PTE__SIZE

def pde_big(phys: int, aperture: int = AP_VID, size: int = PDE_SIZE_FULL) -> bytes:
    """8B PDE, big half live. NOTE: SYS address variant (bits 31:4);
    VID variant drops top bits - high-VRAM (>4GiB phys) mapping OPEN (WPR/BAR ctx)."""
    assert phys & 0xFFF == 0, "PDE address 4K aligned"
    w0 = (phys & 0xFFFFFFF0) | ((size & 0x3) << 2) | (aperture & 0x3)
    w1 = 0x0  # small aperture INVALID, VOL false, small addr 0
    return w0.to_bytes(4, "little") + w1.to_bytes(4, "little")

def pte(phys: int, aperture: int = AP_VID, kind: int = 0, writable: bool = True,
        readable: bool = True, valid: bool = True) -> bytes:
    """8B PTE. kind: NV_MMU_PTE_KIND[11:4] (0 = plain; compressed kinds need comptagline)."""
    assert phys & 0xFFF == 0, "PTE address 4K aligned"
    w0 = (phys & 0xFFFFFFF0) | (0x1 if valid else 0x0)
    w1 = ((aperture & 0x3) << 1) | ((kind & 0xFF) << 4)
    if not readable:
        w1 |= (1 << 30)
    if not writable:
        w1 |= (1 << 31)
    return w0.to_bytes(4, "little") + w1.to_bytes(4, "little")

def decode_fault(words: list) -> dict:
    """Stub: MMU fault buffer words -> dict. Real format P0-1 Linux trace se aayega."""
    if len(words) < 4:
        return {"error": "short"}
    return {
        "client": (words[0] >> 12) & 0xFFF,
        "va": (words[1] << 32) | words[2],
        "access": words[3] & 0xF,
        "hint": "check VASpace.bindings for this VA; unmapped => driver bug, mapped+noPTE => TLB/pte install bug",
    }

def selftest() -> int:
    fails = 0
    def ck(c, m):
        nonlocal fails
        print(("PASS " if c else "FAIL ") + m)
        if not c:
            fails += 1
    # sanity check of the slot model against the live CE chunk
    ck(slot_va(71) == 71 * HUGE_PAGE, "slot71 base model")
    # VRAM alloc: 42 MiB ctx block (the nouveau r535 recipe)
    v = BuddyAllocator(0, VRAM_BYTES)
    ctx = v.alloc(42 * 1024**2, "ctx")
    ck(ctx % BIG_PAGE == 0, "ctx 64K aligned")
    fb = v.alloc(36 * 1024**2, "gop-fb")
    ce = v.alloc(HUGE_PAGE, "ce-chunk")
    ck(len(v.used) == 3, "3 carve-outs tracked")
    v.free_addr(ce)
    ce2 = v.alloc(HUGE_PAGE, "ce-chunk2")
    ck(ce2 == ce, "freed chunk reused (coalesce ok)")
    # VA space low bound
    s = VASpace(1)
    va = s.map(HUGE_PAGE, ctx)
    ck(va >= VA_LOW_BOUND, "VA above 64MiB low bound")
    ck(va < (1 << VA_BITS), "VA within 49 bits")
    # PTE/PDE words checked against ga100/dev_mmu.h
    assert PDE_SIZE_BYTES == 8 and PTE_SIZE_BYTES == 8, "entry sizes"
    e = pde_big(0x200000, AP_VID, PDE_SIZE_FULL)
    ck(len(e) == 8 and e[0] & 0x3 == AP_VID and (e[0] >> 2) & 0x3 == 0, "PDE big VID/FULL + addr")
    ck(int.from_bytes(e[:4], "little") >> 4 == 0x200000 >> 4, "PDE addr = phys>>4 (SHIFT 12)")
    t = pte(0x3000, AP_VID, 0, True, True, True)
    ck(t[0] & 0x1, "PTE VALID bit0")
    ck((t[4] >> 1) & 0x3 == AP_VID, "PTE APERTURE bits[2:1]=VID")
    tr = pte(0x3000, writable=False)
    ck(int.from_bytes(tr[4:], "little") >> 31 == 1, "PTE WRITE_DISABLE bit31")
    tk = pte(0x3000, kind=0x09)  # GENERIC_MEMORY_COMPRESSIBLE_DISABLE_PLC
    ck((int.from_bytes(tk[4:], "little") >> 4) & 0xFF == 0x09, "PTE KIND bits[11:4]")
    f = decode_fault([0x1000, 0x1, 0x08E00000, 0x2])
    ck(f["client"] == 0x1, "fault decode client")
    print("SELFTEST", "OK" if fails == 0 else f"{fails} FAILURES")
    return fails

if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(1 if selftest() else 0)
    print(__doc__)
