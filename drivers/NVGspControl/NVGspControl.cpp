#include "NVGspControl.hpp"
#include "../NVGspCore/NVGspExec.hpp"
#include "../NVGspCore/NVGspKernelApi.hpp"
#include "../NVGspCore/NVGspBooterUnloadBlob.hpp"
#include "../NVGspCore/NVGspGrContext.hpp"
#include "../NVGspCore/NVGspDisplayLut.hpp"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

#define super IOService
OSDefineMetaClassAndStructors(NVGspControl, IOService)

namespace {
// 0.117.0 (B7): BAR0 accesses reuse one mapping (the kext's persistent
// doorbell map, published in start()) instead of creating and tearing
// down a 16 MiB device mapping per PRAMIN access / doorbell. Callers keep
// their map->release(): sharedBar0Map() hands out a retained reference.
IOMemoryMap *gBar0Map = nullptr;
IOPCIDevice *gBar0Pci = nullptr;

IOMemoryMap *sharedBar0Map(IOPCIDevice *pci) {
    if (!pci) return nullptr;
    IOMemoryMap *cached = gBar0Map;
    if (cached && gBar0Pci == pci) {
        cached->retain();
        return cached;
    }
    return pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
}
}  // namespace


namespace {
constexpr IOByteCount kUsableFbSizeMb = 0x001183A4;
constexpr IOByteCount kVgaWorkspaceBase = 0x00625F04;
constexpr IOByteCount kSec2Ucode3Fuse = 0x00824148;
constexpr IOByteCount kGspUcode9Fuse = 0x008241e0;
constexpr IOByteCount kPromBase = 0x00300000;
constexpr size_t kPromBytes = 0x100000;
constexpr IOByteCount kBoot0 = 0;

bool read32(IOMemoryMap *map, IOByteCount offset, UInt32 *value);

struct SequencerResult {
    UInt32 commands, writes, modifies, polls, delays, stores;
    UInt32 coreReset, coreStart, coreHalt, coreResume;
    UInt32 failedIndex, failedOpcode, failedRegister, lastValue;
};

struct Bar0Io {
    IOMemoryMap *map;
    bool read(uint32_t offset, uint32_t *value) { return read32(map, offset, value); }
    bool write(uint32_t offset, uint32_t value) {
        if (!map || offset > map->getLength() || map->getLength() - offset < 4) return false;
        *reinterpret_cast<volatile UInt32 *>(map->getVirtualAddress() + offset) = value;
        OSSynchronizeIO();
        return true;
    }
    void delay(uint32_t usec) { IODelay(usec); }
};

bool resetPulse(Bar0Io &io, uint32_t engine, uint32_t hwcfg2) {
    uint32_t scratch = 0;
    if (!io.write(engine, 1)) return false;
    for (unsigned i = 0; i < 10; ++i) if (!io.read(engine, &scratch)) return false;
    if (!io.write(engine, 0)) return false;
    for (unsigned i = 0; i < 10; ++i) if (!io.read(engine, &scratch)) return false;
    for (unsigned i = 0; i < 5000; ++i) {
        if (io.read(hwcfg2, &scratch) && !(scratch & (1U << 12))) return true;
        IODelay(10);
    }
    return false;
}

bool read32(IOMemoryMap *map, IOByteCount offset, UInt32 *value) {
    if (!map || !value || offset > map->getLength() ||
        map->getLength() - offset < sizeof(UInt32)) return false;
    const volatile UInt32 *reg = reinterpret_cast<const volatile UInt32 *>(
        map->getVirtualAddress() + offset);
    *value = *reg;
    return true;
}

struct PraminPteResult {
    UInt32 windowBefore;
    UInt32 windowProgrammed;
    UInt32 windowObserved;
    UInt32 windowRestored;
    UInt64 observedPte;
};

bool praminPteAccess(IOPCIDevice *pci, UInt64 physicalAddress,
                     UInt64 *pte, bool write, bool validLast,
                     PraminPteResult *result) {
    if (!pci || !pte || !result || (physicalAddress & 7) ||
        (physicalAddress >> 16) > 0x00ffffffULL)
        return false;
    bzero(result, sizeof(*result));
    IOMemoryMap *map = sharedBar0Map(pci);
    if (!map || map->getLength() < 0x00700000 +
        (physicalAddress & 0xffff) + sizeof(UInt64)) {
        if (map) map->release();
        return false;
    }
    Bar0Io bar0{map};
    const UInt32 dataOffset = 0x00700000 +
        static_cast<UInt32>(physicalAddress & 0xffff);
    bool ok = bar0.read(0x1700, &result->windowBefore);
    result->windowProgrammed = (result->windowBefore & 0xfc000000U) |
        static_cast<UInt32>((physicalAddress >> 16) & 0x00ffffffU);
    ok = ok && bar0.write(0x1700, result->windowProgrammed) &&
        bar0.read(0x1700, &result->windowObserved) &&
        ((result->windowObserved & 0x03ffffffU) ==
         (result->windowProgrammed & 0x03ffffffU));
    UInt32 lo = 0, hi = 0;
    if (ok && write) {
        __builtin_memcpy(&lo, pte, 4);
        __builtin_memcpy(&hi, reinterpret_cast<const UInt8 *>(pte) + 4, 4);
        // Install address/kind before VALID. During removal clear VALID first.
        ok = validLast ? (bar0.write(dataOffset + 4, hi) &&
                          bar0.write(dataOffset, lo))
                       : (bar0.write(dataOffset, lo) &&
                          bar0.write(dataOffset + 4, hi));
    }
    ok = ok && bar0.read(dataOffset, &lo) &&
        bar0.read(dataOffset + 4, &hi);
    __builtin_memcpy(&result->observedPte, &lo, 4);
    __builtin_memcpy(reinterpret_cast<UInt8 *>(&result->observedPte) + 4,
                     &hi, 4);
    if (!write && ok) *pte = result->observedPte;
    if (write) ok = ok && result->observedPte == *pte;
    const bool restored = bar0.write(0x1700, result->windowBefore) &&
        bar0.read(0x1700, &result->windowRestored) &&
        result->windowRestored == result->windowBefore;
    map->release();
    return ok && restored;
}

bool praminZeroRange(IOPCIDevice *pci, UInt64 physicalAddress, UInt64 bytes) {
    if (!pci || !bytes || (physicalAddress & 3) || (bytes & 3) ||
        bytes > (16ULL * 1024ULL * 1024ULL) ||
        ((physicalAddress + bytes - 1) >> 16) > 0x00ffffffULL)
        return false;
    IOMemoryMap *map = sharedBar0Map(pci);
    if (!map || map->getLength() < 0x00710000) {
        if (map) map->release();
        return false;
    }
    Bar0Io bar0{map};
    UInt32 windowBefore = 0, observed = 0, restored = 0;
    bool ok = bar0.read(0x1700, &windowBefore);
    UInt64 cursor = physicalAddress;
    UInt64 remaining = bytes;
    while (ok && remaining) {
        const UInt32 low = static_cast<UInt32>(cursor & 0xffffULL);
        UInt64 chunk = 0x10000ULL - low;
        if (chunk > remaining) chunk = remaining;
        const UInt32 programmed = (windowBefore & 0xfc000000U) |
            static_cast<UInt32>((cursor >> 16) & 0x00ffffffU);
        ok = bar0.write(0x1700, programmed) &&
            bar0.read(0x1700, &observed) &&
            ((observed & 0x03ffffffU) == (programmed & 0x03ffffffU));
        for (UInt64 offset = 0; ok && offset < chunk; offset += 4)
            ok = bar0.write(0x00700000 + low +
                            static_cast<UInt32>(offset), 0);
        cursor += chunk;
        remaining -= chunk;
    }
    const bool restoreOk = bar0.write(0x1700, windowBefore) &&
        bar0.read(0x1700, &restored) && restored == windowBefore;
    map->release();
    return ok && restoreOk;
}

// 0.38.0: write `count` consecutive 4 KiB PTEs (template | page<<8,
// page = firstPage + i) into one page table through a single PRAMIN
// window; firstPage == ~0 writes zero (removal, VALID cleared first as
// the low word). The table must not cross a 64 KiB window. Verifies
// the last PTE by readback.
bool praminWritePteRun(IOPCIDevice *pci, UInt64 pteAddress, UInt64 firstPage,
                       UInt32 count, UInt64 templateBits,
                       UInt32 stride = 8, UInt64 pageStep = 1) {
    if (!pci || !count || (pteAddress & 7) || (stride != 8 && stride != 16) ||
        (pteAddress & 0xffffULL) + UInt64(count) * stride > 0x10000ULL ||
        (pteAddress >> 16) > 0x00ffffffULL)
        return false;
    IOMemoryMap *map = sharedBar0Map(pci);
    if (!map || map->getLength() < 0x00710000) {
        if (map) map->release();
        return false;
    }
    Bar0Io bar0{map};
    UInt32 windowBefore = 0, observed = 0, restored = 0;
    bool ok = bar0.read(0x1700, &windowBefore);
    const UInt32 programmed = (windowBefore & 0xfc000000U) |
        static_cast<UInt32>((pteAddress >> 16) & 0x00ffffffU);
    ok = ok && bar0.write(0x1700, programmed) &&
        bar0.read(0x1700, &observed) &&
        ((observed & 0x03ffffffU) == (programmed & 0x03ffffffU));
    const UInt32 base = 0x00700000 +
        static_cast<UInt32>(pteAddress & 0xffffULL);
    UInt64 last = 0;
    for (UInt32 i = 0; ok && i < count; ++i) {
        last = firstPage == ~0ULL ? 0 : (templateBits |
            (((firstPage + i * pageStep) & 0x01ffffffULL) << 8));
        const UInt32 lo = static_cast<UInt32>(last);
        const UInt32 hi = static_cast<UInt32>(last >> 32);
        const UInt32 at = base + i * stride;
        ok = firstPage == ~0ULL
            ? (bar0.write(at, lo) && bar0.write(at + 4, hi))
            : (bar0.write(at + 4, hi) && bar0.write(at, lo));
        // Dual PDE: upper 8 bytes (small-PT half) stay zero.
        if (ok && stride == 16)
            ok = bar0.write(at + 8, 0) && bar0.write(at + 12, 0);
    }
    UInt32 rlo = 0, rhi = 0;
    const UInt32 lastAt = base + (count - 1) * stride;
    ok = ok && bar0.read(lastAt, &rlo) && bar0.read(lastAt + 4, &rhi) &&
        ((UInt64(rhi) << 32) | rlo) == last;
    const bool restoreOk = bar0.write(0x1700, windowBefore) &&
        bar0.read(0x1700, &restored) && restored == windowBefore;
    map->release();
    return ok && restoreOk;
}

// 0.43.0: single-PRAMIN-window helpers (range must stay inside one
// 64 KiB window). praminHashRange: FNV-1a over bytes, same algorithm as
// the BAR1 GOP hash. praminWritePattern: head dwords, then fillCount
// copies of fill, then tail dwords, contiguous from physicalAddress.
bool praminHashRange(IOPCIDevice *pci, UInt64 physicalAddress, UInt32 bytes,
                     UInt64 *hashOut) {
    if (!pci || !hashOut || !bytes || (physicalAddress & 3) || (bytes & 3) ||
        (physicalAddress & 0xffffULL) + bytes > 0x10000ULL)
        return false;
    IOMemoryMap *map = sharedBar0Map(pci);
    if (!map || map->getLength() < 0x00710000) {
        if (map) map->release();
        return false;
    }
    Bar0Io bar0{map};
    UInt32 windowBefore = 0, observed = 0, restored = 0;
    bool ok = bar0.read(0x1700, &windowBefore);
    const UInt32 programmed = (windowBefore & 0xfc000000U) |
        static_cast<UInt32>((physicalAddress >> 16) & 0x00ffffffU);
    ok = ok && bar0.write(0x1700, programmed) &&
        bar0.read(0x1700, &observed) &&
        ((observed & 0x03ffffffU) == (programmed & 0x03ffffffU));
    const UInt32 base = 0x00700000 +
        static_cast<UInt32>(physicalAddress & 0xffffULL);
    UInt64 hash = 14695981039346656037ULL;
    for (UInt32 off = 0; ok && off < bytes; off += 4) {
        UInt32 v = 0;
        ok = bar0.read(base + off, &v);
        for (UInt32 b = 0; b < 4; ++b) {
            hash ^= (v >> (b * 8)) & 0xffU;
            hash *= 1099511628211ULL;
        }
    }
    const bool restoreOk = bar0.write(0x1700, windowBefore) &&
        bar0.read(0x1700, &restored) && restored == windowBefore;
    map->release();
    if (ok && restoreOk) *hashOut = hash;
    return ok && restoreOk;
}

bool praminWritePattern(IOPCIDevice *pci, UInt64 physicalAddress,
                        const UInt32 *head, UInt32 headCount, UInt32 fill,
                        UInt32 fillCount, const UInt32 *tail,
                        UInt32 tailCount) {
    const UInt64 bytes = 4ULL * (headCount + fillCount + tailCount);
    if (!pci || !bytes || (physicalAddress & 3) ||
        (physicalAddress & 0xffffULL) + bytes > 0x10000ULL)
        return false;
    IOMemoryMap *map = sharedBar0Map(pci);
    if (!map || map->getLength() < 0x00710000) {
        if (map) map->release();
        return false;
    }
    Bar0Io bar0{map};
    UInt32 windowBefore = 0, observed = 0, restored = 0;
    bool ok = bar0.read(0x1700, &windowBefore);
    const UInt32 programmed = (windowBefore & 0xfc000000U) |
        static_cast<UInt32>((physicalAddress >> 16) & 0x00ffffffU);
    ok = ok && bar0.write(0x1700, programmed) &&
        bar0.read(0x1700, &observed) &&
        ((observed & 0x03ffffffU) == (programmed & 0x03ffffffU));
    UInt32 at = 0x00700000 + static_cast<UInt32>(physicalAddress & 0xffffULL);
    for (UInt32 i = 0; ok && i < headCount; ++i, at += 4)
        ok = bar0.write(at, head[i]);
    for (UInt32 i = 0; ok && i < fillCount; ++i, at += 4)
        ok = bar0.write(at, fill);
    for (UInt32 i = 0; ok && i < tailCount; ++i, at += 4)
        ok = bar0.write(at, tail[i]);
    UInt32 last = 0;
    ok = ok && bar0.read(at - 4, &last) &&
        last == (tailCount ? tail[tailCount - 1]
                           : (fillCount ? fill : head[headCount - 1]));
    const bool restoreOk = bar0.write(0x1700, windowBefore) &&
        bar0.read(0x1700, &restored) && restored == windowBefore;
    map->release();
    return ok && restoreOk;
}

// 0.45.0: BAR1 remap for the GOP surface. After GSP boot BAR1 runs in
// virtual mode (NV_PBUS_BAR1_BLOCK 0x1704: PTR 27:0, TARGET 29:28,
// MODE 31) with nothing mapped at BAR1 offset 0, so CPU/WindowServer
// writes no longer reach VRAM. Walk instance -> PDB (RAMIN +0x200) ->
// PD3[0] -> PD2[0] -> PD1[0] -> PD0 (ver2: PDE aperture 2:1, addr 32:8
// <<12). If PD0[0..17] are all empty, install 18 x 2 MiB PTEs (BAR1 VA
// [0,36 MiB) -> VRAM [0,36 MiB)), then a full TLB invalidate through
// NV_VIRTUAL_FUNCTION_PRIV_MMU_INVALIDATE (0xBB30B0: ALL_VA|ALL_PDB|
// TRIGGER, poll TRIGGER clear) as kgmmuCommitTlbInvalidate_TU102 does.
struct Bar1Remap {
    UInt32 block, pdbLo, pdbHi, invalidateFinal;
    UInt64 inst, pdb, pd3e, pd2e, pd1e, pd0Table, pd0First, pd0Last;
    UInt32 usedSlots, stage;  // stage: how far the walk got
    bool installed, invalidated;
    // 0.47.0: PD0[0] big (64K) page table owned by GSP.
    UInt64 bigPt, bigValidMask, bigFirstValid;
    UInt32 bigFilled, hugeFilled;
    // 0.49.0: the live BAR1 bind is NV_VIRTUAL_FUNCTION_PRIV_BAR1_BLOCK
    // (VF 0xF40 -> BAR0 0xB80F40), not legacy 0x1704.
    UInt32 vfBlockBefore, vfBlockAfter, bindStatus;
    bool physicalBound;
};

static bool pdeNext(IOPCIDevice *pci, UInt64 table, UInt64 *entry,
                    UInt64 *next) {
    PraminPteResult r{};
    if (!praminPteAccess(pci, table, entry, false, false, &r)) return false;
    if (((*entry >> 1) & 3) != 1) return false;  // not a vidmem PDE
    *next = ((*entry >> 8) & 0x1ffffffULL) << 12;
    return *next != 0;
}

bool bar1RemapGop(IOPCIDevice *pci, Bar1Remap *out) {
    bzero(out, sizeof(*out));
    IOMemoryMap *map = sharedBar0Map(pci);
    if (!map || map->getLength() < 0x00BB30B4) {
        if (map) map->release();
        return false;
    }
    Bar0Io bar0{map};
    bool ok = bar0.read(0x1704, &out->block);
    // MODE virtual, TARGET vidmem.
    ok = ok && (out->block >> 31) == 1 && ((out->block >> 28) & 3) == 0;
    out->inst = UInt64(out->block & 0x0fffffffU) << 12;
    UInt64 pdbWord = 0;
    PraminPteResult r{};
    ok = ok && praminPteAccess(pci, out->inst + 0x200, &pdbWord, false, false,
                               &r);
    out->pdbLo = static_cast<UInt32>(pdbWord);
    out->pdbHi = static_cast<UInt32>(pdbWord >> 32);
    out->pdb = (UInt64(out->pdbHi) << 32) | (out->pdbLo & 0xfffff000U);
    ok = ok && (out->pdbLo & 3) == 0 && out->pdb;
    if (ok) out->stage = 1;
    UInt64 pd2 = 0, pd1 = 0;
    ok = ok && pdeNext(pci, out->pdb, &out->pd3e, &pd2);
    if (ok) out->stage = 2;
    ok = ok && pdeNext(pci, pd2, &out->pd2e, &pd1);
    if (ok) out->stage = 3;
    ok = ok && pdeNext(pci, pd1, &out->pd1e, &out->pd0Table);
    if (ok) out->stage = 4;
    for (UInt32 i = 0; ok && i < 18; ++i) {
        UInt64 lo = 0, hi = 0;
        PraminPteResult a{}, b{};
        ok = praminPteAccess(pci, out->pd0Table + i * 16, &lo, false, false,
                             &a) &&
            praminPteAccess(pci, out->pd0Table + i * 16 + 8, &hi, false,
                            false, &b);
        if (i == 0) out->pd0First = lo;
        if (i == 17) out->pd0Last = lo;
        if (lo || hi) ++out->usedSlots;
    }
    if (ok) out->stage = 5;
    // 0.49.0: 0.47/0.48 PTE edits proved BAR1 traffic never reaches the
    // walked tables (reads = 0xBAD0ACxx). CPU-RM (us) owns BAR1 in the
    // GSP-client split, so bind BAR1 back to PHYSICAL mode exactly as
    // kbusBar1InstBlkBind_TU102 does for bIsModePhysical: write
    // NV_VIRTUAL_FUNCTION_PRIV_BAR1_BLOCK = MODE_PHYSICAL|TARGET_VID|PTR 0,
    // then poll BIND_STATUS (0xB80F50) BAR1_PENDING 0:0 / OUTSTANDING 1:1.
    bool vfOk = bar0.read(0x00B80F40, &out->vfBlockBefore);
    if (vfOk && (out->vfBlockBefore >> 31) == 1) {
        out->stage = 6;
        if (bar0.write(0x00B80F40, 0)) {
            UInt32 st = 0;
            for (UInt32 i = 0; i < 100000; ++i) {
                if (!bar0.read(0x00B80F50, &st)) break;
                if ((st & 3) == 0) { out->physicalBound = true; break; }
                IODelay(1);
            }
            out->bindStatus = st;
            out->stage = 7;
        }
    }
    // 0.161.1: already physical counts as bound. Since the 0.146.6 early
    // BAR1 pass, the second finishBar1() found the bind it had made itself
    // and reported "not installed", so bar1-live went No and CPU-visible
    // VRAM allocations were refused. bar1-live still needs the 2 MiB hash.
    if (vfOk && (out->vfBlockBefore >> 31) == 0) out->physicalBound = true;
    bar0.read(0x00B80F40, &out->vfBlockAfter);
    out->installed = out->physicalBound;
    map->release();
    return ok;
}

// 0.57.0: write `count` dwords at physicalAddress, chunked per 64 KiB
// PRAMIN window.
bool praminWriteWords(IOPCIDevice *pci, UInt64 physicalAddress,
                      const UInt32 *words, UInt32 count) {
    while (count) {
        const UInt64 room = (0x10000ULL - (physicalAddress & 0xffffULL)) / 4;
        const UInt32 n = static_cast<UInt32>(room < count ? room : count);
        if (!praminWritePattern(pci, physicalAddress, words, n, 0, 0,
                                nullptr, 0))
            return false;
        physicalAddress += UInt64(n) * 4;
        words += n;
        count -= n;
    }
    return true;
}

// 0.40.0: read-only GR hang diagnostics under one BAR0 map: VF MMU
// fault ADDR_LO/HI, INST_LO/HI, INFO, STATUS (0xBB3080..94, tu102/ga102
// dev_vm.h at FULL_PHYS_OFFSET 0xB80000), PGRAPH status/intr/exception
// (0x400700/0x400100/0x400108) and FECS mailbox0/1, current/new ctx,
// 0x409c18 (nouveau gf100 offsets).
constexpr UInt32 kGrDiagCount = 14;
bool readGrDiag(IOPCIDevice *pci, UInt32 *out) {
    static const UInt32 kOffsets[kGrDiagCount] = {
        0xBB3080, 0xBB3084, 0xBB3088, 0xBB308C, 0xBB3090, 0xBB3094,
        0x400700, 0x400100, 0x400108,
        0x409800, 0x409804, 0x409b00, 0x409b04, 0x409c18};
    IOMemoryMap *map = pci ? sharedBar0Map(pci) : nullptr;
    bool ok = map && map->getLength() >= 0x00BB3098;
    if (ok) {
        Bar0Io bar0{map};
        for (UInt32 i = 0; i < kGrDiagCount && ok; ++i)
            ok = bar0.read(kOffsets[i], &out[i]);
    }
    if (map) map->release();
    return ok;
}

bool runCpuSequencer(Bar0Io &io, UInt8 *message, UInt32 messageBytes,
                     UInt32 chipId0, UInt64 libosArgsBus,
                     SequencerResult *result) {
    if (!message || !result || messageBytes < 120) return false;
    UInt32 *params = reinterpret_cast<UInt32 *>(message + 80);
    const UInt32 bufferSize = params[0], commandEnd = params[1];
    UInt32 *saved = params + 2;
    UInt32 *commands = params + 10;
    if (!bufferSize || commandEnd >= bufferSize ||
        120ULL + static_cast<UInt64>(commandEnd) * 4 > messageBytes)
        return false;

    SequencerResult value{};
    UInt32 i = 0;
    while (i < commandEnd) {
        const UInt32 commandIndex = i;
        const UInt32 opcode = commands[i++];
        UInt32 payload = 0;
        switch (opcode) {
            case 0: payload = 2; break;
            case 1: payload = 3; break;
            case 2: payload = 5; break;
            case 3: payload = 1; break;
            case 4: payload = 2; break;
            case 5: case 6: case 7: case 8: payload = 0; break;
            default: value.failedIndex = commandIndex; value.failedOpcode = opcode;
                     *result = value; return false;
        }
        if (i + payload > commandEnd) {
            value.failedIndex = commandIndex; value.failedOpcode = opcode;
            *result = value; return false;
        }
        UInt32 reg = 0, current = 0;
        bool ok = true;
        switch (opcode) {
            case 0:
                reg = commands[i]; ok = io.write(reg, commands[i + 1]);
                ++value.writes; break;
            case 1:
                reg = commands[i]; ok = io.read(reg, &current) &&
                    io.write(reg, (current & ~commands[i + 1]) | commands[i + 2]);
                ++value.modifies; break;
            case 2:
                reg = commands[i];
                ok = false;
                for (UInt32 retry = 0; retry < 200000; ++retry) {
                    if (!io.read(reg, &current)) break;
                    if ((current & commands[i + 1]) == commands[i + 2]) {
                        ok = true; break;
                    }
                    IODelay(10);
                }
                ++value.polls; break;
            case 3:
                IODelay(commands[i]); ++value.delays; break;
            case 4:
                reg = commands[i];
                ok = commands[i + 1] < 8 && io.read(reg, &saved[commands[i + 1]]);
                ++value.stores; break;
            case 5:
                ok = resetPulse(io, 0x001103c0, 0x001100f4) &&
                     io.write(0x00111668, 0) &&
                     nvgsp::falconPoll(io, 0x00111668, 1, 1, 200000, 10) &&
                     io.write(0x00110084, chipId0) &&
                     io.read(nvgsp::falcon::kFbifCtl, &current) &&
                     io.write(nvgsp::falcon::kFbifCtl, current | (1U << 7)) &&
                     io.write(nvgsp::falcon::kDmaCtl, 0);
                value.coreReset = ok; break;
            case 6:
                ok = io.read(nvgsp::falcon::kCpuCtl, &current) &&
                     io.write((current & (1U << 6)) ? nvgsp::falcon::kCpuCtlAlias
                                                    : nvgsp::falcon::kCpuCtl,
                              1U << 1);
                value.coreStart = ok; break;
            case 7:
                ok = nvgsp::falconPoll(io, nvgsp::falcon::kCpuCtl, 1U << 4,
                                       1U << 4, 200000, 10);
                value.coreHalt = ok; break;
            case 8: {
                UInt32 secCpu = 0, secMailbox = ~0U;
                ok = resetPulse(io, 0x001103c0, 0x001100f4) &&
                     io.write(0x00111668, 0x111) &&
                     io.write(nvgsp::falcon::kMailbox0,
                              static_cast<UInt32>(libosArgsBus)) &&
                     io.write(nvgsp::falcon::kMailbox1,
                              static_cast<UInt32>(libosArgsBus >> 32)) &&
                     io.read(nvgsp::sec2::kCpuCtl, &secCpu) &&
                     io.write((secCpu & (1U << 6)) ? nvgsp::sec2::kCpuCtlAlias
                                                   : nvgsp::sec2::kCpuCtl,
                              1U << 1) &&
                     nvgsp::falconPoll(io, 0x001180f8, 1U << 26, 1U << 26,
                                       200000, 10) &&
                     io.read(nvgsp::sec2::kMailbox0, &secMailbox) &&
                     secMailbox == 0;
                value.lastValue = secMailbox;
                value.coreResume = ok; break;
            }
        }
        if (!ok) {
            value.failedIndex = commandIndex;
            value.failedOpcode = opcode;
            value.failedRegister = reg;
            value.lastValue = current;
            *result = value;
            return false;
        }
        i += payload;
        ++value.commands;
    }
    *result = value;
    return i == commandEnd;
}
}

void NVGspControl::gspDoorbell(void *ctx) {
    auto *self = static_cast<NVGspControl *>(ctx);
    if (!self || !self->doorbellMap_) return;
    volatile UInt32 *reg = reinterpret_cast<volatile UInt32 *>(
        static_cast<uintptr_t>(self->doorbellMap_->getVirtualAddress()) + 0x110c00);
    OSSynchronizeIO();
    *reg = 0;
}

// 0.130.0 / 0.131.0: Resizable BAR done by the kext. macOS IOPCIFamily only
// sizes BARs up to 1 GiB: with BIOS ReBAR on it drops BAR1 entirely (live 26
// Sep, at 16/8/2 GiB). So macOS keeps its 256 MiB BAR1 (OpenCore
// ResizeAppleGpuBars 8) and, before anything maps BAR1, this code does what
// Linux pci_resize_resource does: GPU decode off, ReBAR control (ext cap 0x15)
// BAR1 size, BAR1 placed above DRAM, the root port's (GPP0) 64-bit prefetch
// window around BAR1 + BAR3, decode on.
// 0.131.0 after the 0.130.3 boot loop: BAR1 had been put at 0x18_0000_0000,
// but DRAM ends at TOM2 = 0x18_9000_0000 (96 GiB + the 2.25 GiB PCI hole
// hoisted above 4 GiB), so its first 2.25 GiB overlapped RAM (the offset-0
// mismatch was RAM being read) and the console/NVDisplay/GSP got RAM as
// "VRAM" -> platform reset. Now:
//  - BAR1 base = align_up(max(TOM2 (AMD MSR C001_001D when SYSCFG.MtrrTom2En),
//    macOS's own BAR1/BAR3 ends), size); BAR3 stays where macOS put it.
//  - verify: new BAR1 + 0 reads what old BAR1 + 0 read, far end decoded.
//  - boot guard: NVRAM nvgsp-rebar-boot is set (flash forced) before the
//    change and cleared 90 s after a good GSP chain; if it is still set at
//    the next start, the previous resize never came up -> skip.
//  - the kernel console is disabled across the move and re-pointed after
//    (kPEBaseAddressChange); saveDeviceState() so IOPCIFamily restores ours.
// NVRAM nvgsp-rebar = BAR1 size in GiB (e.g. "16"); absent/0 = off.
static IORegistryEntry *nvramOptions() {
    return IORegistryEntry::fromPath("/options", gIODTPlane);
}

static UInt32 nvramU32(const char *key) {
    IORegistryEntry *options = nvramOptions();
    UInt32 v = 0;
    if (options) {
        OSObject *o = options->getProperty(key);
        OSData *d = OSDynamicCast(OSData, o);
        OSString *str = OSDynamicCast(OSString, o);
        const char *p = d ? static_cast<const char *>(d->getBytesNoCopy()) : str ? str->getCStringNoCopy() : nullptr;
        const unsigned n = d ? d->getLength() : str ? str->getLength() : 0;
        for (unsigned i = 0; p && i < n && p[i] >= '0' && p[i] <= '9' && v < 100000; ++i)
            v = v * 10 + static_cast<UInt32>(p[i] - '0');
        options->release();
    }
    return v;
}

// value nullptr = delete; the write is flushed to flash right away (a reset
// later in this boot must still see it)
static void nvramSetSync(const char *key, const char *value) {
    IORegistryEntry *options = nvramOptions();
    if (!options) return;
    if (value) {
        OSData *d = OSData::withBytes(value, static_cast<unsigned>(strlen(value)));
        if (d) { options->setProperty(key, d); d->release(); }
    } else {
        options->removeProperty(key);
    }
    OSData *one = OSData::withBytes("1", 1);
    if (one) { options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", one); one->release(); }
    options->release();
}

// AMD TOM2 (top of DRAM above 4 GiB), 0 if not AMD / not enabled
static UInt64 amdTom2() {
    UInt32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    if (b != 0x68747541 || d != 0x69746e65 || c != 0x444d4163) return 0;   // AuthenticAMD
    auto rd = [](UInt32 msr) {
        UInt32 lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return (static_cast<UInt64>(hi) << 32) | lo;
    };
    if (!(rd(0xC0010010) & (1ULL << 21))) return 0;   // SYSCFG.MtrrTom2En
    return rd(0xC001001D) & 0x0000ffffff800000ULL;   // bits 47:23
}

IODeviceMemory *NVGspControl::bar1Dev() const {
    return bar1Mem_ ? bar1Mem_ : pci_ ? pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1) : nullptr;
}

IODeviceMemory *NVGspControl::bar3Dev() const {
    return bar3Mem_ ? bar3Mem_ : pci_ ? pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress3) : nullptr;
}

// Map only the first `bytes` of BAR1: a whole 16 GiB BAR1 kernel mapping per
// debug hash would cost gigabytes of page tables.
IOMemoryMap *NVGspControl::mapBar1Head(IOByteCount bytes) const {
    IODeviceMemory *b = bar1Dev();
    if (!b || !bytes) return nullptr;
    if (bytes > b->getLength()) bytes = b->getLength();
    return b->createMappingInTask(kernel_task, 0, kIOMapAnywhere, 0, bytes);
}

// 0.151.0: a ring submit used to cost ~13 us, nearly all of it PRAMIN
// window traffic: each access reads 0x1700, moves the window, reads it back,
// writes, reads the data back and restores the window, and every read is a
// PCIe round trip. BAR1 runs in physical mode (offset = VRAM address, ReBAR
// covers all of it), so the ring words are plain uncached loads and stores
// through small kernel windows made once.
volatile UInt32 *NVGspControl::bar1Ptr(UInt64 phys, UInt32 bytes) {
    if (!bar1Direct_ || !bar1Lock_ || (phys & 3) || !bytes) return nullptr;
    constexpr UInt64 kWin = 0x200000;
    const UInt64 base = phys & ~(kWin - 1);
    if (phys + bytes > base + kWin) return nullptr;
    IODeviceMemory *b1 = bar1Dev();
    if (!b1 || base + kWin > b1->getLength()) return nullptr;
    IOMemoryMap *map = nullptr;
    IOLockLock(bar1Lock_);
    for (UInt32 i = 0; i < bar1Wins_ && !map; ++i)
        if (bar1Win_[i].base == base) map = bar1Win_[i].map;
    if (!map && bar1Wins_ < 16) {
        map = b1->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, base, kWin);
        if (map) bar1Win_[bar1Wins_++] = {base, map};
    }
    IOLockUnlock(bar1Lock_);
    if (!map) return nullptr;
    return reinterpret_cast<volatile UInt32 *>(map->getVirtualAddress() + (phys - base));
}

bool NVGspControl::ringWrite(UInt64 phys, const UInt32 *words, UInt32 count) {
    volatile UInt32 *p = bar1Ptr(phys, count * 4);
    if (!p) return praminWriteWords(pci_, phys, words, count);
    for (UInt32 i = 0; i < count; ++i) p[i] = words[i];
    OSSynchronizeIO();
    return true;
}

bool NVGspControl::ringRead(UInt64 phys, UInt32 *value) {
    volatile UInt32 *p = bar1Ptr(phys, 4);
    if (p) { *value = *p; return true; }
    UInt64 v = 0;
    PraminPteResult r{};
    if (!praminPteAccess(pci_, phys & ~7ULL, &v, false, false, &r)) return false;
    *value = static_cast<UInt32>(phys & 4 ? v >> 32 : v);
    return true;
}

bool NVGspControl::rebarProgram(IOPCIDevice *bridge, UInt32 ctrl, UInt64 bar1, UInt64 bar3,
                                UInt64 winBase, UInt64 winEnd) {
    const UInt16 cmd = pci_->configRead16(kIOPCIConfigCommand);
    pci_->configWrite16(kIOPCIConfigCommand, cmd & ~static_cast<UInt16>(2));   // memory decode off
    pci_->configWrite32(rebar_.ctrlOff, ctrl);
    const UInt32 lo1 = pci_->configRead32(0x14) & 0xf, lo3 = pci_->configRead32(0x1c) & 0xf;
    pci_->configWrite32(0x14, static_cast<UInt32>(bar1) | lo1);
    pci_->configWrite32(0x18, static_cast<UInt32>(bar1 >> 32));
    pci_->configWrite32(0x1c, static_cast<UInt32>(bar3) | lo3);
    pci_->configWrite32(0x20, static_cast<UInt32>(bar3 >> 32));
    // bridge: close the window first (base > limit), then set the upper
    // halves, then open it, so it never spans a stray range in between
    const UInt32 w = bridge->configRead32(0x24);
    bridge->configWrite32(0x24, 0x0000fff0 | (w & 0x000f000f));
    bridge->configWrite32(0x28, static_cast<UInt32>(winBase >> 32));
    bridge->configWrite32(0x2c, static_cast<UInt32>(winEnd >> 32));
    bridge->configWrite32(0x24, (static_cast<UInt32>(winEnd >> 16) & 0xfff00000) |
                                (static_cast<UInt32>(winBase >> 16) & 0xfff0) | (w & 0x000f000f));
    pci_->configWrite16(kIOPCIConfigCommand, cmd);
    return (pci_->configRead32(0x14) & ~0xfU) == static_cast<UInt32>(bar1) &&
           pci_->configRead32(0x18) == static_cast<UInt32>(bar1 >> 32);
}

static UInt64 fnvHash(const volatile UInt8 *p, unsigned n) {
    UInt64 h = 14695981039346656037ULL;
    for (unsigned i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

// FNV of 4 KiB at `off` of `mem`, ~0 if it cannot be mapped
static UInt64 hashDevPage(IODeviceMemory *mem, UInt64 off) {
    IOMemoryMap *mp = mem->createMappingInTask(kernel_task, 0, kIOMapAnywhere, off, 4096);
    if (!mp) return ~0ULL;
    const UInt64 h = fnvHash(reinterpret_cast<const volatile UInt8 *>(
        static_cast<uintptr_t>(mp->getVirtualAddress())), 4096);
    mp->release();
    return h;
}

void NVGspControl::rebarOkCallout(thread_call_param_t self, thread_call_param_t) {
    NVGspControl *d = static_cast<NVGspControl *>(self);
    nvramSetSync("nvgsp-rebar-boot", nullptr);
    d->setProperty("NVGspControl-rebar-guard-cleared", true);
}

bool NVGspControl::rebarResize() {
    UInt32 stage = 1;
    const UInt32 wantGiB = nvramU32("nvgsp-rebar");
    IOPCIDevice *bridge = nullptr;
    UInt32 cap = 0, entry = 8, capReg = 0, n = 0;
    UInt64 size = 0;
    if (!wantGiB) goto out;
    stage = 11;                                   // guard: last resize never came up
    if (nvramU32("nvgsp-rebar-boot")) {
        nvramSetSync("nvgsp-rebar-boot", nullptr);   // one skipped boot re-arms it
        goto out;
    }
    stage = 2;
    // own walk: IOPCIFamily's extendedFindPCICapability misses it on this GPU
    // (live 0.130.0: ReBAR ext cap sits at 0xbb0, 7th in the chain)
    for (UInt32 off = 0x100, i = 0; off >= 0x100 && off < 0x1000 && i < 64; ++i) {
        const UInt32 h = pci_->configRead32(off);
        if (h == 0xffffffffU || h == 0) break;
        if ((h & 0xffff) == 0x15) { cap = off; break; }
        off = h >> 20;
    }
    setProperty("NVGspControl-rebar-cap-off", cap, 32);
    if (!cap) goto out;
    stage = 3;
    {
        const UInt32 nbars = (pci_->configRead32(cap + 8) >> 5) & 7;
        for (UInt32 i = 0; i < nbars && i < 6; ++i)
            if ((pci_->configRead32(cap + 8 + 8 * i) & 7) == 1) entry = i;
    }
    if (entry == 8) goto out;
    stage = 4;
    rebar_.ctrlOff = cap + 8 + 8 * entry;
    capReg = pci_->configRead32(cap + 4 + 8 * entry);
    rebar_.ctrl = pci_->configRead32(rebar_.ctrlOff);
    setProperty("NVGspControl-rebar-cap", capReg, 32);
    setProperty("NVGspControl-rebar-ctrl-before", rebar_.ctrl, 32);
    while (n < 27 && (1ULL << (n + 1)) <= static_cast<UInt64>(wantGiB) * 1024) ++n;
    while (n && !(capReg & (1U << (n + 4)))) --n;
    size = 1ULL << (n + 20);
    if (size <= (256ULL << 20)) goto out;
    stage = 5;
    {
        IOService *b = pci_->getProvider();
        bridge = OSDynamicCast(IOPCIDevice, b ? b->getProvider() : nullptr);
    }
    if (!bridge) goto out;
    stage = 6;
    {
        IODeviceMemory *old1 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
        IODeviceMemory *old3 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress3);
        if (!old1 || !old3) goto out;
        const UInt64 oldBar1 = old1->getPhysicalAddress(), oldBar3 = old3->getPhysicalAddress();
        const UInt64 bar3Bytes = old3->getLength();
        const UInt32 oldW24 = bridge->configRead32(0x24), oldW28 = bridge->configRead32(0x28),
                     oldW2c = bridge->configRead32(0x2c);
        const UInt64 oldWinBase = (static_cast<UInt64>(oldW28) << 32) | ((oldW24 & 0xfff0) << 16);
        const UInt64 oldWinEnd = (static_cast<UInt64>(oldW2c) << 32) | (oldW24 & 0xfff00000) | 0xfffff;
        const UInt64 tom2 = amdTom2();
        setProperty("NVGspControl-rebar-tom2", tom2, 64);
        setProperty("NVGspControl-rebar-old-window", oldWinBase, 64);
        // Candidates (0.131.0): 1. what the firmware does with BIOS ReBAR on
        // (Windows AllocConfig 26 Sep: BAR1 0xF8_0000_0000 16 GiB, BAR3
        // 0xFC_0000_0000): top-down under the AMD reserved HT hole at
        // 0xFD_0000_0000, BAR3 right after BAR1. 2. just above DRAM/macOS's
        // BARs, BAR3 left where macOS put it. Each must clear TOM2.
        UInt64 floor = oldBar1 + old1->getLength();
        if (oldBar3 + bar3Bytes > floor) floor = oldBar3 + bar3Bytes;
        if (tom2 > floor) floor = tom2;
        struct Cand { UInt64 bar1, bar3; } cand[2] = {
            {(0xFD00000000ULL - size - bar3Bytes) & ~(size - 1), 0},
            {(floor + size - 1) & ~(size - 1), oldBar3},
        };
        cand[0].bar3 = cand[0].bar1 + size;
        const UInt64 endProbe = size - (64ULL << 20);
        UInt64 hOnes = 14695981039346656037ULL;
        for (unsigned i = 0; i < 4096; ++i) { hOnes ^= 0xff; hOnes *= 1099511628211ULL; }
        const UInt64 h0 = hashDevPage(old1, 0);
        const UInt64 h3 = hashDevPage(old3, 0);   // BAR3 may legitimately read all-ones pre-GSP
        stage = 7;
        if (h0 == ~0ULL || !tom2) goto out;
        setProperty("NVGspControl-rebar-hash-old0", h0, 64);
        setProperty("NVGspControl-rebar-hash-old3", h3, 64);
        // guard on before touching anything
        nvramSetSync("nvgsp-rebar-boot", "1");
        // kernel console off across the move (it writes through the old BAR1)
        IOPlatformExpert *pl = getPlatform();
        PE_Video con;
        bzero(&con, sizeof(con));
        if (pl) pl->getConsoleInfo(&con);
        const UInt64 conPhys = con.v_baseAddr & ~static_cast<UInt64>(3);
        const bool conInBar1 = pl && conPhys >= oldBar1 && conPhys < oldBar1 + old1->getLength();
        if (conInBar1) { pl->setConsoleInfo(nullptr, kPEDisableScreen); markBoot("con-off"); }
        const UInt32 newCtrl = (rebar_.ctrl & ~0x3f00U) | (n << 8);
        IODeviceMemory *m1 = nullptr;
        UInt64 base1 = 0, base3 = 0;
        UInt64 got[6] = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL};
        stage = 8;
        for (int k = 0; k < 2 && !m1; ++k) {
            const UInt64 b1 = cand[k].bar1, b3 = cand[k].bar3;
            // never below DRAM top, never overlapping BAR3, BAR3 32 MiB aligned
            if (b1 < tom2 || b3 < tom2 || (b3 < b1 + size && b3 + bar3Bytes > b1) || (b3 & (bar3Bytes - 1)))
                continue;
            const UInt64 wb = b3 < b1 ? b3 : b1;
            const UInt64 we = (b3 + bar3Bytes > b1 + size ? b3 + bar3Bytes : b1 + size) - 1;
            rebarProgram(bridge, newCtrl, b1, b3, wb, we);
            IODeviceMemory *t1 = IODeviceMemory::withRange(b1, size);
            IODeviceMemory *t3 = IODeviceMemory::withRange(b3, bar3Bytes);
            if (t1 && t3) {
                got[3 * k] = hashDevPage(t1, 0);
                got[3 * k + 1] = hashDevPage(t1, endProbe);
                got[3 * k + 2] = hashDevPage(t3, 0);
            }
            OSSafeReleaseNULL(t3);
            const bool ok = t1 && got[3 * k] == h0 &&
                            got[3 * k + 1] != hOnes && got[3 * k + 1] != ~0ULL &&
                            got[3 * k + 2] == h3;
            if (ok) { m1 = t1; base1 = b1; base3 = b3; setProperty("NVGspControl-rebar-candidate", k, 32); }
            else OSSafeReleaseNULL(t1);
        }
        setProperty("NVGspControl-rebar-hash-bar1", got, sizeof(got));
        if (!m1) {
            rebarProgram(bridge, rebar_.ctrl, oldBar1, oldBar3, oldWinBase, oldWinEnd);
            if (conInBar1) pl->setConsoleInfo(nullptr, kPEEnableScreen);
            nvramSetSync("nvgsp-rebar-boot", nullptr);
            stage = 9;
            goto out;
        }
        setProperty("NVGspControl-rebar-target", base1, 64);
        // IOPCIFamily restores its saved config on power transitions: save ours.
        // 0.161.0: saveDeviceState() is IOPCIFamily's On -> Doze step (config
        // space goes to the shadow), so pair it with restoreDeviceState() to
        // come back On. Left in Doze, Tahoe's IOPCIDevice::getResources() keeps
        // saying "nub VGA is not powered" and the Aux KC NVDisplay never probes.
        pci_->saveDeviceState();
        bridge->saveDeviceState();
        bridge->restoreDeviceState();
        pci_->restoreDeviceState();
        if (conInBar1) {
            con.v_baseAddr = (base1 + (conPhys - oldBar1)) | (con.v_baseAddr & 3);
            pl->setConsoleInfo(&con, kPEBaseAddressChange);
            pl->setConsoleInfo(nullptr, kPEEnableScreen);
            markBoot("con-on");
            setProperty("NVGspControl-rebar-console", con.v_baseAddr, 64);
        }
        bar1Mem_ = m1;
        bar3Mem_ = IODeviceMemory::withRange(base3, bar3Bytes);
        rebar_.active = true;
        rebar_.ctrl = newCtrl;
        rebar_.sizeLog2Mb = n;
        rebar_.bar1Base = base1;
        rebar_.bar1Bytes = size;
        rebar_.bar3Base = base3;
        rebar_.bar3Bytes = bar3Bytes;
        stage = 10;
    }
out:
    setProperty("NVGspControl-rebar-stage", stage, 32);
    IODeviceMemory *b1 = bar1Dev();
    setProperty("NVGspControl-bar1-phys", b1 ? b1->getPhysicalAddress() : 0, 64);
    setProperty("NVGspControl-bar1-bytes", b1 ? b1->getLength() : 0, 64);
    IODeviceMemory *boot1 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
    setProperty("NVGspControl-bar1-boot-phys", boot1 ? boot1->getPhysicalAddress() : 0, 64);
    setProperty("NVGspControl-bar1-final", true);   // NVDisplay waits for this
    IOLog("NVGspControl: ReBAR stage %u, BAR1 0x%llx %llu MiB\n", stage,
          b1 ? static_cast<unsigned long long>(b1->getPhysicalAddress()) : 0ULL,
          b1 ? static_cast<unsigned long long>(b1->getLength() >> 20) : 0ULL);
    return rebar_.active;
}

bool NVGspControl::start(IOService *provider) {
    if (!super::start(provider)) return false;
    markBoot("start");
    pci_ = OSDynamicCast(IOPCIDevice, provider);
    if (!pci_ || pci_->configRead16(kIOPCIConfigVendorID) != 0x10de ||
        pci_->configRead16(kIOPCIConfigDeviceID) != 0x2704)
        return false;
    rebarOkCall_ = thread_call_allocate(&NVGspControl::rebarOkCallout, this);   // 0.131.0
    rebarResize();   // 0.130.0: before anything maps BAR1
    // 0.100.5: GSP command-queue doorbell.
    doorbellMap_ = pci_->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (doorbellMap_ && doorbellMap_->getLength() > 0x110c04)
        init_.setDoorbell(&NVGspControl::gspDoorbell, this);
    if (doorbellMap_ && doorbellMap_->getLength() >= 0x01000000) {   // 0.117.0
        gBar0Pci = pci_;
        gBar0Map = doorbellMap_;
    }
    // 0.100.3: GPU identity for System Information / About This Mac (the
    // framebuffer alone reports 33 MB, the scan-out surface): model string and
    // total VRAM from the usable-FB register (MiB).
    {
        static const char kModel[] = "NVIDIA GeForce RTX 4080";
        OSData *model = OSData::withBytes(kModel, sizeof(kModel));
        if (model) { pci_->setProperty("model", model); model->release(); }
        UInt32 fbMb = 0;
        IOMemoryMap *m = sharedBar0Map(pci_);
        if (m && m->getLength() > kUsableFbSizeMb + 4) {
            Bar0Io io{m};
            io.read(static_cast<uint32_t>(kUsableFbSizeMb), &fbMb);
        }
        if (m) m->release();
        if (fbMb && fbMb < 0x100000) {
            pci_->setProperty("VRAM,totalMB", fbMb, 32);
            const uint64_t bytes = static_cast<uint64_t>(fbMb) << 20;
            OSData *total = OSData::withBytes(&bytes, sizeof(bytes));
            if (total) { pci_->setProperty("VRAM,totalsize", total); total->release(); }
        }
    }
    UInt32 draw = 0;
    drawTest_ = PE_parse_boot_argn("nvgspdraw", &draw, sizeof(draw)) &&
        draw != 0;
    setProperty("NVGspControl-draw-test", drawTest_);
    lock_ = IOLockAlloc();
    if (!lock_) return false;
    bar1Lock_ = IOLockAlloc();
    {
        UInt32 off = 0;
        if (PE_parse_boot_argn("nvgsp-nobar1ring", &off, sizeof(off)) && off) bar1Direct_ = false;
        setProperty("NVGspControl-bar1-ring", bar1Direct_);
    }
    setProperty("NVGspControl-ready", true);
    // 0.96.0: S3 sleep/wake interest (quiesce + daemon-driven resume).
    sleepWakeNotifier_ = registerSleepWakeInterest(&NVGspControl::sleepWakeHandler, this);
    setProperty("NVGspControl-sleepwake-interest", sleepWakeNotifier_ != nullptr);
    // 0.99.0: slow-path self-drive callout.
    resumeCall_ = thread_call_allocate(&NVGspControl::resumeDriveCallout, this);
    // 0.115.0: RC -> automatic GPU reset (NVRAM nvgsp-autoreset=1).
    autoResetCall_ = thread_call_allocate(&NVGspControl::autoResetCallout, this);
    boostCall_ = thread_call_allocate(&NVGspControl::boostCallout, this);    // 0.135.0
    // 0.114.0: user-client ABI level for NVK (nvkmd_macos): 114 = arena
    // binds at 64 KiB granularity, VRAM suballocation safe.
    setProperty("NVGspControl-abi", kUserAbi, 32);
    setProperty("NVGspControl-exec-segment-flags", nvgsp::kExecSupportedFlags, 32);
    registerService();
    IOLog("NVGspControl: userspace firmware staging service ready\n");
    return true;
}

void NVGspControl::stop(IOService *provider) {
    if (sleepWakeNotifier_) sleepWakeNotifier_->remove();
    sleepWakeNotifier_ = nullptr;
    if (boostCall_) {
        thread_call_cancel(boostCall_);
        thread_call_free(boostCall_);
        boostCall_ = nullptr;
    }
    if (autoResetCall_) {
        thread_call_cancel(autoResetCall_);
        thread_call_free(autoResetCall_);
        autoResetCall_ = nullptr;
    }
    if (resumeCall_) {
        resumeAbort_ = true;
        thread_call_cancel(resumeCall_);
        for (UInt32 ms = 0; resumeActive_ && ms < 10000; ++ms) IOSleep(1);
        thread_call_free(resumeCall_);
        resumeCall_ = nullptr;
    }
    if (queueSnap_) {
        IOFree(queueSnap_, nvgsp::kGspSharedBytes);
        queueSnap_ = nullptr;
    }
    if (dispSave_) {
        IOFree(dispSave_, 0x10000);
        dispSave_ = nullptr;
    }
    init_.setDoorbell(nullptr, nullptr);
    if (gBar0Map == doorbellMap_) {   // 0.117.0: no new users of the cache
        gBar0Map = nullptr;
        gBar0Pci = nullptr;
    }
    OSSafeReleaseNULL(doorbellMap_);
    if (headCache_) {
        IOFree(headCache_, 1024);
        headCache_ = nullptr;
        headCacheValid_ = false;
    }
    if (intrArmed_) disarmInterrupts(nullptr);
    if (wl_ && irq_) wl_->removeEventSource(irq_);
    OSSafeReleaseNULL(irq_);
    OSSafeReleaseNULL(wl_);
    OSSafeReleaseNULL(intrBar0_);
    staged_ = executed_ = false;
    statusSequence_ = 0;
    initDone_ = false;
    initResult_ = initPrivateResult_ = ~0U;
    postInitPhase_ = 0;
    internalClient_ = internalDevice_ = internalSubdevice_ = 0;
    virtualOffset_ = localMemoryOffset_ = pte4KAddress_ = originalPte_ = 0;
    gpfifoBackingOffset_ = userdBackingOffset_ = 0;
    instanceBackingOffset_ = methodBackingOffset_ = 0;
    gpfifoBackingSize_ = userdBackingSize_ = 0;
    instanceBackingSize_ = methodBackingSize_ = 0;
    methodBufferBytes_ = 0;
    errBackingOffset_ = errBackingSize_ = 0;
    channelGpFifoVa_ = 0;
    channelCid_ = 0;
    kickToken_ = kickPolls_ = 0;
    kickTimeOk_ = kickRung_ = false;
    pbRung_ = false;
    pbPolls_ = 0;
    ctxBackingOffset_ = 0;
    ctxPtesInstalled_ = 0;
    pd0Address_ = 0;
    ctxHugeInstalled_ = 0;
    gr3dOk_ = false;
    lastGrOwner_ = nullptr;                  // 0.145.0
    rcEvents_ = mmuFaultEvents_ = otherEvents_ = 0;
    bar1Finished_ = false;
    bar1Live_ = false;                       // 0.123.0
    grPersistent_ = false;
    subPbOff_ = subSeq_ = 0;
    scratchOffset_ = 0;
    scratchHuge_ = 0;
    pdbAddress_ = 0;
    vramVaTables_ = 0;
    arenaDropAllLocked(false);               // 0.111.0: stop frees the contexts
    scratchTry_ = 0;
    scratchRetry_ = false;
    ceChunk_ = 0;
    ceMapped_ = ceStarted_ = cePersistent_ = false;
    ceToken_ = cePbOff_ = ceSeq_ = 0;
    ceOutstanding_ = 0;                      // 0.112.0
    ceUserdOffset_ = ceInstOffset_ = ceMthdOffset_ = 0;
    videoDropLocked();                       // 0.116.0
    clientChannelsDropLocked();              // 0.178.0
    freeSavedVramLocked();                   // 0.127.0
    thawLocked();
    wndOwnsScreen_ = false;
    fbHugeInstalled_ = 0;
    grRung_ = false;
    grPolls_ = 0;
    channelBackingComplete_ = channelAllocOk_ = false;
    gpfifoPteInstalled_ = gpfifoPteRestored_ = false;
    dmaMapCompleted_ = false;
    bar2MapUnsupported_ = false;
    praminPteReadOk_ = false;
    hostPteMapped_ = hostPteValidated_ = hostPteRestored_ = false;
    pteMapInvalidateOk_ = pteRestoreInvalidateOk_ = false;
    gopSurfaceHashPre_ = 0;
    gopSurfaceHashPreOk_ = false;
    for (unsigned i = 0; i < 8; ++i) gopSurfaceBytesPre_[i] = 0;
    gopSurfaceHashMid_ = 0;
    gopSurfaceHashMidOk_ = false;
    bisectPoll_ = 0;
    init_.reset();
    booter_.reset();
    gsp_.reset();
    pci_ = nullptr;
    for (UInt32 i = 0; i < bar1Wins_; ++i)
        if (bar1Win_[i].map) bar1Win_[i].map->release();
    bar1Wins_ = 0;
    if (bar1Lock_) { IOLockFree(bar1Lock_); bar1Lock_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    super::stop(provider);
}

IOReturn NVGspControl::executeBoot() {
    if (!pci_ || !staged_ || executed_) return kIOReturnNotReady;
    markBoot("gsp-boot");
    IODeviceMemory *bar0 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IOMemoryMap *map = bar0 ? bar0->map() : nullptr;
    if (!map || map->getLength() < kPromBase + kPromBytes) {
        if (map) map->release();
        return kIOReturnNoResources;
    }
    Bar0Io io{map};
    UInt8 *rom = static_cast<UInt8 *>(IOMalloc(kPromBytes));
    if (!rom) { map->release(); return kIOReturnNoMemory; }
    bool romRead = true;
    for (size_t i = 0; i < kPromBytes; i += 4) {
        UInt32 word = 0;
        if (!io.read(kPromBase + static_cast<uint32_t>(i), &word)) { romRead = false; break; }
        __builtin_memcpy(rom + i, &word, 4);
    }
    nvgsp::VbiosFwsecView vbios{};
    UInt32 gspFuse = 0, boot0 = 0;
    const bool vbiosOk = romRead && nvgsp::parseVbiosFwsec(rom, kPromBytes, &vbios);
    const bool identityOk = io.read(kBoot0, &boot0) &&
        io.read(kGspUcode9Fuse, &gspFuse);
    const nvgsp::WprMeta *meta = gsp_.metadata();
    // 0.99.1: fresh WPR metadata for every boot (S3 re-boot saw the first
    // boot's verified/bootCount and the GSP RISC-V halted, mbox0 0x80000000).
    setProperty("NVGspControl-exec-meta-rewritten", gsp_.rewriteMetadata());
    setProperty("NVGspControl-exec-vbios-ok", vbiosOk);
    if (!vbiosOk || !identityOk || !meta) {
        IOFree(rom, kPromBytes); map->release(); return kIOReturnBadMedia;
    }

    const UInt16 commandBefore = pci_->configRead16(kIOPCIConfigCommand);
    const bool busMasterBefore = (commandBefore & 4) != 0;
    if (!busMasterBefore) pci_->setBusMasterEnable(true);
    const bool busMaster = (pci_->configRead16(kIOPCIConfigCommand) & 4) != 0;

    // Stage the final prerequisite before the first register write. If this
    // allocation fails the running GOP state remains untouched.
    nvgsp::FwsecStaging fwsec;
    const bool fwsecStage = busMaster &&
        fwsec.stage(vbios.fwsec, gspFuse & 0xff, meta->frtsOffset,
                    nullptr, vbios.dmaImageSize);

    // NVIDIA normal boot: reset GSP into Falcon, execute board FWSEC/FRTS,
    // reset GSP into RISC-V, program LibOS args, then run SEC2 Booter Load.
    const bool gspFalconReset = fwsecStage &&
        resetPulse(io, 0x001103c0, 0x001100f4) &&
        io.write(0x00111668, 0) &&
        nvgsp::falconPoll(io, 0x00111668, 1, 1, 200000, 10) &&
        io.write(0x00110084, boot0);
    nvgsp::FwsecExecutionResult fwsecResult{};
    const bool fwsecOk = gspFalconReset && nvgsp::executeFwsecFrts(
        io, vbios.fwsec, fwsec.busAddress(), fwsec.imageSize(),
        meta->frtsOffset, &fwsecResult);

    const bool gspRiscvReset = fwsecOk &&
        resetPulse(io, 0x001103c0, 0x001100f4) &&
        io.write(0x00111668, 0x111) &&
        io.write(nvgsp::falcon::kMailbox0,
                 static_cast<uint32_t>(init_.libosArgsBus())) &&
        io.write(nvgsp::falcon::kMailbox1,
                 static_cast<uint32_t>(init_.libosArgsBus() >> 32));
    uint32_t sec2Bcr = 0;
    const bool sec2Enable = gspRiscvReset &&
        resetPulse(io, 0x008403c0, 0x008400f4) &&
        io.write(0x00841668, 0) &&
        nvgsp::sec2Poll(io, 0x00841668, 1, 1, 200000, 10) &&
        io.read(0x00841668, &sec2Bcr);
    const bool sec2Reset = sec2Enable &&
        io.write(0x00840084, boot0);
    uint32_t sec2DmaCtl = 0, sec2FbifCtl = 0, sec2Transcfg = 0,
             sec2CpuCtl = 0, sec2DmaCommand = 0, sec2Rm = 0;
    io.read(nvgsp::sec2::kDmaCtl, &sec2DmaCtl);
    io.read(nvgsp::sec2::kFbifCtl, &sec2FbifCtl);
    io.read(nvgsp::sec2::kFbifTranscfg0, &sec2Transcfg);
    io.read(nvgsp::sec2::kCpuCtl, &sec2CpuCtl);
    io.read(nvgsp::sec2::kDmaCommand, &sec2DmaCommand);
    io.read(0x00840084, &sec2Rm);
    nvgsp::BooterView booterView{
        nullptr, booter_.imageSize(), nullptr, 0, 0,
        booter_.patchLocation(), 0, booter_.engineId(), booter_.ucodeId(),
        booter_.layout()
    };
    nvgsp::BooterExecutionResult booterResult{};
    const bool booterOk = sec2Reset && nvgsp::executeBooterLoad(
        io, booterView, booter_.busAddress(), booter_.imageSize(),
        bootMailboxOverride_ ? bootMailboxOverride_ : gsp_.metadataBusAddress(),
        &booterResult);
    const bool osVersionOk = booterOk &&
        io.write(nvgsp::falcon::kBase + 0x80, gsp_.appVersion());
    uint32_t riscvCpuCtl = 0;
    bool gspActive = false;
    if (osVersionOk) {
        for (unsigned i = 0; i < 200000; ++i) {
            if (io.read(0x00111388, &riscvCpuCtl) && (riscvCpuCtl & (1U << 7))) {
                gspActive = true; break;
            }
            IODelay(10);
        }
    }
    executed_ = gspActive;
    if (!executed_ && !busMasterBefore) pci_->setBusMasterEnable(false);
    setProperty("NVGspControl-exec-gsp-falcon-reset", gspFalconReset);
    setProperty("NVGspControl-exec-fwsec-stage", fwsecStage);
    setProperty("NVGspControl-exec-fwsec-ok", fwsecOk);
    setProperty("NVGspControl-exec-gsp-riscv-reset", gspRiscvReset);
    setProperty("NVGspControl-exec-sec2-reset", sec2Reset);
    setProperty("NVGspControl-exec-sec2-enable", sec2Enable);
    setProperty("NVGspControl-exec-sec2-bcr", sec2Bcr, 32);
    setProperty("NVGspControl-exec-sec2-dmactl", sec2DmaCtl, 32);
    setProperty("NVGspControl-exec-sec2-fbifctl", sec2FbifCtl, 32);
    setProperty("NVGspControl-exec-sec2-transcfg0", sec2Transcfg, 32);
    setProperty("NVGspControl-exec-sec2-cpuctl", sec2CpuCtl, 32);
    setProperty("NVGspControl-exec-sec2-dmacmd", sec2DmaCommand, 32);
    setProperty("NVGspControl-exec-sec2-rm", sec2Rm, 32);
    setProperty("NVGspControl-exec-booter-ok", booterOk);
    setProperty("NVGspControl-exec-gsp-active", gspActive);
    setProperty("NVGspControl-exec-fwsec-dma", fwsecResult.dmaTransfers, 32);
    setProperty("NVGspControl-exec-booter-dma", booterResult.dmaTransfers, 32);
    setProperty("NVGspControl-exec-sec2-mailbox0", booterResult.mailbox0, 32);
    setProperty("NVGspControl-exec-riscv-cpuctl", riscvCpuCtl, 32);
    setProperty("NVGspControl-executed", executed_);
    // 0.9.4: mid-point GOP hash (end of executeBoot, before any --status
    // poll). Isolates FWSEC/SEC2/GSP boot vs the phase machine.
    gopSurfaceHashMid_ = 0;
    gopSurfaceHashMidOk_ = false;
    IODeviceMemory *bar1Mid =
        bar1Dev();
    if (bar1Mid && bar1Mid->getLength() >= 4096) {
        IOMemoryMap *bar1Map = mapBar1Head(4096);
        if (bar1Map && bar1Map->getLength() >= 4096) {
            const volatile UInt8 *bytes =
                reinterpret_cast<const volatile UInt8 *>(
                    static_cast<uintptr_t>(bar1Map->getVirtualAddress()));
            UInt64 hash = 14695981039346656037ULL;
            for (unsigned i = 0; i < 4096; ++i) {
                hash ^= bytes[i];
                hash *= 1099511628211ULL;
            }
            gopSurfaceHashMid_ = hash;
            gopSurfaceHashMidOk_ = true;
        }
        if (bar1Map) bar1Map->release();
    }
    // 0.46.0: identity proof while BAR1 is still physical: VRAM[0,4K)
    // via PRAMIN must hash equal to BAR1+0 read at the same moment.
    // Content-independent (0.45.0's content gate failed once a logged-in
    // WindowServer had redrawn the surface).
    UInt64 vramMid = 0;
    gopAtVram0_ = gopSurfaceHashMidOk_ &&
        praminHashRange(pci_, 0, 4096, &vramMid) &&
        vramMid == gopSurfaceHashMid_;
    setProperty("NVGspControl-gop-vram-hash-mid", vramMid, 64);
    setProperty("NVGspControl-gop-at-vram0", gopAtVram0_);
    setProperty("NVGspControl-gop-surface-hash-mid",
                gopSurfaceHashMid_, 64);
    setProperty("NVGspControl-gop-surface-hash-mid-ok",
                gopSurfaceHashMidOk_);
    IOFree(rom, kPromBytes);
    map->release();
    return executed_ ? kIOReturnSuccess : kIOReturnIOError;
}

bool NVGspControl::hashBar1Surface(UInt64 *hash) {
    if (!pci_ || !hash) return false;
    IODeviceMemory *bar1 =
        bar1Dev();
    if (!bar1 || bar1->getLength() < 4096) return false;
    IOMemoryMap *map = mapBar1Head(4096);
    if (!map || map->getLength() < 4096) {
        if (map) map->release();
        return false;
    }
    const volatile UInt8 *bytes =
        reinterpret_cast<const volatile UInt8 *>(
            static_cast<uintptr_t>(map->getVirtualAddress()));
    UInt64 h = 14695981039346656037ULL;
    for (unsigned i = 0; i < 4096; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    map->release();
    *hash = h;
    return true;
}

bool NVGspControl::hashBar1Range(UInt64 offset, unsigned len, UInt64 *hash) {
    if (!pci_ || !hash || len == 0 || len > 4096) return false;
    IODeviceMemory *bar1 =
        bar1Dev();
    if (!bar1 || bar1->getLength() < offset + len) return false;
    IOMemoryMap *map = mapBar1Head(offset + len);
    if (!map || map->getLength() < offset + len) {
        if (map) map->release();
        return false;
    }
    const volatile UInt8 *bytes =
        reinterpret_cast<const volatile UInt8 *>(
            static_cast<uintptr_t>(map->getVirtualAddress())) +
        offset;
    UInt64 h = 14695981039346656037ULL;
    for (unsigned i = 0; i < len; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    map->release();
    *hash = h;
    return true;
}

namespace {
// 0.9.5: decimal append without libc (kext-safe).
void appendDec(char *dst, size_t size, size_t *pos, UInt32 v) {
    char tmp[10];
    unsigned n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v && n < sizeof(tmp));
    while (n && *pos + 1 < size) dst[(*pos)++] = tmp[--n];
}
void appendStr(char *dst, size_t size, size_t *pos, const char *s) {
    while (*s && *pos + 1 < size) dst[(*pos)++] = *s++;
}
} // namespace

IOReturn NVGspControl::setExperimentFlags(UInt32 flags) {
    // Only before --boot: the chain reads the flags while it runs.
    if (executed_) return kIOReturnNotPermitted;
    experimentFlags_ = flags;
    if (flags & 1) drawTest_ = true;
    setProperty("NVGspControl-experiment-flags", flags, 32);
    setProperty("NVGspControl-draw-test", drawTest_);
    return kIOReturnSuccess;
}

void NVGspControl::finishBar1() {
    if (bar1Finished_ || !pci_) return;
    bar1Finished_ = true;
    markBoot("bar1");
    // 0.45.0: BAR1 remap (persists past teardown on purpose),
    // then a CPU-side BAR1+0 hash vs the pre-GSP GOP hash.
    Bar1Remap b1{};
    const bool walked = bar1RemapGop(pci_, &b1);
    setProperty("NVGspControl-bar1-walk-ok", walked);
    setProperty("NVGspControl-bar1-stage", b1.stage, 32);
    setProperty("NVGspControl-bar1-block", b1.block, 32);
    setProperty("NVGspControl-bar1-pdb", b1.pdb, 64);
    setProperty("NVGspControl-bar1-pdb-lo", b1.pdbLo, 32);
    setProperty("NVGspControl-bar1-pd3e", b1.pd3e, 64);
    setProperty("NVGspControl-bar1-pd2e", b1.pd2e, 64);
    setProperty("NVGspControl-bar1-pd1e", b1.pd1e, 64);
    setProperty("NVGspControl-bar1-pd0-table", b1.pd0Table, 64);
    setProperty("NVGspControl-bar1-pd0-first", b1.pd0First, 64);
    setProperty("NVGspControl-bar1-pd0-used", b1.usedSlots, 32);
    setProperty("NVGspControl-bar1-installed", b1.installed);
    setProperty("NVGspControl-bar1-vf-block-before",
                b1.vfBlockBefore, 32);
    setProperty("NVGspControl-bar1-vf-block-after",
                b1.vfBlockAfter, 32);
    setProperty("NVGspControl-bar1-bind-status", b1.bindStatus,
                32);
    setProperty("NVGspControl-bar1-physical-bound",
                b1.physicalBound);
    setProperty("NVGspControl-bar1-big-pt", b1.bigPt, 64);
    setProperty("NVGspControl-bar1-big-valid-mask",
                b1.bigValidMask, 64);
    setProperty("NVGspControl-bar1-big-first-valid",
                b1.bigFirstValid, 64);
    setProperty("NVGspControl-bar1-big-filled", b1.bigFilled, 32);
    setProperty("NVGspControl-bar1-huge-filled", b1.hugeFilled,
                32);
    setProperty("NVGspControl-bar1-invalidated", b1.invalidated);
    setProperty("NVGspControl-bar1-invalidate-final",
                b1.invalidateFinal, 32);
    UInt64 bar1Hash = 0;
    bool bar1HashOk = false;
    IODeviceMemory *bar1 = bar1Dev();
    IOMemoryMap *bar1Map = bar1 ? mapBar1Head(0x201000) : nullptr;
    // 0.47.0: identity check at BAR1 +2 MiB (our huge PTE
    // range) against PRAMIN VRAM @2 MiB.
    if (bar1Map && bar1Map->getLength() >= 0x201000) {
        const volatile UInt8 *bytes =
            reinterpret_cast<const volatile UInt8 *>(
                static_cast<uintptr_t>(
                    bar1Map->getVirtualAddress() + 0x200000));
        UInt64 hash = 14695981039346656037ULL;
        for (unsigned i = 0; i < 4096; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
        bar1Hash = hash;
        bar1HashOk = true;
    }
    if (bar1Map) bar1Map->release();
    UInt64 vram2m = 0;
    const bool vram2mOk = praminHashRange(pci_, 0x200000, 4096,
                                          &vram2m);
    // 0.48.0: raw evidence — RAMIN 0x200..0x20f (PDB,
    // ADR_LIMIT), BAR1 and PRAMIN words at 0 and 2 MiB, PD0[1]
    // readback.
    {
        UInt64 raw[10]{};
        PraminPteResult q{};
        praminPteAccess(pci_, b1.inst + 0x200, &raw[0], false,
                        false, &q);
        praminPteAccess(pci_, b1.inst + 0x208, &raw[1], false,
                        false, &q);
        praminPteAccess(pci_, 0, &raw[2], false, false, &q);
        praminPteAccess(pci_, 0x200000, &raw[3], false, false, &q);
        praminPteAccess(pci_, b1.pd0Table + 16, &raw[4], false,
                        false, &q);
        praminPteAccess(pci_, b1.bigPt, &raw[5], false, false, &q);
        IOMemoryMap *m = bar1 ? mapBar1Head(0x200010) : nullptr;
        if (m && m->getLength() >= 0x200010) {
            const volatile UInt64 *w =
                reinterpret_cast<const volatile UInt64 *>(
                    static_cast<uintptr_t>(m->getVirtualAddress()));
            raw[6] = w[0];
            raw[7] = w[1];
            raw[8] = w[0x200000 / 8];
            raw[9] = w[0x200000 / 8 + 1];
        }
        if (m) m->release();
        setProperty("NVGspControl-bar1-raw", raw, sizeof(raw));
    }
    setProperty("NVGspControl-bar1-hash-2m", bar1Hash, 64);
    setProperty("NVGspControl-bar1-vram-hash-2m", vram2m, 64);
    // 0.131.0: resized BAR1 proven after GSP boot -> clear the boot guard
    // 90 s later (a crash before that skips the resize on the next boot)
    if (rebar_.active && !rebarOkArmed_ && rebarOkCall_ && bar1HashOk && vram2mOk &&
        b1.installed && bar1Hash == vram2m) {
        UInt64 deadline = 0;
        clock_interval_to_deadline(90, kSecondScale, &deadline);
        thread_call_enter_delayed(rebarOkCall_, deadline);
        rebarOkArmed_ = true;
    }
    setProperty("NVGspControl-bar1-live",
                bar1HashOk && vram2mOk && b1.installed &&
                bar1Hash == vram2m);
    // 0.123.0: remember the identity-mapped BAR1 for CPU-visible VRAM.
    // (Runs inside pollStatusLocked: lock_ is already held.)
    {
        IODeviceMemory *b = bar1Dev();
        bar1Live_ = bar1HashOk && vram2mOk && b1.installed && bar1Hash == vram2m;
        bar1Bytes_ = b ? b->getLength() : 0;
        const UInt64 end = vramCpuWindowEnd();
        setProperty("NVGspControl-vram-cpu-bytes", end > 0x40000000ULL ? end - 0x40000000ULL : 0, 64);
    }
}

// 0.123.0: end of the CPU-visible part of the VRAM heap ([1 GiB, end)): the
// heap end capped by the BAR1 size (BAR1 physical mode: offset = VRAM phys).
// 0 when BAR1 is not live or does not reach past 1 GiB (256 MiB BAR1).
UInt64 NVGspControl::vramCpuWindowEnd() const {
    if (!bar1Live_ || !fbFreeLimit_) return 0;
    const UInt64 heapEnd = ((fbFreeLimit_ + 1) & ~0x1fffffULL) - (64ULL << 20);
    const UInt64 end = (bar1Bytes_ < heapEnd ? bar1Bytes_ : heapEnd) & ~0x1fffffULL;
    return end > 0x40000000ULL ? end : 0;
}

// 0.70.0: MSI interrupt path. Vector v lives in LEAF[v/32] bit v%32 of the
// VF CPU interrupt tree (BAR0 0xB80000 + 0x1000); its subtree is leaf/2 and
// is enabled in TOP_EN_SET[0]. 0.74.0: several vectors (kVecGsp/kVecDisp/
// kVecGrNs) from INTR_GET_KERNEL_TABLE; MSI EOI via XVE_CYA_2 (AD103).
bool NVGspControl::armInterrupts(const UInt32 *vectors) {
    if (intrArmed_ || !pci_) return false;
    int msiIndex = -1;
    for (int i = 0; i < 8; ++i) {
        int type = 0;
        if (pci_->getInterruptType(i, &type) != kIOReturnSuccess) break;
        if (type & kIOInterruptTypePCIMessaged) { msiIndex = i; break; }
    }
    setProperty("NVGspControl-intr-msi-index", static_cast<UInt32>(msiIndex), 32);
    if (msiIndex < 0) return false;
    intrBar0_ = sharedBar0Map(pci_);
    if (!intrBar0_ || intrBar0_->getLength() < 0xB82000) {
        OSSafeReleaseNULL(intrBar0_);
        return false;
    }
    wl_ = IOWorkLoop::workLoop();
    irq_ = IOInterruptEventSource::interruptEventSource(
        this, &NVGspControl::onInterrupt, pci_, msiIndex);
    if (!wl_ || !irq_ || wl_->addEventSource(irq_) != kIOReturnSuccess) {
        OSSafeReleaseNULL(irq_);
        OSSafeReleaseNULL(wl_);
        OSSafeReleaseNULL(intrBar0_);
        return false;
    }
    Bar0Io bar0{intrBar0_};
    for (UInt32 k = 0; k < kVecCount; ++k) {
        vec_[k] = vectors[k] < 256 ? vectors[k] : ~0U;
        vecCount_[k] = 0;
    }
    setProperty("NVGspControl-intr-vectors", vec_, sizeof(vec_));
    intrArmed_ = true;
    irq_->enable();
    // Display: enable head-0 LAST_DATA (vblank) at the FE (kheadReadPending
    // Vblank_v03_00 tests FE_RM_INTR_STAT_HEAD_TIMING LAST_DATA).
    if (vec_[kVecDisp] != ~0U) {
        UInt32 en = 0;
        bar0.read(0x611D80, &en);
        setProperty("NVGspControl-intr-disp-en-before", en, 32);
        bar0.write(0x611800, 2);                         // clear stale LAST_DATA
        bar0.write(0x611D80, en | 2);                    // EN_HEAD_TIMING(0).LAST_DATA
    }
    UInt32 topMask = 0;
    for (UInt32 k = 0; k < kVecCount; ++k) {
        if (vec_[k] == ~0U) continue;
        const UInt32 leaf = vec_[k] / 32;
        bar0.write(0xB81200 + leaf * 4, 1U << (vec_[k] % 32));  // LEAF_EN_SET
        topMask |= 1U << (leaf / 2);
    }
    bar0.write(0xB81608, topMask);                       // TOP_EN_SET(0)
    bar0.write(0x00088704, 0);                           // MSI rearm
    UInt8 msiCap = 0;
    pci_->findPCICapability(0x05, &msiCap);
    if (msiCap)
        setProperty("NVGspControl-intr-msi-ctrl-armed",
                    pci_->configRead16(msiCap + 2), 16);
    return true;
}

void NVGspControl::disarmVector(UInt32 k) {
    if (!intrBar0_ || k >= kVecCount || vec_[k] == ~0U) return;
    Bar0Io bar0{intrBar0_};
    bar0.write(0xB81400 + (vec_[k] / 32) * 4, 1U << (vec_[k] % 32));  // LEAF_EN_CLEAR
    if (k == kVecDisp) {
        UInt32 en = 0;
        bar0.read(0x611D80, &en);
        bar0.write(0x611D80, en & ~2U);
    }
    vec_[k] = ~0U;
}

void NVGspControl::disarmInterrupts(const char *why) {
    for (UInt32 k = 0; k < kVecCount; ++k) disarmVector(k);
    if (irq_) irq_->disable();
    intrArmed_ = false;
    if (why) setProperty("NVGspControl-intr-disarmed", why);
}

void NVGspControl::onInterrupt(OSObject *owner, IOInterruptEventSource *, int) {
    NVGspControl *self = OSDynamicCast(NVGspControl, owner);
    if (self) self->serviceInterrupt();
}

void NVGspControl::serviceInterrupt() {
    if (!lock_) return;
    UInt64 tEnter = 0;
    clock_get_uptime(&tEnter);
    lk(__LINE__);
    {
        UInt64 tLocked = 0, ns = 0;
        clock_get_uptime(&tLocked);
        absolutetime_to_nanoseconds(tLocked - tEnter, &ns);
        if (ns > intrLockWaitMaxNs_) intrLockWaitMaxNs_ = ns;
    }
    if (intrArmed_ && intrBar0_) {
        Bar0Io bar0{intrBar0_};
        ++intrCount_;
        bool any = false;
        for (UInt32 k = 0; k < kVecCount; ++k) {
            if (vec_[k] == ~0U) continue;
            const UInt32 leafReg = 0xB81000 + (vec_[k] / 32) * 4;
            const UInt32 bit = 1U << (vec_[k] % 32);
            UInt32 pending = 0;
            bar0.read(leafReg, &pending);
            if (!(pending & bit)) continue;
            any = true;
            ++vecCount_[k];
            if (k == kVecGsp) {
                bar0.write(leafReg, bit);                     // LEAF W1C
                UInt32 irqstat = 0;
                bar0.read(nvgsp::falcon::kBase + 0x008, &irqstat);
                if (irqstat & 0x40)                           // SWGEN0
                    bar0.write(nvgsp::falcon::kBase + 0x004, 0x40);
                inIntr_ = true;
                pollStatusLocked();
                inIntr_ = false;
                bar0.write(nvgsp::falcon::kBase + 0x3e8, 1);  // INTR_RETRIGGER(0)
                setProperty("NVGspControl-intr-last-irqstat", irqstat, 32);
            } else if (k == kVecDisp) {
                // 0.76.0: leaf W1C FIRST, then drain the FE source in a
                // loop. 0.75.0 cleared FE then leaf, so a vblank landing in
                // between was latched in FE but its leaf edge was wiped:
                // vblank stalled until an unrelated MSI (max gap 4.58 s).
                bar0.write(leafReg, bit);
                // 0.78.0: GSP-RM owns EN_HEAD_TIMING too; if our LAST_DATA
                // enable was dropped, count it and put it back.
                {
                    UInt32 en = 0;
                    bar0.read(0x611D80, &en);
                    if (!(en & 2)) {
                        ++vblankEnLost_;
                        bar0.write(0x611D80, en | 2);
                    }
                }
                for (UInt32 pass = 0; pass < 4; ++pass) {
                    UInt32 dispatch = 0, stat = 0;
                    bar0.read(0x611EC0, &dispatch);           // FE_RM_INTR_DISPATCH
                    if (!dispatch) break;
                    if (dispatch & 1) {
                        bar0.read(0x611C00, &stat);           // RM_INTR_STAT_HEAD_TIMING(0)
                        bar0.write(0x611800, stat);           // EVT_STAT W1C
                        if (stat & 2) {
                            UInt64 now = 0;
                            clock_get_uptime(&now);
                            if (lastVblank_) {
                                UInt64 ns = 0;
                                absolutetime_to_nanoseconds(now - lastVblank_, &ns);
                                if (!vblankMinNs_ || ns < vblankMinNs_) vblankMinNs_ = ns;
                                // 0.79.0: interval histogram <12, <24, <40, <80, >=80 ms
                                vblankHist_[ns < 12000000 ? 0 : ns < 24000000 ? 1
                                            : ns < 40000000 ? 2 : ns < 80000000 ? 3 : 4]++;
                                if (ns > vblankMaxNs_) vblankMaxNs_ = ns;
                                vblankSumNs_ += ns;
                            }
                            lastVblank_ = now;
                            ++vblanks_;
                            // 0.80.0: clients are called after lock_ is
                            // dropped (0.79.0: IOFramebuffer's VBL proc can
                            // block for seconds -> service max 8.8 s).
                            vblankDeliver_ = true;
                            vblankDeliverAt_ = now;
                        }
                    }
                    if (dispatch & ~1U) {
                        ++dispOther_;
                        lastDispOther_ = dispatch;
                        break;   // not ours to clear
                    }
                }
                // An unhandled display source must not storm the machine.
                if (dispOther_ > 2000) disarmVector(kVecDisp);
            } else {
                bar0.write(leafReg, bit);                     // GR/CE non-stall: pulse
                // 0.75.0: wake submitRing / fence sleepers (fence landed).
                IOLockWakeup(lock_, &vecCount_[k], false);
                stampDeliverMask_ |= k == kVecCeNs ? 2U : 1U;   // 0.152.0
                if (k != kVecCeNs) stampAdvanceLocked();         // 0.178.12
            }
        }
        if (!any) ++intrSpurious_;
        // Publishing costs an OSNumber per key; vblank runs at 60 Hz.
        if ((intrCount_ & 63) == 1 || !any || intrCount_ < 8) {
            setProperty("NVGspControl-intr-count", intrCount_, 32);
            setProperty("NVGspControl-intr-gsp", vecCount_[kVecGsp], 32);
            setProperty("NVGspControl-intr-disp", vecCount_[kVecDisp], 32);
            setProperty("NVGspControl-intr-gr-nonstall", vecCount_[kVecGrNs], 32);
            setProperty("NVGspControl-intr-ce-nonstall", vecCount_[kVecCeNs], 32);
            setProperty("NVGspControl-vblank-count", vblanks_, 32);
            setProperty("NVGspControl-vblank-en-lost", vblankEnLost_, 32);
            setProperty("NVGspControl-vblank-hist", vblankHist_, sizeof(vblankHist_));
            setProperty("NVGspControl-intr-lockwait-max-ns", intrLockWaitMaxNs_, 64);
            setProperty("NVGspControl-lock-hold-max-ns", lockHoldMaxNs_, 64);
            setProperty("NVGspControl-lock-hold-max-line", lockHoldMaxLine_, 32);
            setProperty("NVGspControl-lock-hold-max-phase", lockHoldMaxPhase_, 32);
            setProperty("NVGspControl-lock-hold-max-uptime-ms", lockHoldMaxUptimeMs_, 64);
            setProperty("NVGspControl-lock-holds-over-10ms", lockHoldsOver10ms_, 32);
            setProperty("NVGspControl-lock-long-holds", longHolds_, sizeof(longHolds_));
            setProperty("NVGspControl-intr-service-max-ns", intrServiceMaxNs_, 64);
            setProperty("NVGspControl-vblank-cb-max-ns", vblankCbMaxNs_, 64);
            if (vblanks_ > 1) {
                setProperty("NVGspControl-vblank-min-ns", vblankMinNs_, 64);
                setProperty("NVGspControl-vblank-max-ns", vblankMaxNs_, 64);
                setProperty("NVGspControl-vblank-avg-ns",
                            vblankSumNs_ / (vblanks_ - 1), 64);
            }
            setProperty("NVGspControl-intr-disp-other", dispOther_, 32);
            setProperty("NVGspControl-intr-disp-other-last", lastDispOther_, 32);
            setProperty("NVGspControl-intr-spurious", intrSpurious_, 32);
            setProperty("NVGspControl-intr-stuck", intrStuck_, 32);
        }
        {
            UInt64 tEnd = 0, ns = 0;
            clock_get_uptime(&tEnd);
            absolutetime_to_nanoseconds(tEnd - tEnter, &ns);
            if (ns > intrServiceMaxNs_) intrServiceMaxNs_ = ns;
        }
        if (intrStuck_ > 5000 || intrSpurious_ > 100000)
            disarmInterrupts(intrStuck_ > 5000 ? "stuck" : "spurious");
        else
            bar0.write(0x00088704, 0);   // MSI EOI (kbifRearmMSI_GM107)
    }
    bool deliver = vblankDeliver_;
    vblankDeliver_ = false;
    const UInt32 count = vblanks_;
    const UInt64 at = vblankDeliverAt_;
    VblankFn fns[kMaxVblankClients];
    void *refs[kMaxVblankClients];
    for (UInt32 c = 0; c < kMaxVblankClients; ++c) {
        fns[c] = vblankFn_[c];
        refs[c] = vblankRef_[c];
    }
    const UInt32 stampMask = stampDeliverMask_;
    stampDeliverMask_ = 0;
    const StampFn sfn = stampFn_;
    void *const sref = stampRef_;
    ulk();
    if (stampMask && sfn) sfn(sref, stampMask);        // 0.152.0
    deliverHotplug();
    if (deliver) {
        UInt64 t0 = 0, t1 = 0, ns = 0;
        clock_get_uptime(&t0);
        for (UInt32 c = 0; c < kMaxVblankClients; ++c)
            if (fns[c]) fns[c](refs[c], count, at);
        clock_get_uptime(&t1);
        absolutetime_to_nanoseconds(t1 - t0, &ns);
        if (ns > vblankCbMaxNs_) vblankCbMaxNs_ = ns;   // published with the counters
    }
}

IOReturn NVGspControl::pingGsp(UInt64 *nsOut, UInt64 *viaIntrOut) {
    if (!lock_) return kIOReturnNotReady;
    lk(__LINE__);
    if (postInitPhase_ != 33 || sleeping_ || pingOutstanding_ || userRpcActive_ || userRpcOutstanding_) {
        ulk();
        return kIOReturnBusy;
    }
    constexpr UInt32 kClientHandle = 0xc0d00001;
    constexpr UInt32 kSubdevice = 0xc0d02080;
    constexpr UInt32 kCmd = 0x20802068, kBytes = 4;  // PERF_GET_CURRENT_PSTATE
    UInt8 control[24 + 4]{};
    __builtin_memcpy(control, &kClientHandle, 4);
    __builtin_memcpy(control + 4, &kSubdevice, 4);
    __builtin_memcpy(control + 8, &kCmd, 4);
    __builtin_memcpy(control + 16, &kBytes, 4);
    pingOutstanding_ = true;
    clock_get_uptime(&pingSentAt_);
    if (!init_.enqueueRpc(76, control, sizeof(control))) {
        pingOutstanding_ = false;
        ulk();
        return kIOReturnIOError;
    }
    // 0.100.11: GSP-RM signals SWGEN0 only for asynchronous events; RPC
    // replies are polled (nouveau r535_gsp_msgq_wait, NVIDIA _kgspRpcRecvPoll).
    for (UInt32 ms = 0; pingOutstanding_ && ms < 7000; ++ms) {
        pollStatusLocked();
        if (!pingOutstanding_) break;
        ulk();
        IOSleep(1);
        lk(__LINE__);
    }
    const bool done = !pingOutstanding_;
    UInt64 ns = 0;
    if (done) absolutetime_to_nanoseconds(pingDoneAt_ - pingSentAt_, &ns);
    ++pings_;
    setProperty("NVGspControl-ping-count", pings_, 32);
    setProperty("NVGspControl-ping-last-ns", ns, 64);
    setProperty("NVGspControl-ping-last-via-intr", done && pingViaIntr_);
    // 0.73.0: interrupt-path snapshot after each ping (read-only).
    if (intrBar0_ && vec_[kVecGsp] < 256) {
        Bar0Io bar0{intrBar0_};
        UInt32 d[8]{};
        const UInt32 leaf = vec_[kVecGsp] / 32;
        bar0.read(0xB81000 + leaf * 4, &d[0]);          // LEAF
        bar0.read(0xB81200 + leaf * 4, &d[1]);          // LEAF_EN
        bar0.read(0xB81600, &d[2]);                     // TOP(0)
        bar0.read(0xB81608, &d[3]);                     // TOP_EN(0)
        bar0.read(nvgsp::falcon::kBase + 0x008, &d[4]); // falcon IRQSTAT
        bar0.read(0x111528, &d[5]);                     // RISCV IRQMASK
        bar0.read(0x11152c, &d[6]);                     // RISCV IRQDEST
        d[7] = pci_->configRead16(0x68 + 2) | (UInt32(pci_->configRead16(0x06)) << 16);
        setProperty("NVGspControl-intr-diag", d, sizeof(d));
    }
    *nsOut = ns;
    *viaIntrOut = done && pingViaIntr_;
    ulk();
    return done ? kIOReturnSuccess : kIOReturnTimeout;
}

// 0.75.0: in-kernel API for NVFramebuffer without a link dependency:
// callPlatformFunction("nvgsp-vblank-register", false, fn, ref, 0, 0) where
// fn is void (*)(void *ref, UInt32 count, UInt64 uptimeAbs), called from the
// MSI workloop on every head-0 vblank. "nvgsp-vblank-unregister" removes it.
IOReturn NVGspControl::callPlatformFunction(const OSSymbol *name, bool wait,
                                            void *p1, void *p2, void *p3,
                                            void *p4) {
    // 0.84.0: hardware cursor API for NVDisplay.
    if (name && name->isEqualTo("nvgsp-cursor-image"))
        return cursorImage(static_cast<const UInt32 *>(p1),
                           static_cast<UInt32>(reinterpret_cast<uintptr_t>(p2)),
                           static_cast<UInt32>(reinterpret_cast<uintptr_t>(p3)));
    if (name && name->isEqualTo("nvgsp-cursor-move"))
        return cursorMove(static_cast<SInt32>(reinterpret_cast<intptr_t>(p1)),
                          static_cast<SInt32>(reinterpret_cast<intptr_t>(p2)));
    if (name && name->isEqualTo("nvgsp-cursor-show"))
        return cursorShow(p1 != nullptr);
    if (name && name->isEqualTo("nvgsp-dpms"))
        return dpSetPower(p1 != nullptr);
    // 0.143.0 (M28): p1 = SInt32 *milli-degrees C. GPU temperature from the
    // thermal sensor at 0x020460 (nouveau gp100_temp_get): bits 16:3 are
    // degrees in 1/256 steps; bit 29 = valid, bit 30 = shadowed copy (what
    // this card reports, ~45 C idle, which matches the Linux reading).
    if (name && name->isEqualTo("nvgsp-gpu-temp")) {
        if (!p1) return kIOReturnBadArgument;
        if (sleeping_) return kIOReturnNotReady;
        UInt32 v = 0;
        if (peekBar0(0x020460, 1, &v) != kIOReturnSuccess || !(v & 0x60000000U))
            return kIOReturnNotReady;
        const SInt32 mC = static_cast<SInt32>(((v & 0x1fff8U) * 1000U) >> 8);
        *static_cast<SInt32 *>(p1) = mC;
        setProperty("NVGspControl-gpu-temp-mC", static_cast<UInt32>(mC), 32);
        return kIOReturnSuccess;
    }
    // 0.137.0: p1 = const UInt32[10] timing (see setMode), p2 = UInt32 *code.
    if (name && name->isEqualTo("nvgsp-set-mode")) {
        UInt32 code = ~0U;
        const IOReturn r = setMode(static_cast<const UInt32 *>(p1), &code);
        if (p2) *static_cast<UInt32 *>(p2) = code;
        return r;
    }
    // 0.152.0 (native N1) for NVAccelerator, no link dependency:
    // "nvgsp-stamp-region"   p1 = UInt64[3] out {VRAM/BAR1 offset, GPU VA, bytes}
    // "nvgsp-stamp-register" p1 = StampFn, p2 = ref (p1 null: unregister)
    // "nvgsp-stamp-poll"     advance CPU-ordered stamps after real ring fences
    // "nvgsp-submit-stamp"   p1 = NVGspKernelSubmit* (see NVGspControl.hpp)
    if (name && name->isEqualTo("nvgsp-stamp-region")) {
        if (!p1) return kIOReturnBadArgument;
        if (!grPersistent_ || !ctxBackingOffset_ || !lock_) return kIOReturnNotReady;
        const UInt64 phys = ctxBackingOffset_ + kStampCtxOff;
        if (!stampZeroed_) {
            lk(__LINE__);
            static const UInt32 zero[256] = {};
            bool ok = true;
            for (UInt64 o = 0; ok && o < kStampBytes; o += sizeof(zero))
                ok = ringWrite(phys + o, zero, 256);
            stampZeroed_ = ok;
            ulk();
            if (!ok) return kIOReturnIOError;
        }
        UInt64 *out = static_cast<UInt64 *>(p1);
        out[0] = phys; out[1] = 0x104000000ULL + kStampCtxOff; out[2] = kStampBytes;
        if (p2) {   // retained BAR1 view of the region for the family's stamp mapping
            IODeviceMemory *b1 = bar1Dev();
            IOMemoryDescriptor *md = b1 && phys + kStampBytes <= b1->getLength()
                ? IOSubMemoryDescriptor::withSubRange(b1, phys, kStampBytes, kIODirectionInOut) : nullptr;
            *static_cast<IOMemoryDescriptor **>(p2) = md;
            if (!md) return kIOReturnNoMemory;
        }
        return kIOReturnSuccess;
    }
    if (name && name->isEqualTo("nvgsp-stamp-poll")) {
        if (!lock_) return kIOReturnNotReady;
        lk(__LINE__);
        stampAdvanceLocked();
        ulk();
        return kIOReturnSuccess;
    }
    if (name && name->isEqualTo("nvgsp-stamp-register")) {
        if (!lock_) return kIOReturnNotReady;
        lk(__LINE__);
        stampFn_ = reinterpret_cast<StampFn>(p1);
        stampRef_ = p1 ? p2 : nullptr;
        ulk();
        return kIOReturnSuccess;
    }
    if (name && name->isEqualTo("nvgsp-submit-stamp")) {
        NVGspKernelSubmit *ks = static_cast<NVGspKernelSubmit *>(p1);
        if (!ks || ks->version != 1) return kIOReturnBadArgument;
        const UInt64 region = 0x104000000ULL + kStampCtxOff;
        if (ks->stampVa && (ks->stampVa < region || ks->stampVa + 4 > region + kStampBytes))
            return kIOReturnBadArgument;
        return submitSegments(ks->owner, ks->engine, ks->va, ks->dwords, ks->flags, ks->n,
                              &ks->seqOut, ks->stampVa, ks->stampValue);
    }
    // 0.159.0: PGRAPH status for utilization sampling (Activity Monitor):
    // *p1 = NV_PGRAPH_STATUS (bit 0 busy). A plain BAR0 read, no lock.
    if (name && name->isEqualTo("nvgsp-gr-busy")) {
        UInt32 *out = static_cast<UInt32 *>(p1);
        if (!out || !pci_ || !grPersistent_) return kIOReturnNotReady;
        IOMemoryMap *m = sharedBar0Map(pci_);
        const bool ok = m && m->getLength() >= 0x400704 && Bar0Io{m}.read(0x400700, out);
        if (m) m->release();
        return ok ? kIOReturnSuccess : kIOReturnIOError;
    }
    // 0.167.0: "nvgsp-ring-progress" p1 = UInt32[4] out {GR submitted, GR
    // done, CE submitted, CE done}. The display pipe notes the submitted
    // values when WindowServer queues a flip and copies the surface only once
    // the done values have passed them: the family queues a flip before the
    // GPU has drawn it and we track no per-resource events.
    if (name && name->isEqualTo("nvgsp-ring-progress")) {
        UInt32 *out = static_cast<UInt32 *>(p1);
        if (!out || !lock_) return kIOReturnBadArgument;
        lk(__LINE__);
        Ring r{};
        UInt32 v = 0;
        out[0] = out[1] = out[2] = out[3] = 0;
        bool ok = true;
        if (anyClientChannelLocked()) {   // 0.178.12: GR work is on many rings: the ordered stamp
            stampAdvanceLocked();
            out[0] = stampQueued_; out[1] = stampQHead_ == stampQTail_ ? stampQueued_ : stampWritten_;
        } else if (grRing(&r)) { out[0] = *r.seq; ok = readRingSem(r, &v); out[1] = v; } else { ok = false; }
        if (ok && ceRing(&r)) { out[2] = *r.seq; v = 0; ok = readRingSem(r, &v); out[3] = v; }
        ulk();
        return ok ? kIOReturnSuccess : kIOReturnNotReady;
    }
    if (name && name->isEqualTo("nvgsp-flip-copy"))                // 0.154.0
        return flipCopy(static_cast<NVGspFlipCopy *>(p1));
    if (name && name->isEqualTo("nvgsp-hotplug-register")) {
        hotplugFn_ = reinterpret_cast<HotplugFn>(p1);
        hotplugRef_ = p2;
        return armHotplug();
    }
    // 0.173.0: "nvgsp-reset-register" p1 = ResetDoneFn(void *ref), p2 = ref
    // (p1 null: unregister p2). Called after a GPU reset (or S3 re-boot) brought
    // GR back, without lock_: the display pipe puts its last frame back into
    // the scan-out memory the reset wiped (the screen stayed black until the
    // next WindowServer flip, 1 Oct 08:00).
    if (name && name->isEqualTo("nvgsp-reset-register")) {
        if (!lock_) return kIOReturnNotReady;
        lk(__LINE__);
        IOReturn ret = kIOReturnNoResources;
        for (UInt32 c = 0; c < 4; ++c) {
            if (p1 && !resetDoneFn_[c]) {
                resetDoneFn_[c] = reinterpret_cast<ResetDoneFn>(p1); resetDoneRef_[c] = p2;
                ret = kIOReturnSuccess; break;
            }
            if (!p1 && resetDoneRef_[c] == p2) {
                resetDoneFn_[c] = nullptr; resetDoneRef_[c] = nullptr;
                ret = kIOReturnSuccess; break;
            }
        }
        ulk();
        return ret;
    }
    const bool reg = name && name->isEqualTo("nvgsp-vblank-register");
    const bool unreg = name && name->isEqualTo("nvgsp-vblank-unregister");
    if (!reg && !unreg)
        return super::callPlatformFunction(name, wait, p1, p2, p3, p4);
    if (!lock_) return kIOReturnNotReady;
    lk(__LINE__);
    IOReturn ret = kIOReturnNoResources;
    for (UInt32 c = 0; c < kMaxVblankClients; ++c) {
        if (reg && !vblankFn_[c]) {
            vblankFn_[c] = reinterpret_cast<VblankFn>(p1);
            vblankRef_[c] = p2;
            ret = kIOReturnSuccess;
            break;
        }
        if (unreg && vblankFn_[c] == reinterpret_cast<VblankFn>(p1) &&
            vblankRef_[c] == p2) {
            vblankFn_[c] = nullptr;
            vblankRef_[c] = nullptr;
            ret = kIOReturnSuccess;
            break;
        }
    }
    UInt32 n = 0;
    for (UInt32 c = 0; c < kMaxVblankClients; ++c) n += vblankFn_[c] != nullptr;
    setProperty("NVGspControl-vblank-clients", n, 32);
    ulk();
    return ret;
}

// 0.80.0: live debug surface (no reboot per experiment).
IOReturn NVGspControl::peekBar0(UInt32 offset, UInt32 count, UInt32 *out) {
    if (!pci_ || !out || !count || count > 64 || (offset & 3)) return kIOReturnBadArgument;
    IOMemoryMap *map = sharedBar0Map(pci_);
    IOReturn ret = kIOReturnNoMemory;
    if (map && map->getLength() >= UInt64(offset) + count * 4) {
        Bar0Io bar0{map};
        for (UInt32 i = 0; i < count; ++i) bar0.read(offset + i * 4, &out[i]);
        ret = kIOReturnSuccess;
    }
    if (map) map->release();
    return ret;
}

IOReturn NVGspControl::vramAccess(UInt64 offset, UInt32 *words, UInt32 count, bool write) {
    // 0.81.0: VRAM read/write through the PRAMIN window under lock_ (the
    // window at BAR0 0x1700 is shared with every in-kernel PRAMIN user).
    if (!lock_ || !pci_ || !words || !count || count > 1024 || (offset & 3))
        return kIOReturnBadArgument;
    lk(__LINE__);
    bool ok = true;
    if (write) {
        ok = praminWriteWords(pci_, offset, words, count);
    } else {
        // The public DWORD ABI allows offset&7 == 4; PTE access does not.
        // Consume that leading high DWORD, then read aligned pairs.
        UInt32 i = 0;
        if (offset & 4) {
            UInt64 v = 0;
            PraminPteResult r{};
            ok = praminPteAccess(pci_, offset - 4, &v, false, false, &r);
            if (ok) words[i++] = static_cast<UInt32>(v >> 32);
        }
        for (; ok && i < count; i += 2) {
            UInt64 v = 0;
            PraminPteResult r{};
            ok = praminPteAccess(pci_, offset + i * 4, &v, false, false, &r);
            words[i] = static_cast<UInt32>(v);
            if (i + 1 < count) words[i + 1] = static_cast<UInt32>(v >> 32);
        }
    }
    ulk();
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn NVGspControl::pokeBar0(UInt32 offset, UInt32 value) {
    // Display engine window only (PDISP 0x610000-0x6FFFFF incl. channel
    // user areas); everything else stays read-only from userspace.
    // 0.100.2: + PMGR VPLL block 0x00e000-0x00effc (display PLL bring-up).
    if (!pci_ || (offset & 3) ||
        !((offset >= 0x610000 && offset <= 0x6FFFFC) ||
          (offset >= 0x00e000 && offset <= 0x00effc)))
        return kIOReturnNotPermitted;
    IOMemoryMap *map = sharedBar0Map(pci_);
    IOReturn ret = kIOReturnNoMemory;
    if (map && map->getLength() > offset + 4) {
        Bar0Io bar0{map};
        ret = bar0.write(offset, value) ? kIOReturnSuccess : kIOReturnIOError;
    }
    if (map) map->release();
    return ret;
}

// Dedicated admin probe ABI: resolve the reserved channel token through RM.
// No arbitrary MMIO offset, production channel or caller token is accepted.
IOReturn NVGspControl::diagnosticChannelKick(UInt32 handle) {
    if (handle != 0xc0e9006f) return kIOReturnBadArgument;
    const UInt32 generation = resetGeneration();
    UInt32 req[7] = {0xc0d00001, handle, 0xc36f0108, 0, 4, 0, 0};
    UInt8 reply[112]{};
    UInt32 bytes = sizeof(reply), result = ~0U, status = ~0U, token = 0;
    const IOReturn rpc = userRpc(76, reinterpret_cast<const UInt8 *>(req), sizeof(req), reply, &bytes, &result);
    if (rpc != kIOReturnSuccess) return rpc;
    if (bytes >= 108) {
        __builtin_memcpy(&status, reply + 92, 4);
        __builtin_memcpy(&token, reply + 104, 4);
    }
    if (result || status || !token || (token & 0xffff) != 9)
        return kIOReturnNotReady;
    lk(__LINE__);
    IOReturn ret = kIOReturnOffline;
    if (!resetBusy_ && generation == resetGeneration()) {
        IOMemoryMap *map = sharedBar0Map(pci_);
        ret = kIOReturnNoMemory;
        if (map && map->getLength() >= 0x00BB0094) {
            Bar0Io bar0{map};
            ret = bar0.write(0x00BB0090, token) ? kIOReturnSuccess : kIOReturnIOError;
            if (ret == kIOReturnSuccess) setProperty("NVGspControl-probe-channel-token", token, 32);
        }
        if (map) map->release();
    }
    ulk();
    return ret;
}

// One RPC at a time owns userRpcReply_. Callers used to get kIOReturnBusy
// straight away when another thread's RPC was in flight; after an RC the
// perf-boost RPC can sit on the slot for seconds, so the reset's FBSR memlist
// failed and the GPU stayed dead until reboot. Now wait for the slot.
void NVGspControl::ulk() {
    UInt64 now = 0, ns = 0;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now - lockAt_, &ns);
    if (ns > 10000000ULL) ++lockHoldsOver10ms_;
    if (ns > 5000000ULL) {   // per-line table of the long holds
        UInt32 i = 0;
        while (i < 16 && longHolds_[i].line && longHolds_[i].line != lockLine_) ++i;
        if (i < 16) {
            longHolds_[i].line = lockLine_;
            ++longHolds_[i].count;
            if (ns > longHolds_[i].maxNs) longHolds_[i].maxNs = ns;
        }
    }
    if (ns > lockHoldMaxNs_) {
        UInt64 upNs = 0;
        absolutetime_to_nanoseconds(now, &upNs);
        lockHoldMaxNs_ = ns;
        lockHoldMaxLine_ = lockLine_;
        lockHoldMaxPhase_ = postInitPhase_;
        lockHoldMaxUptimeMs_ = upNs / 1000000ULL;
    }
    IOLockUnlock(lock_);
}

int NVGspControl::sleepLk(void *event, UInt64 deadline, UInt32 interType) {
    const UInt32 line = lockLine_;
    UInt64 now = 0, ns = 0;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now - lockAt_, &ns);
    if (ns > lockHoldMaxNs_) { lockHoldMaxNs_ = ns; lockHoldMaxLine_ = line; lockHoldMaxPhase_ = postInitPhase_; }
    const int r = deadline ? IOLockSleepDeadline(lock_, event, deadline, interType)
                           : IOLockSleep(lock_, event, interType);
    clock_get_uptime(&lockAt_);
    lockLine_ = line;
    return r;
}

void NVGspControl::markBoot(const char *tag) {
    UInt64 now = 0, ns = 0;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &ns);
    if (bootTlLen_ + 40 >= sizeof(bootTl_)) return;
    const int n = snprintf(bootTl_ + bootTlLen_, sizeof(bootTl_) - bootTlLen_, "%s@%llu ",
                           tag, (unsigned long long)(ns / 1000000ULL));
    if (n > 0) bootTlLen_ += static_cast<UInt32>(n);
    setProperty("NVGspControl-boot-timeline", bootTl_);
}

bool NVGspControl::waitRpcSlotLocked(UInt32 ms) {
    for (UInt32 t = 0;; ++t) {
        if (postInitPhase_ != 33 || sleeping_) return false;
        if (!userRpcActive_ && !userRpcOutstanding_ && !pingOutstanding_) return true;
        if (t >= ms) return false;
        ulk();
        IOSleep(1);
        lk(__LINE__);
    }
}

IOReturn NVGspControl::userRpc(UInt32 function, const UInt8 *params, UInt32 bytes,
                               UInt8 *reply, UInt32 *replyBytes, UInt32 *rpcResult) {
    if (!lock_ || !params || bytes > 3900 || !reply || !replyBytes) return kIOReturnBadArgument;
    lk(__LINE__);
    if (!waitRpcSlotLocked(6000)) {
        ulk();
        return kIOReturnBusy;
    }
    // The interrupt/poll thread clears outstanding when the reply arrives.
    // Keep ownership until this caller copies it: another sleeping caller
    // can acquire lock_ before we do and otherwise overwrite our reply.
    userRpcActive_ = true;
    userRpcFunction_ = function;
    userRpcOutstanding_ = true;
    userRpcReplyBytes_ = 0;
    userRpcSeq_ = 0x80000000U | (++userRpcSeqCounter_ & 0x7fffffffU);   // 0.178.9
    if (!init_.enqueueRpc(function, params, bytes, userRpcSeq_)) {
        userRpcOutstanding_ = false;
        userRpcActive_ = false;
        ulk();
        return kIOReturnIOError;
    }
    // Drive the status queue ourselves (no dependency on the daemon poll).
    for (UInt32 ms = 0; userRpcOutstanding_ && ms < 5000; ++ms) {
        pollStatusLocked();
        if (!userRpcOutstanding_) break;
        ulk();
        IOSleep(1);
        lk(__LINE__);
    }
    const bool done = !userRpcOutstanding_;
    userRpcOutstanding_ = false;
    const UInt32 n = done ? (userRpcReplyBytes_ < *replyBytes ? userRpcReplyBytes_ : *replyBytes) : 0;
    if (n) __builtin_memcpy(reply, userRpcReply_, n);
    *replyBytes = n;
    if (rpcResult) *rpcResult = done ? userRpcResult_ : ~0U;
    userRpcActive_ = false;
    ++userRpcs_;
    setProperty("NVGspControl-user-rpc-count", userRpcs_, 32);
    setProperty("NVGspControl-user-rpc-last-function", function, 32);
    ulk();
    return done ? kIOReturnSuccess : kIOReturnTimeout;
}

// 0.83.0: kernel-managed core channel pushbuffer (4 KiB at
// dispPbBackingOffset_, PUT/GET at BAR0 0x680000/4). Words are appended at
// corePut_; the PB wraps with a DMA JUMP to 0 like the window PB. Waits for
// GET == PUT (fetched), not for the UPDATE to arm.
IOReturn NVGspControl::submitCore(const UInt32 *words, UInt32 count) {
    if (!lock_ || !pci_ || !words || !count || count > 256) return kIOReturnBadArgument;
    lk(__LINE__);
    IOReturn ret = kIOReturnNotReady;
    if (dispPbBackingOffset_ && corePut_) {
        UInt32 base = corePut_;
        bool ok = true;
        if (base + count * 4 + 4 > 4096) {
            const UInt32 jump[1] = {0x20000000U};
            ok = praminWriteWords(pci_, dispPbBackingOffset_ + base, jump, 1);
            base = 0;
        }
        ok = ok && praminWriteWords(pci_, dispPbBackingOffset_ + base, words, count);
        ret = kIOReturnIOError;
        IOMemoryMap *map = ok ? sharedBar0Map(pci_) : nullptr;
        if (map && map->getLength() >= 0x00680008) {
            Bar0Io bar0{map};
            const UInt32 put = base + count * 4;
            UInt32 get = 0;
            bar0.write(0x00680000, put);
            corePut_ = put;
            ret = kIOReturnTimeout;
            for (UInt32 i = 0; i < 4000; ++i) {
                if (bar0.read(0x00680004, &get) && get == put) { ret = kIOReturnSuccess; break; }
                IODelay(25);
            }
            setProperty("NVGspControl-core-get", get, 32);
        }
        if (map) map->release();
    }
    setProperty("NVGspControl-core-put", corePut_, 32);
    setProperty("NVGspControl-core-submit-result", static_cast<UInt32>(ret), 32);
    ulk();
    return ret;
}

// 0.138.0 (D1): the VBIOS leaves SET_WINDOW_INTERLOCK_FLAGS = window 0
// armed, so a core UPDATE that does not rewrite the interlocks waits for a
// window-0 UPDATE that is interlocked with the core, and every later core
// method (cursor, OLUT, notifier) queues behind it. That stall is what made
// core ctxdma methods look broken (27 Sep: with it cleared, a notifier
// ctxdma arms and the notifier is written). IGNORE_INTERLOCK releases it.
IOReturn NVGspControl::coreUnstick() {
    auto accel = [this](UInt32 value) {
        UInt8 ctrl[24 + 20]{};
        const UInt32 hdr[6] = {0xc0d00001, 0xc0d0c770, 0xc3700102, 0, 20, 0};
        const UInt32 prm[5] = {0, nvgsp::kDispCoreChannelDma, 0, value, 8};
        __builtin_memcpy(ctrl, hdr, sizeof(hdr));
        __builtin_memcpy(ctrl + 24, prm, sizeof(prm));
        UInt8 reply[256];
        UInt32 replyBytes = sizeof(reply), result = ~0U;
        return userRpc(76, ctrl, sizeof(ctrl), reply, &replyBytes, &result);
    };
    const UInt32 clear[] = {(2U << 18) | 0x218, 0, 0, (1U << 18) | 0x200, 1};
    IOReturn r = submitCore(clear, sizeof(clear) / 4);
    if (r == kIOReturnTimeout) {
        accel(8);   // NVC370_CTRL_ACCL_IGNORE_INTERLOCK
        IOSleep(20);
        r = submitCore(clear, sizeof(clear) / 4);
        accel(0);
        setProperty("NVGspControl-core-unstick", static_cast<UInt32>(r), 32);
    }
    return r;
}

UInt32 NVGspControl::coreException() {
    UInt32 stat = 0;
    IOMemoryMap *map = sharedBar0Map(pci_);
    if (map && map->getLength() >= 0x611858) {
        Bar0Io bar0{map};
        UInt32 pending = 0;
        // FE_EXCEPT retains an ACKed snapshot, including VALID/type NONE.
        // Only the live core interrupt source establishes a pending fault.
        if (bar0.read(0x611854, &pending) && (pending & 1U) &&
            bar0.read(0x611020, &stat) && (stat & 0x10000000U)) {
            UInt32 data = 0, code = 0;
            bar0.read(0x611024, &data);
            bar0.read(0x611028, &code);
            bar0.write(0x611020, 0x90000000U);   // ack, as nouveau does
            const UInt32 rec[3] = {stat, data, code};
            OSData *d = OSData::withBytes(rec, sizeof(rec));
            if (d) { setProperty("NVGspControl-core-exception", d); d->release(); }
        } else {
            stat = 0;
        }
    }
    if (map) map->release();
    return stat;
}

IOReturn NVGspControl::waitCursorArmed(UInt32 usage, const UInt32 state[7]) {
    if (!pci_ || !state) return kIOReturnBadArgument;
    IOMemoryMap *map = sharedBar0Map(pci_);
    if (!map || map->getLength() < 0x68a0a4) {
        if (map) map->release();
        return kIOReturnNoMemory;
    }
    Bar0Io bar0{map};
    IOReturn result = kIOReturnTimeout;
    for (UInt32 ms = 0; ms < 200; ++ms) {
        UInt32 pending = 0, cs = 0, put = 0, get = 0, assyUsage = 0, armedUsage = 0;
        UInt32 assy[7] = {}, armed[7] = {};
        bool ok = bar0.read(0x611854, &pending) && bar0.read(0x610630, &cs) &&
            bar0.read(0x680000, &put) && bar0.read(0x680004, &get) &&
            bar0.read(0x682030, &assyUsage) && bar0.read(0x68a030, &armedUsage);
        for (UInt32 i = 0; ok && i < 7; ++i)
            ok = bar0.read(0x682088 + i*4, &assy[i]) && bar0.read(0x68a088 + i*4, &armed[i]);
        if (!ok) { result = kIOReturnIOError; break; }
        if (pending & 1U) { coreException(); result = kIOReturnIOError; break; }
        if (((cs >> 16) & 0x1f) == 0x0b && put == get &&
            assyUsage == usage && armedUsage == usage &&
            !__builtin_memcmp(assy, state, sizeof(assy)) &&
            !__builtin_memcmp(armed, state, sizeof(armed))) {
            result = kIOReturnSuccess;
            break;
        }
        IOSleep(1);
    }
    map->release();
    setProperty("NVGspControl-cursor-arm-result", static_cast<UInt32>(result), 32);
    return result;
}

// 0.84.0: hardware cursor (head 0), nvkms EvoSetCursorImageC3 + MoveCursorC3
// with nouveau r535_curs_init ordering. 64x64 A8R8G8B8, two 16 KiB buffers
// at the tail of the scratch VRAM (scratch + 0xFF00000); the VRAM ctxdma the
// window uses (RAMIN+0x2000) gets a core-channel (chid 0) RAMHT entry.
static constexpr UInt32 kCursorCtxdma = 0xc0d0d002;
// Dual legacy/client hash slots must not alias the cursor's 56/60 slots.
// The old OLUT handle D006 used 52/56; D00A uses 48/52 instead.
static constexpr UInt32 kOlutCtxdma = 0xc0d0d00a;
static constexpr UInt32 kIlutCtxdma = 0xc0d0d007;
// 0.101.0: see the call site. Returns the number of PD0 tables installed.
UInt32 NVGspControl::installVramWindow() {
    constexpr UInt64 kVramVa = 0x2000000000ULL;
    constexpr UInt64 kTablesOff = 0xF800000ULL;
    const UInt64 pages = ((fbFreeLimit_ + 1) & ~0x1fffffULL) >> 21;
    const UInt32 tables = static_cast<UInt32>((pages + 255) / 256);
    const UInt32 first = static_cast<UInt32>(kVramVa >> 29) & 0x1ff;
    const UInt64 tabBase = scratchOffset_ + kTablesOff;
    if (!pages || tables > 64 || (scratchTry_ < kTablesOff + tables * 4096ULL))
        return 0;
    PraminPteResult r{};
    UInt64 e = 0, pd2 = 0, pd1 = 0;
    if (!praminPteAccess(pci_, pdbAddress_, &e, false, false, &r) || ((e >> 1) & 3) != 1)
        return 0;
    pd2 = ((e >> 8) & ((1ULL << 46) - 1)) << 12;
    if (!praminPteAccess(pci_, pd2, &e, false, false, &r) || ((e >> 1) & 3) != 1)
        return 0;
    pd1 = ((e >> 8) & ((1ULL << 46) - 1)) << 12;
    setProperty("NVGspControl-vram-va-pd1", pd1, 64);
    for (UInt32 t = 0; t < tables; ++t) {
        if (!praminPteAccess(pci_, pd1 + (first + t) * 8ULL, &e, false, false, &r) || e)
            return 0;   // slot taken: leave RM's tables alone
    }
    if (!praminZeroRange(pci_, tabBase, tables * 4096ULL)) return 0;
    for (UInt32 t = 0; t < tables; ++t) {
        const UInt64 firstPage = UInt64(t) * 256;
        const UInt32 n = static_cast<UInt32>(pages - firstPage < 256 ? pages - firstPage : 256);
        if (!praminWritePteRun(pci_, tabBase + t * 4096ULL, (firstPage << 21) >> 12, n,
                               1ULL, 16, 0x200))
            return 0;
    }
    for (UInt32 t = 0; t < tables; ++t) {
        UInt64 pde = (((tabBase + t * 4096ULL) >> 12) << 8) | 2;   // vidmem PDE
        if (!praminPteAccess(pci_, pd1 + (first + t) * 8ULL, &pde, true, true, &r))
            return t;
    }
    setProperty("NVGspControl-vram-va-base", kVramVa, 64);
    setProperty("NVGspControl-vram-va-bytes", pages << 21, 64);
    return tables;
}

// 0.102.0: see the call site. PTE aperture SYSTEM_COHERENT (2) + VOL -> |0xD.
UInt32 NVGspControl::installSharedWindow() {
    constexpr UInt64 kShmVa = 0x3000000000ULL;
    constexpr UInt64 kTableOff = 0xF820000ULL;   // after the 32 VRAM-window tables
    const UInt32 first = static_cast<UInt32>(kShmVa >> 29) & 0x1ff;
    const UInt64 table = scratchOffset_ + kTableOff;
    PraminPteResult r{};
    UInt64 e = 0, pd1 = 0;
    if (!pdbAddress_ || !praminPteAccess(pci_, pdbAddress_, &e, false, false, &r)) return 0;
    UInt64 pd2 = ((e >> 8) & ((1ULL << 46) - 1)) << 12;
    if (!praminPteAccess(pci_, pd2, &e, false, false, &r)) return 0;
    pd1 = ((e >> 8) & ((1ULL << 46) - 1)) << 12;
    if (!praminPteAccess(pci_, pd1 + first * 8ULL, &e, false, false, &r) || e) return 0;
    if (!praminZeroRange(pci_, table, 4096)) return 0;
    UInt32 n = 0;
    for (; n < kShmChunks; ++n) {
        IOBufferMemoryDescriptor *b = shmChunk_[n];   // 0.104.0: reuse after reset
        const bool fresh = !b;
        if (fresh) {
            b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous |
                kIOMemoryKernelUserShared, 0x200000, 0x000000FFFFE00000ULL);
            if (!b) break;
            if (b->prepare() != kIOReturnSuccess) { b->release(); break; }
        }
        IOByteCount len = 0;
        const UInt64 phys = b->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
        if (!phys || (phys & 0x1fffff) || len < 0x200000) {
            if (fresh) { b->complete(); b->release(); }
            break;
        }
        if (fresh) bzero(b->getBytesNoCopy(), 0x200000);
        // 0.103.1: VOL (bit 3) = uncached in GPU L2. Without it, CPU rewrites
        // of reused QMD/cbuf slots were not seen (stale L2 lines, 20k async
        // launches: missing outputs from the 3rd ring round on).
        UInt64 pte = ((phys >> 12) << 8) | 0xD;
        if (!praminPteAccess(pci_, table + n * 16ULL, &pte, true, true, &r)) {
            if (fresh) { b->complete(); b->release(); }
            break;
        }
        shmChunk_[n] = b;
    }
    if (!n) return 0;
    UInt64 pde = ((table >> 12) << 8) | 2;
    if (!praminPteAccess(pci_, pd1 + first * 8ULL, &pde, true, true, &r)) return 0;
    if (!shmUser_)
        shmUser_ = IOMultiMemoryDescriptor::withDescriptors(
            reinterpret_cast<IOMemoryDescriptor **>(shmChunk_), n, kIODirectionInOut, false);
    setProperty("NVGspControl-shm-va", kShmVa, 64);
    setProperty("NVGspControl-shm-bytes", UInt64(n) << 21, 64);
    return n;
}

// 0.105.0: GPU TLB + PDE-cache invalidate for our VAS from the CPU through
// the Turing+ virtual-function MMU registers (nouveau tu102_vmm_flush; works
// with GSP-RM, unlike the privileged host MEM_OP from our channel = Xid 32).
IOReturn NVGspControl::tlbInvalidate() {
    if (!pci_ || !pdbAddress_) return kIOReturnNotReady;
    IOMemoryMap *map = sharedBar0Map(pci_);
    IOReturn ret = kIOReturnNoMemory;
    if (map && map->getLength() >= 0xb830b4) {
        Bar0Io bar0{map};
        bar0.write(0xb830a0, static_cast<UInt32>(pdbAddress_ >> 8));
        bar0.write(0xb830a4, 0);
        // 0.178.3: with per-client channels (own VAS/PDB) live, ALL_PDB too:
        // a client's arena changes and a reused PDB must reach their TLBs
        bool own = false;
        for (UInt32 i = 0; i < kMaxClientChannels; ++i) own |= cchan_[i].state != 0;
        bar0.write(0xb830b0, own ? 0x80000003 : 0x80000001);   // trigger | ALL_VA (| ALL_PDB)
        ret = kIOReturnTimeout;
        for (UInt32 i = 0; i < 20000; ++i) {
            UInt32 v = 0;
            if (bar0.read(0xb830b0, &v) && !(v & 0x80000000)) { ret = kIOReturnSuccess; break; }
            IODelay(1);
        }
        ++tlbInvalidates_;
    }
    if (map) map->release();
    setProperty("NVGspControl-tlb-invalidates", tlbInvalidates_, 32);
    setProperty("NVGspControl-tlb-invalidate-last", static_cast<UInt32>(ret), 32);
    return ret;
}

// 0.111.0 per-client arenas ---------------------------------------------
// Every user client gets private page tables for the user VA arena
// (PD1[320..383], GPU VA 0x28_0000_0000..0x30_0000_0000): 64 PD0 tables =
// 256 KiB carved from a 4 MiB kext-owned VRAM pool (16 clients). Clients
// can therefore use the same VAs (NVK processes all start their VA heap at
// the same place) and cannot see each other's arena mappings. Before a
// client's work is submitted, arenaSwitchLocked() drains the GR/CE rings,
// points PD1[320..383] at that client's tables and flushes the TLB; while
// one client keeps submitting there is no switch cost.
// 0.114.0: the tables are managed by nvgsp::ArenaMap (NVGspArenaMap.hpp):
// 2 MiB PTEs where a whole aligned region maps contiguous memory, 64 KiB
// big-page tables (LPTs, from per-client 2 MiB VRAM chunks) otherwise.

bool NVGspControl::ArenaBackend::read64(UInt64 vram, UInt64 *v) {
    PraminPteResult r{};
    return praminPteAccess(d->pci_, vram, v, false, false, &r);
}

bool NVGspControl::ArenaBackend::write64(UInt64 vram, UInt64 v) {
    PraminPteResult r{};
    // installing: address/kind before VALID; removing: VALID first
    return praminPteAccess(d->pci_, vram, &v, true, (v & 1) != 0, &r);
}

bool NVGspControl::ArenaBackend::zero(UInt64 vram, UInt64 bytes) {
    return praminZeroRange(d->pci_, vram, bytes);
}

// 0.165.0: sparse tables in bulk, 4 KiB per PRAMIN write
bool NVGspControl::ArenaBackend::fill16(UInt64 vram, UInt64 bytes, UInt64 q0, UInt64 q1) {
    static UInt32 page[1024];
    for (UInt32 i = 0; i < 1024; i += 4) {
        page[i] = static_cast<UInt32>(q0); page[i + 1] = static_cast<UInt32>(q0 >> 32);
        page[i + 2] = static_cast<UInt32>(q1); page[i + 3] = static_cast<UInt32>(q1 >> 32);
    }
    for (UInt64 o = 0; o < bytes; o += 4096) {
        const UInt32 n = bytes - o >= 4096 ? 1024 : static_cast<UInt32>((bytes - o) / 4);
        if (!praminWriteWords(d->pci_, vram + o, page, n)) return false;
    }
    return true;
}

// LPT chunk: a 2 MiB VRAM object owned by the arena context (tag `ctx`),
// released with it.
bool NVGspControl::ArenaBackend::allocChunk(UInt64 *phys) {
    UInt32 h = 0;
    return d->memAllocLocked(ctx, 0x200000, 0, &h, phys) == kIOReturnSuccess;
}

void *NVGspControl::ArenaBackend::allocOwners(UInt32 bytes) { return IOMalloc(bytes); }
void NVGspControl::ArenaBackend::freeOwners(void *p, UInt32 bytes) { IOFree(p, bytes); }

// Free the LPT chunks of `c` (VRAM objects tagged with the context).
void NVGspControl::arenaFreeChunksLocked(ArenaCtx *c) {
    for (UInt32 i = 0; i < kMaxMem; ++i)
        if (mem_[i].owner == c) releaseGpuMemLocked(i);
}

// Caller holds lock_. `create` makes the context (and the pool) on demand.
// 0.165.0: unmapped user VA is sparse (reads 0, writes dropped) instead of an
// MMU fault that kills the channel, like the M1 with wild accesses. NVRAM
// nvgsp-sparse=0 brings the faults back (driver debugging).
static bool arenaSparseByNvram() {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    bool on = true;
    if (options) {
        OSObject *o = options->copyProperty("nvgsp-sparse");
        if (OSData *d = OSDynamicCast(OSData, o))
            on = !(d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '0');
        else if (OSString *str = OSDynamicCast(OSString, o))
            on = !str->isEqualTo("0");
        OSSafeReleaseNULL(o);
        options->release();
    }
    return on;
}

// .178.24: bounded diagnosis of private recovery after the newer native stamp
// fixes. Read only at the first global reset. A second reset or S3 disables
// this exception even if NVRAM remains set; normal fallback stays the default.
static bool postResetPrivateDiagnosticByNvram() {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    bool on = false;
    if (options) {
        OSObject *o = options->copyProperty("nvgsp-postreset-private");
        if (OSData *d = OSDynamicCast(OSData, o))
            on = d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '1';
        else if (OSString *str = OSDynamicCast(OSString, o)) on = str->isEqualTo("1");
        OSSafeReleaseNULL(o);
        options->release();
    }
    return on;
}

bool NVGspControl::privateChannelGenerationAllowed() const {
    return gpuResets_ == 0 || (resetPrivateDiagnostic_ && gpuResets_ == 1);
}

// 0.178.6: per-client GR channels for every client (NVRAM nvgsp-ownchannel=1)
bool NVGspControl::ownChannelAuto() {
    static int on = -1;
    if (on < 0) {
        IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
        on = 0;
        if (options) {
            OSObject *o = options->copyProperty("nvgsp-ownchannel");
            if (OSData *d = OSDynamicCast(OSData, o))
                on = d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '1';
            else if (OSString *str = OSDynamicCast(OSString, o))
                on = str->isEqualTo("1");
            OSSafeReleaseNULL(o);
            options->release();
        }
        setProperty("NVGspControl-ownchannel-auto", on != 0);
    }
    // .178.22: fresh-boot private-channel soaks pass, but after a global
    // fault/reset mixed work repeatedly hits RC109, even with WFI/quiesce.
    // Shared fallback stays the default; .24 can opt in for the first reset
    // only to retest recovery with .5.17 native stamp correctness fixes.
    return on != 0 && privateChannelGenerationAllowed();
}

NVGspControl::ArenaCtx *NVGspControl::arenaForLocked(const void *owner, bool create) {
    if (!owner) return nullptr;
    for (UInt32 i = 0; i < kMaxArenaCtx; ++i)
        if (arenaCtx_[i] && arenaCtx_[i]->owner == owner) {
            ArenaCtx *c = arenaCtx_[i];
            if (!c->ready && create) {                    // after a GPU reset
                IOLog("NVGspControl: arena slot %u (pid %d) rebuilt empty for owner %p\n", i, c->pid, owner);
                ArenaBackend b{this, c};
                arenaFreeChunksLocked(c);
                c->map.sparse = arenaSparseByNvram();
                c->ready = c->map.init(b, arenaPoolPhys_ + i * kArenaCtxBytes);
            }
            return c->ready || !create ? c : nullptr;
        }
    if (!create) return nullptr;
    if (!arenaPoolPhys_) {
        UInt32 h = 0;
        UInt64 phys = 0;
        if (memAllocLocked(&arenaPoolPhys_, kMaxArenaCtx * kArenaCtxBytes, 0, &h, &phys) !=
            kIOReturnSuccess)
            return nullptr;
        arenaPoolPhys_ = phys;
    }
    UInt32 slot = 0;
    while (slot < kMaxArenaCtx && arenaCtx_[slot]) ++slot;
    if (slot == kMaxArenaCtx) {
        // 0.172.0: reclaim a context left stale by a GPU reset. Its owner is
        // from before the reset and only ever gets Offline now (it reopens
        // with a new client), so its tables are dead weight.
        for (UInt32 i = 0; i < kMaxArenaCtx; ++i)
            if (arenaCtx_[i] && !arenaCtx_[i]->ready && arenaCtx_[i] != arenaActive_) {
                arenaFreeCtxLocked(i);
                ++arenaReclaims_;
                setProperty("NVGspControl-arena-reclaims", arenaReclaims_, 32);
                slot = i;
                break;
            }
        if (slot == kMaxArenaCtx) {
            ++arenaFull_;
            setProperty("NVGspControl-arena-full", arenaFull_, 32);
            return nullptr;
        }
    }
    ArenaCtx *c = static_cast<ArenaCtx *>(IOMalloc(sizeof(ArenaCtx)));
    if (!c) return nullptr;
    bzero(c, sizeof(*c));                                // ArenaMap needs zeroed state
    c->owner = owner;
    c->pid = proc_selfpid();                             // 0.158.0
    ArenaBackend b{this, c};
    c->map.sparse = arenaSparseByNvram();
    setProperty("NVGspControl-arena-sparse", c->map.sparse);
    c->ready = c->map.init(b, arenaPoolPhys_ + slot * kArenaCtxBytes);
    if (!c->ready) { IOFree(c, sizeof(ArenaCtx)); return nullptr; }
    arenaCtx_[slot] = c;
    setProperty("NVGspControl-arena-clients", slot + 1, 32);
    return c;
}

// Point PD1[320..383] at `c`'s tables (nullptr: no arena). Caller holds lock_.
bool NVGspControl::arenaWritePd1Locked(const ArenaCtx *c) {
    if (!pdbAddress_) return false;
    PraminPteResult r{};
    UInt64 e = 0;
    bool ok = praminPteAccess(pci_, pdbAddress_, &e, false, false, &r);
    const UInt64 pd2 = ((e >> 8) & ((1ULL << 46) - 1)) << 12;
    ok = ok && praminPteAccess(pci_, pd2, &e, false, false, &r);
    const UInt64 pd1 = ((e >> 8) & ((1ULL << 46) - 1)) << 12;
    for (UInt32 slot = 0; ok && slot < 64; ++slot) {
        UInt64 pde = c ? (((c->map.tables + slot * 4096ULL) >> 12) << 8) | 2 : 0;
        ok = praminPteAccess(pci_, pd1 + (320 + slot) * 8ULL, &pde, true, true, &r);
    }
    return ok;
}

// Wait until the GR, CE and video rings are idle (all submitted fences reached).
bool NVGspControl::drainEnginesLocked() {
    Ring ring{};
    if (grRing(&ring) && !waitRingSem(ring, subSeq_, 2000000)) return false;
    asyncOutstanding_ = 0;
    if (cePersistent_ && ceChunk_ && ceSeq_) {
        Ring ce{};
        if (ceRing(&ce) && !waitRingSem(ce, ceSeq_, 2000000)) return false;
        ceOutstanding_ = 0;
    }
    for (UInt32 i = 0; i < nvgsp::kVideoEngineCount; ++i) {   // 0.116.0
        Ring vr{};
        if (!videoRing(i, &vr) || !video_[i].seq || video_[i].dead) continue;
        // 0.122.0: a video ring that does not progress must not take GR/CE
        // (NVK) down with it: mark it dead instead of failing the drain.
        if (!waitRingSem(vr, video_[i].seq, 200000)) {
            videoMarkDeadLocked(i);
            continue;
        }
        video_[i].outstanding = 0;
    }
    return true;
}

// Make `owner`'s arena the live one before its work reaches the GPU. A
// client without an arena runs with none, so it cannot reach the previous
// client's mappings either. Caller holds lock_.
IOReturn NVGspControl::arenaSwitchLocked(const void *owner) {
    if (vramFrozen_) {                               // 0.127.0: sleep in progress
        const UInt32 gen = gpuResets_;
        waitThawLocked();
        if (gen != gpuResets_) return kIOReturnNotReady;   // GPU state lost meanwhile
    }
    ArenaCtx *target = arenaForLocked(owner, false);
    if (target && !target->ready) target = nullptr;
    // 0.168.0: never run a client's work without its tables. A submit from a
    // client whose arena is missing or not ready (after a reset, or while it
    // is set up) used to install no arena and run: its native slabs faulted
    // (1 Oct 04:57, va 0x2800a00000 "active -1 ... mapped by no client") and
    // the resets that followed did not bring GR back. NotReady sends the
    // client through its recovery, which rebinds into fresh tables.
    if (!target) {
        ++nullArenaRefusals_;
        setProperty("NVGspControl-null-arena-refusals", nullArenaRefusals_, 32);
        return kIOReturnNotReady;
    }
    if (arenaInstalled_ && arenaActive_ == target) return kIOReturnSuccess;
    if (!drainEnginesLocked()) return kIOReturnTimeout;
    if (!arenaWritePd1Locked(target)) return kIOReturnIOError;
    arenaActive_ = target;
    arenaInstalled_ = true;
    ++arenaSwitches_;
    setProperty("NVGspControl-arena-switches", arenaSwitches_, 32);
    return tlbInvalidate();                          // no lock_ inside
}

// 0.170.0: the last 16 GPU events (RCs, ring hangs, resets) with their
// uptime, as NVGspControl-gpu-events; kept across resets. Caller holds lock_.
void NVGspControl::noteGpuEventLocked(const char *what) {
    UInt64 now = 0, ns = 0;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &ns);
    snprintf(gpuEvents_[gpuEventCount_ % 16], sizeof(gpuEvents_[0]), "%llu.%03llu %s",
             ns / 1000000000ULL, (ns / 1000000ULL) % 1000, what);
    ++gpuEventCount_;
    OSArray *a = OSArray::withCapacity(16);
    if (!a) return;
    const UInt32 n = gpuEventCount_ < 16 ? gpuEventCount_ : 16;
    for (UInt32 k = gpuEventCount_ - n; k < gpuEventCount_; ++k)
        if (OSString *str = OSString::withCString(gpuEvents_[k % 16])) { a->setObject(str); str->release(); }
    setProperty("NVGspControl-gpu-events", a);
    a->release();
}

// 0.158.0: name the owner of a faulting user-arena VA: which client's
// tables map it (pid, object slot, page size) and whose arena the GPU was
// walking. A VA mapped only by another client's arena means work ran under
// the wrong tables; mapped by nobody means it outlived its memory. Caller
// holds lock_.
void NVGspControl::explainFaultLocked(UInt64 va) {
    char s[480];
    int n = 0;
    int activeSlot = -1;
    for (UInt32 i = 0; i < kMaxArenaCtx; ++i)
        if (arenaCtx_[i] && arenaCtx_[i] == arenaActive_) activeSlot = static_cast<int>(i);
    n += snprintf(s + n, sizeof(s) - n, "va 0x%llx active %d pid %d:", va, activeSlot,
                  activeSlot >= 0 ? arenaCtx_[activeSlot]->pid : 0);
    bool any = false;
    for (UInt32 i = 0; i < kMaxArenaCtx && n < static_cast<int>(sizeof(s)) - 48; ++i) {
        ArenaCtx *c = arenaCtx_[i];
        if (!c || !c->ready) continue;
        UInt32 how = 0;
        const UInt16 tag = c->map.ownerAt(va, &how);
        if (!tag) continue;
        const UInt64 bytes = tag <= kMaxMem ? mem_[tag - 1].bytes : 0;
        const UInt32 dom = tag <= kMaxMem ? mem_[tag - 1].domain : 0;
        n += snprintf(s + n, sizeof(s) - n, " [slot %u pid %d obj %u dom %u 0x%llx %s]", i, c->pid, tag,
                      dom, bytes, how == 3 ? "4K" : how == 2 ? "64K" : "2M");
        any = true;
    }
    if (!any) n += snprintf(s + n, sizeof(s) - n, " mapped by no client");
    // 0.164.0: every arena as it stands (after a reset a client can hold a
    // context that is not ready, or one without its staging page)
    for (UInt32 i = 0; i < kMaxArenaCtx && n > 0 && n < static_cast<int>(sizeof(s)) - 40; ++i) {
        ArenaCtx *c = arenaCtx_[i];
        if (!c) continue;
        UInt32 how = 0;
        const bool stg = c->ready && c->map.ownerAt(0x2800000000ULL, &how) != 0;
        n += snprintf(s + n, sizeof(s) - n, " {%u pid %d %s%s}", i, c->pid, c->ready ? "ready" : "stale",
                      stg ? " stg" : "");
    }
    setProperty("NVGspControl-fault-owner", s);
    IOLog("NVGspControl: MMU fault %s\n", s);
}

// Free one arena context: LPT chunks, owner arrays, the struct.
void NVGspControl::arenaFreeCtxLocked(UInt32 slot) {
    ArenaCtx *c = arenaCtx_[slot];
    if (!c) return;
    ArenaBackend b{this, c};
    arenaFreeChunksLocked(c);
    c->map.release(b);
    IOFree(c, sizeof(ArenaCtx));
    arenaCtx_[slot] = nullptr;
}

// Drop `owner`'s arena (client closed). Caller holds lock_ and has already
// unbound the client's objects.
void NVGspControl::arenaReleaseLocked(const void *owner) {
    for (UInt32 i = 0; i < kMaxArenaCtx; ++i) {
        ArenaCtx *c = arenaCtx_[i];
        if (!c || c->owner != owner) continue;
        IOLog("NVGspControl: arena slot %u (pid %d) released, owner %p\n", i, c->pid, owner);
        if (arenaActive_ == c) {
            // 0.155.0: memFreeAll waited for this client's work before
            // taking the lock (no drain with lock_ held here any more)
            // its in-flight work (if any) loses the arena, never the reverse
            if (arenaWritePd1Locked(nullptr)) tlbInvalidate();
            arenaActive_ = nullptr;
        }
        arenaFreeCtxLocked(i);
    }
}

// VAS torn down (stop: free everything; reset: keep the per-client
// contexts but mark their tables for re-initialisation on next use).
void NVGspControl::arenaDropAllLocked(bool keepContexts) {
    IOLog("NVGspControl: arenas dropped (keep %d) gen %u\n", keepContexts ? 1 : 0, resetGeneration());
    for (UInt32 i = 0; i < kMaxArenaCtx; ++i) {
        ArenaCtx *c = arenaCtx_[i];
        if (!c) continue;
        if (keepContexts) c->ready = false;
        else arenaFreeCtxLocked(i);
    }
    arenaActive_ = nullptr;
    arenaInstalled_ = false;
}

// Flush the TLB when `c` is the arena the GPU currently walks.
bool NVGspControl::arenaFlushIfLiveLocked(const ArenaCtx *c) {
    // 0.178.4: an owner with its own GR channel has these tables live in its
    // private VAS at all times
    const bool own = c && clientChannelLocked(c->owner) &&
                     clientChannelLocked(c->owner)->arenaTables == c->map.tables;
    if (!c || (c != arenaActive_ && !own)) return true;
    return tlbInvalidate() == kIOReturnSuccess;
}

// PTE flag bits from the user-client flags: bits 7:0 kind, bit 8 system
// memory (coherent aperture + VOL). 0.163.0: bit 9 with bit 8 = system
// memory the GPU L2 may cache (coherent aperture, no VOL), for Metal Shared
// buffers: VOL made every GPU read a PCIe round trip (2 GB/s against 227 for
// VRAM, 2.5 s per Vision convolution). submitSegments keeps it coherent.
static UInt64 arenaPteFlags(UInt32 flags) {
    const UInt64 sys = (flags & 0x100) ? ((flags & 0x200) ? 0x4 : 0xC) : 0;
    return (UInt64(flags & 0xff) << 56) | sys;
}

// 0.105.0: bind (bytes > 0) or unbind (bytes == 0: one 2 MiB page at va) in
// the caller's arena. flags: bits 7:0 PTE kind, bit 8 = sysmem-coherent
// aperture (+VOL) instead of VRAM. 0.114.0: va, phys, bytes 64 KiB aligned.
IOReturn NVGspControl::vaBind(const void *owner, UInt64 va, UInt64 phys, UInt64 bytes,
                              UInt32 flags) {
    if (!lock_ || !pci_ || !pdbAddress_ || !scratchOffset_) return kIOReturnNotReady;
    lk(__LINE__);
    waitThawLocked();                                // 0.127.0
    ArenaCtx *c = arenaForLocked(owner, true);
    bool ok = false;
    if (c) {
        ArenaBackend b{this, c};
        ok = bytes ? c->map.bind(b, va, bytes, phys, arenaPteFlags(flags),
                                 nvgsp::kArenaRawOwner)
                   : c->map.unbind(b, va, nvgsp::kArenaPageBytes);
        ok = ok && arenaFlushIfLiveLocked(c);
    }
    ulk();
    return c ? (ok ? kIOReturnSuccess : kIOReturnBadArgument) : kIOReturnNoResources;
}

// 0.109.0: invalidate every arena PTE that still points at object `handle`,
// then flush the GPU TLB — all before the caller releases the pages, so no
// cached translation can reach memory the kernel may already have reused.
// Caller holds lock_.
bool NVGspControl::unbindObjectLocked(UInt32 handle) {
    if (!handle || handle > kMaxMem) return false;
    ArenaCtx *c = arenaForLocked(mem_[handle - 1].owner, false);
    if (!c || !c->ready) return true;                  // never bound
    ArenaBackend b{this, c};
    bool any = false;
    bool ok = c->map.unbindOwner(b, static_cast<UInt16>(handle), &any);
    if (any) ok = arenaFlushIfLiveLocked(c) && ok;
    return ok;
}

// 0.109.0: unbind a range of the caller's arena (0.114.0: 64 KiB aligned).
IOReturn NVGspControl::vaUnbind(const void *owner, UInt64 va, UInt64 bytes) {
    if (!lock_ || !pci_ || !pdbAddress_ || !scratchOffset_) return kIOReturnNotReady;
    lk(__LINE__);
    waitThawLocked();                                // 0.127.0
    ArenaCtx *c = arenaForLocked(owner, false);
    bool ok = true;
    if (c && c->ready) {
        ArenaBackend b{this, c};
        ok = c->map.unbind(b, va, bytes) && arenaFlushIfLiveLocked(c);
    }
    ulk();
    return ok ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn NVGspControl::memInfo(UInt64 *heapBytesOut, UInt64 *vramUsedOut, UInt64 *sysUsedOut) {
    constexpr UInt64 kHeapBase = 0x40000000ULL;
    if (!lock_ || !heapBytesOut || !vramUsedOut || !sysUsedOut) return kIOReturnBadArgument;
    lk(__LINE__);
    const UInt64 end = ((fbFreeLimit_ + 1) & ~0x1fffffULL) - (64ULL << 20);
    UInt64 vram = 0, sys = 0;
    for (UInt32 i = 0; i < kMaxMem; ++i) {
        if (!mem_[i].owner) continue;
        if (mem_[i].domain == 0) vram += mem_[i].bytes; else sys += mem_[i].bytes;
    }
    ulk();
    *heapBytesOut = fbFreeLimit_ && end > kHeapBase ? end - kHeapBase : 0;
    *vramUsedOut = vram;
    *sysUsedOut = sys;
    return kIOReturnSuccess;
}

// 0.107.0 memory objects -------------------------------------------------
IOReturn NVGspControl::memAlloc(const void *owner, UInt64 bytes, UInt32 domain,
                                UInt32 *handleOut, UInt64 *physOut) {
    if (!lock_ || !owner || !bytes || domain > 2 || !handleOut || !physOut)
        return kIOReturnBadArgument;
    // 0.147.0: system memory chunks (physically contiguous 2 MiB each, then
    // zeroed) are allocated before taking lock_: done inside it, one
    // allocation held the lock for up to 46 ms and delayed vblank and fence
    // interrupts (desktop hitches under load).
    if (!kce_.ready) kceEnsure();                    // 0.149.0
    IOBufferMemoryDescriptor **pre = nullptr;
    if (domain == 1) {
        const UInt64 aligned = (bytes + 0x1fffff) & ~0x1fffffULL;
        const UInt32 n = static_cast<UInt32>(aligned >> 21);
        pre = static_cast<IOBufferMemoryDescriptor **>(IOMalloc(n * sizeof(void *)));
        if (!pre) return kIOReturnNoMemory;
        bzero(pre, n * sizeof(void *));
        for (UInt32 i = 0; i < n; ++i) {
            IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous |
                kIOMemoryKernelUserShared, 0x200000, 0x000000FFFFE00000ULL);
            if (b && b->prepare() != kIOReturnSuccess) { b->release(); b = nullptr; }
            if (!b) {
                for (UInt32 k = 0; k < i; ++k) { pre[k]->complete(); pre[k]->release(); }
                IOFree(pre, n * sizeof(void *));
                return kIOReturnNoMemory;
            }
            bzero(b->getBytesNoCopy(), 0x200000);
            pre[i] = b;
        }
    }
    lk(__LINE__);
    waitThawLocked();                                // 0.127.0
    IOReturn ret = memAllocLocked(owner, bytes, domain, handleOut, physOut, pre);
    // 0.149.0: heap full -> move idle client objects to system memory until
    // this one fits (or nothing is left to move)
    for (UInt32 tries = 0; ret == kIOReturnNoMemory && domain == 0 && tries < 64; ++tries) {
        if (!evictOneLocked(owner)) break;
        ret = memAllocLocked(owner, bytes, domain, handleOut, physOut, nullptr);
    }
    if (ret == kIOReturnSuccess) {
        mem_[*handleOut - 1].client = true;
        mem_[*handleOut - 1].stamp = ++useClock_;
    }
    // 0.147.1: VRAM objects are zeroed before the owner sees the handle; a
    // new object used to show whatever the previous owner (any process)
    // left there. SYS chunks are zeroed when they are allocated.
    if (ret == kIOReturnSuccess && (domain == 0 || domain == 2)) {
        const UInt64 objBytes = mem_[*handleOut - 1].bytes;
        const UInt64 phys = mem_[*handleOut - 1].phys;
        ulk();
        bool zeroed = zeroVram(phys, objBytes);
        lk(__LINE__);
        // 0.149.0: no BAR1 view (ReBAR off) -> CE fill on the kernel channel.
        // Tahoe (29 Sep) runs without ReBAR for now, and that kernel CE fill
        // is exactly what 0.147.3 warned about: it lands on the ring the
        // clients' execSegments use, CE0 raises RC 39 on chid 8 and lock_
        // stays held up to 6.6 s (vblank and fence interrupts wait behind
        // it). So it is opt-in now (boot-arg nvgsp-cezero=1); without ReBAR
        // a new VRAM object is simply not zeroed.
        UInt32 ceZero = 0;
        if (!zeroed && PE_parse_boot_argn("nvgsp-cezero", &ceZero, sizeof(ceZero)) && ceZero) {
            Ring ring{};
            UInt32 words[64];
            const UInt32 n = nvgsp::buildPhysFill(phys, objBytes, 0, words, 64);
            UInt64 ns = 0;
            zeroed = n && kceRing(&ring) && submitRing(ring, words, n, &ns) == kIOReturnSuccess;
        }
        if (!zeroed) ++vramZeroMisses_;
        ++vramZeroed_;
        setProperty("NVGspControl-vram-zeroed", vramZeroed_, 32);
        setProperty("NVGspControl-vram-zero-misses", vramZeroMisses_, 32);
    }
    ulk();
    return ret;
}

bool NVGspControl::zeroVram(UInt64 phys, UInt64 bytes) {
    // 0.147.3: through BAR1 by the CPU. With ReBAR, BAR1 maps all of VRAM
    // 1:1 (the cpu-visible objects rely on the same thing), so no GPU channel
    // is involved. 0.147.1/0.147.2 used the CE ring from the kernel, and that
    // pushbuffer collides with the clients' execSegments on the same ring
    // (Xid 32, invalid pushbuffer); S3 never saw that because no client runs.
    // Write-combined, ~9 GB/s, with lock_ not held.
    IODeviceMemory *bar1 = bar1Dev();
    if (!bar1 || !bytes || phys + bytes > bar1->getLength()) return false;
    IOMemoryMap *map = bar1->createMappingInTask(kernel_task, 0,
                                                 kIOMapAnywhere | kIOMapWriteCombineCache,
                                                 phys, bytes);
    if (!map) return false;
    bzero(reinterpret_cast<void *>(static_cast<uintptr_t>(map->getVirtualAddress())), bytes);
    OSSynchronizeIO();
    map->release();
    return true;
}

// 0.111.0: body of memAlloc for callers that already hold lock_.
IOReturn NVGspControl::memAllocLocked(const void *owner, UInt64 bytes, UInt32 domain,
                                      UInt32 *handleOut, UInt64 *physOut,
                                      IOBufferMemoryDescriptor **pre) {
    constexpr UInt64 kHeapBase = 0x40000000ULL;   // 1 GiB (below: kext carve-outs)
    bytes = (bytes + 0x1fffff) & ~0x1fffffULL;
    // `pre` (SYS chunks the caller allocated unlocked) is always consumed
    auto dropPre = [&]() {
        if (!pre) return;
        for (UInt32 k = 0; k < (bytes >> 21); ++k)
            if (pre[k]) { pre[k]->complete(); pre[k]->release(); }
        IOFree(pre, (bytes >> 21) * sizeof(void *));
        pre = nullptr;
    };
    UInt32 h = 0;
    while (h < kMaxMem && mem_[h].owner) ++h;
    if (h == kMaxMem) { dropPre(); return kIOReturnNoResources; }
    GpuMem m{};
    // 0.123.0: domain 2 = VRAM inside the BAR1 window (CPU-mappable)
    const bool cpu = domain == 2;
    if (cpu) {
        if (!vramCpuWindowEnd()) { dropPre(); return kIOReturnUnsupported; }
        domain = 0;
    }
    m.owner = owner; m.bytes = bytes; m.domain = domain; m.cpu = cpu;
    IOReturn ret = kIOReturnNoMemory;
    if (domain == 0) {
        // 0.126.0: first fit over [start, end) from the sorted range index
        // (was an O(n^2) rescan of every memory object)
        auto fit = [&](UInt64 start, UInt64 end) -> bool {
            return vramHeap_.fit(start, end, bytes, &m.phys);
        };
        const UInt64 heapEnd = ((fbFreeLimit_ + 1) & ~0x1fffffULL) - (64ULL << 20);
        const UInt64 window = vramCpuWindowEnd();
        bool placed;
        if (cpu) {
            placed = fit(kHeapBase, window);
        } else {
            // 0.123.0: plain VRAM goes above the CPU-visible window first so the
            // window stays free for objects that need CPU access
            placed = (window && fit(window, heapEnd)) || fit(kHeapBase, heapEnd);
        }
        if (placed && cpu) {
            IODeviceMemory *bar1 = bar1Dev();
            m.user = bar1 ? IOSubMemoryDescriptor::withSubRange(bar1, m.phys, bytes,
                                                                kIODirectionInOut)
                          : nullptr;
            placed = m.user != nullptr;
        }
        if (placed) ret = kIOReturnSuccess;
    } else {
        const UInt32 n = static_cast<UInt32>(bytes >> 21);
        m.chunk = pre ? pre : static_cast<IOBufferMemoryDescriptor **>(IOMalloc(n * sizeof(void *)));
        const bool had = pre != nullptr;
        pre = nullptr;   // owned by m now
        if (m.chunk) {
            if (!had) bzero(m.chunk, n * sizeof(void *));
            UInt32 i = had ? n : 0;
            for (; i < n; ++i) {
                IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                    kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous |
                    kIOMemoryKernelUserShared, 0x200000, 0x000000FFFFE00000ULL);
                if (!b) break;
                if (b->prepare() != kIOReturnSuccess) { b->release(); break; }
                bzero(b->getBytesNoCopy(), 0x200000);
                m.chunk[i] = b;
            }
            m.chunks = i;
            if (i == n) {
                m.user = IOMultiMemoryDescriptor::withDescriptors(
                    reinterpret_cast<IOMemoryDescriptor **>(m.chunk), n, kIODirectionInOut, false);
                ret = m.user ? kIOReturnSuccess : kIOReturnNoMemory;
            }
            if (ret != kIOReturnSuccess) {
                for (UInt32 k = 0; k < i; ++k) { m.chunk[k]->complete(); m.chunk[k]->release(); }
                IOFree(m.chunk, n * sizeof(void *));
            }
        }
    }
    if (ret == kIOReturnSuccess && domain == 0 && !vramHeap_.insert(m.phys, bytes)) {
        if (m.user) m.user->release();
        ret = kIOReturnNoResources;
    }
    if (ret == kIOReturnSuccess) {
        mem_[h] = m;
        *handleOut = h + 1;
        *physOut = m.phys;
    }
    return ret;
}

// 0.127.0: the S3 copy of a VRAM object (see saveVramLocked).
static void freeSavedChunks(NVGspControl::GpuMem &m) {
    if (!m.saved) return;
    for (UInt32 k = 0; k < m.savedChunks; ++k) { m.saved[k]->complete(); m.saved[k]->release(); }
    IOFree(m.saved, nvgsp::evictChunks(m.bytes) * sizeof(void *));
    m.saved = nullptr;
    m.savedChunks = 0;
}

static void releaseGpuMem(NVGspControl::GpuMem &m) {
    freeSavedChunks(m);
    if (m.user && m.domain == 3) m.user->complete();   // 0.134.0: unwire user pages
    if (m.user) m.user->release();
    for (UInt32 k = 0; k < m.chunks; ++k) { m.chunk[k]->complete(); m.chunk[k]->release(); }
    if (m.chunk) IOFree(m.chunk, (m.bytes >> 21) * sizeof(void *));
    bzero(&m, sizeof(m));
}

// 0.127.0: S3 save/restore of the kext-heap VRAM objects ----------------
// GSP-RM's FBSR restores only RM's own VRAM. Before srSuspend every live
// VRAM object (user heap, arena tables, video chunks) is copied by the CE
// channel into 2 MiB contiguous sysmem chunks (physical addressing, no VA),
// and copied back after a successful srResume. Meanwhile memory and submit
// calls wait (vramFrozen_), so nothing changes between the two copies.
// 0.128.0: GPU VAs of the GR / CE fence semaphores (32-bit sequence), for
// NVK's GPU-side cross-engine waits (host SEM_EXECUTE ACQ_CIRC_GEQ). Both
// rings live in the one kext VAS every client's arena is switched into.
void NVGspControl::publishSemVa() {
    Ring r{};
    setProperty("NVGspControl-gr-sem-va", grRing(&r) ? r.semVa : 0, 64);
    setProperty("NVGspControl-ce-sem-va", ceRing(&r) ? r.semVa : 0, 64);
}

void NVGspControl::waitThawLocked() {
    while (vramFrozen_) sleepLk(&vramFrozen_, 0, THREAD_UNINT);
}

void NVGspControl::thawLocked() {
    vramFrozen_ = false;
    IOLockWakeup(lock_, &vramFrozen_, false);
}

// 0.154.0 (native N4): the display pipe's flip as physical copy-engine
// lines (surface pages in system RAM -> scan-out in VRAM) on the privileged
// kernel copy channel, instead of a CPU memcpy of the whole frame. GR work
// queued so far (the composite that drew the surface) finishes first.
IOReturn NVGspControl::flipCopy(NVGspFlipCopy *f) {
    if (!f || f->version != 1 || !f->src || !f->rows || !f->widthBytes || f->widthBytes > f->srcRowBytes ||
        f->widthBytes > f->dstRowBytes)
        return kIOReturnBadArgument;
    IODeviceMemory *b1 = bar1Dev();
    if (!b1 || !lock_) return kIOReturnNotReady;
    const UInt64 base = b1->getPhysicalAddress();
    const UInt64 span = UInt64(f->rows - 1) * f->dstRowBytes + f->widthBytes;
    if (f->dstBus < base || f->dstBus + span > base + b1->getLength()) return kIOReturnBadArgument;
    const UInt64 dstVram = f->dstBus - base;
    constexpr UInt32 kMax = (nvgsp::kVideoPbBytes / 4 - 64) / nvgsp::kCeCopyWords;
    auto *batch = static_cast<nvgsp::EvictCopy *>(IOMalloc(kMax * sizeof(nvgsp::EvictCopy)));
    if (!batch) return kIOReturnNoMemory;
    lk(__LINE__);
    Ring gr{};
    bool ok = !grRing(&gr) || waitRingSem(gr, *gr.seq, 200000);
    UInt32 n = 0, lines = 0;
    // one line per (row x physically contiguous run); a whole-frame run when
    // both sides are tightly packed with the same pitch
    const bool linear = f->widthBytes == f->srcRowBytes && f->srcRowBytes == f->dstRowBytes;
    const UInt32 rows = linear ? 1 : f->rows;
    const UInt64 rowLen = linear ? UInt64(f->rows) * f->srcRowBytes : f->widthBytes;
    for (UInt32 y = 0; ok && y < rows; ++y) {
        UInt64 done = 0;
        while (ok && done < rowLen) {
            const UInt64 so = f->srcOffset + UInt64(y) * f->srcRowBytes + done;
            IOByteCount segLen = 0;
            const UInt64 phys = f->src->getPhysicalSegment(so, &segLen, kIOMemoryMapperNone);
            if (!phys || !segLen) { ok = false; break; }
            UInt64 len = rowLen - done;
            if (len > segLen) len = segLen;
            if (len > 0xfffff000ULL) len = 0xfffff000ULL;
            batch[n++] = nvgsp::EvictCopy{phys, dstVram + UInt64(y) * f->dstRowBytes + done, true, false,
                                          static_cast<UInt32>(len)};
            done += len; ++lines;
            if (n == kMax) { ok = ceCopyLinesLocked(batch, n); n = 0; }
        }
    }
    if (ok && n) ok = ceCopyLinesLocked(batch, n);
    ulk();
    IOFree(batch, kMax * sizeof(nvgsp::EvictCopy));
    f->copiesOut = lines;
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

// any number of physical lines that fit the kernel copy channel's pushbuffer
bool NVGspControl::ceCopyLinesLocked(const nvgsp::EvictCopy *c, UInt32 n) {
    Ring ring{};
    if (!n || !kceRing(&ring)) return false;
    const UInt32 max = 2 + n * nvgsp::kCeCopyWords;
    if (max * 4 + 64 > ring.pbBytes) return false;
    UInt32 *words = static_cast<UInt32 *>(IOMalloc(max * 4));
    if (!words) return false;
    const UInt32 count = nvgsp::buildEvictBatch(c, n, words, max);
    UInt64 ns = 0;
    const bool ok = count && submitRing(ring, words, count, &ns) == kIOReturnSuccess;
    IOFree(words, max * 4);
    return ok;
}

bool NVGspControl::ceCopyBatchLocked(const nvgsp::EvictCopy *c, UInt32 n) {
    // 0.149.0: physical copies only on the privileged kernel channel
    Ring ring{};
    if (!kceRing(&ring)) return false;
    constexpr UInt32 kMax = 2 + nvgsp::kEvictBatchCopies * nvgsp::kCeCopyWords;
    UInt32 *words = static_cast<UInt32 *>(IOMalloc(kMax * 4));   // off the kernel stack
    if (!words) return false;
    const UInt32 count = nvgsp::buildEvictBatch(c, n, words, kMax);
    UInt64 ns = 0;
    const bool ok = count && submitRing(ring, words, count, &ns) == kIOReturnSuccess;
    IOFree(words, kMax * 4);
    return ok;
}

void NVGspControl::freeSavedVramLocked() {
    for (UInt32 i = 0; i < kMaxMem; ++i) freeSavedChunks(mem_[i]);
    vramSaved_ = false;
}

bool NVGspControl::saveVramLocked(bool needGr, bool clientOnly) {
    constexpr UInt64 kSaveCap = 4ULL << 30;   // wired sysmem taken at sleep
    freeSavedVramLocked();
    // 0.177.1: a reset keeps client objects only. The kext's own (arena page
    // tables, the GR context pool, channel chunks) are rebuilt by the reset;
    // putting the old ones back reloaded the faulted GR context after an RC
    // reset and every following reset faulted again (type 13, 1 Oct 21:16).
    auto take = [&](const GpuMem &m) { return m.owner && m.domain == 0 && (!clientOnly || m.client); };
    UInt64 total = 0;
    for (UInt32 i = 0; i < kMaxMem; ++i)
        if (take(mem_[i])) total += mem_[i].bytes;
    vramSavedBytes_ = total;
    if (!total) return vramSaved_ = true;
    if (total > kSaveCap) return false;
    // 0.176.0: before a reset GR may be the engine that died (RC); its unfinished
    // writes are lost either way, so only the copy engine has to be idle
    if (!drainEnginesLocked() && needGr) return false;
    if (!needGr && cePersistent_ && ceChunk_ && ceSeq_) {
        Ring ce{};
        if (!ceRing(&ce) || !waitRingSem(ce, ceSeq_, 2000000)) return false;
        ceOutstanding_ = 0;
    }
    auto *batch = static_cast<nvgsp::EvictCopy *>(
        IOMalloc(nvgsp::kEvictBatchCopies * sizeof(nvgsp::EvictCopy)));
    bool ok = batch != nullptr;
    UInt32 n = 0;
    for (UInt32 i = 0; ok && i < kMaxMem; ++i) {
        GpuMem &m = mem_[i];
        if (!take(m)) continue;
        const UInt32 chunks = nvgsp::evictChunks(m.bytes);
        m.saved = static_cast<IOBufferMemoryDescriptor **>(IOMalloc(chunks * sizeof(void *)));
        if (!m.saved) { ok = false; break; }
        bzero(m.saved, chunks * sizeof(void *));
        for (UInt32 k = 0; ok && k < chunks; ++k) {
            IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                nvgsp::kEvictChunkBytes, 0x000000FFFFE00000ULL);
            if (!b || b->prepare() != kIOReturnSuccess) {
                if (b) b->release();
                ok = false;
                break;
            }
            m.saved[k] = b;
            m.savedChunks = k + 1;
            IOByteCount len = 0;
            const UInt64 phys = b->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
            const UInt64 off = UInt64(k) * nvgsp::kEvictChunkBytes;
            const UInt64 left = m.bytes - off;
            ok = phys && len >= nvgsp::kEvictChunkBytes;
            batch[n++] = nvgsp::EvictCopy{m.phys + off, phys, false, true,
                                          static_cast<UInt32>(left < nvgsp::kEvictChunkBytes
                                                                  ? left : nvgsp::kEvictChunkBytes)};
            if (ok && n == nvgsp::kEvictBatchCopies) { ok = ceCopyBatchLocked(batch, n); n = 0; }
        }
    }
    if (ok && n) ok = ceCopyBatchLocked(batch, n);
    if (batch) IOFree(batch, nvgsp::kEvictBatchCopies * sizeof(nvgsp::EvictCopy));
    if (!ok) freeSavedVramLocked();
    vramSaved_ = ok;
    return ok;
}

bool NVGspControl::restoreVramLocked(UInt32 *skipped) {
    if (!vramSaved_) return false;
    auto *batch = static_cast<nvgsp::EvictCopy *>(
        IOMalloc(nvgsp::kEvictBatchCopies * sizeof(nvgsp::EvictCopy)));
    bool ok = batch != nullptr;
    UInt32 n = 0;
    for (UInt32 i = 0; ok && i < kMaxMem; ++i) {
        const GpuMem &m = mem_[i];
        if (!m.owner || m.domain != 0) continue;
        const UInt32 chunks = nvgsp::evictChunks(m.bytes);
        if (!m.saved || m.savedChunks != chunks) {   // no copy
            // 0.176.2: after a reset, objects allocated while the GPU came back have
            // none and need none; S3 (skipped == nullptr) stays all-or-nothing
            if (skipped) { ++*skipped; continue; }
            ok = false;
            break;
        }
        for (UInt32 k = 0; ok && k < chunks; ++k) {
            IOByteCount len = 0;
            const UInt64 phys = m.saved[k]->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
            const UInt64 off = UInt64(k) * nvgsp::kEvictChunkBytes;
            const UInt64 left = m.bytes - off;
            ok = phys != 0;
            batch[n++] = nvgsp::EvictCopy{phys, m.phys + off, true, false,
                                          static_cast<UInt32>(left < nvgsp::kEvictChunkBytes
                                                                  ? left : nvgsp::kEvictChunkBytes)};
            if (ok && n == nvgsp::kEvictBatchCopies) { ok = ceCopyBatchLocked(batch, n); n = 0; }
        }
    }
    if (ok && n) ok = ceCopyBatchLocked(batch, n);
    if (batch) IOFree(batch, nvgsp::kEvictBatchCopies * sizeof(nvgsp::EvictCopy));
    return ok;
}

// 0.149.0: VRAM oversubscription ------------------------------------------
// There is no per-submit buffer list, so "recently used" is tracked per
// client (its last submission) and per object (its last alloc/bind). The
// victim is the plain VRAM object of the least recently active other client;
// the requester's own objects go last. A moved object stays in system memory
// until it is freed: its arena PTEs point at coherent sysmem from then on.
void NVGspControl::noteUseLocked(const void *owner) {
    UInt32 slot = kOwnerUse, oldest = 0;
    for (UInt32 i = 0; i < kOwnerUse; ++i) {
        if (ownerUse_[i].owner == owner) { slot = i; break; }
        if (ownerUse_[i].stamp < ownerUse_[oldest].stamp) oldest = i;
    }
    if (slot == kOwnerUse) { slot = oldest; ownerUse_[slot].owner = owner; }
    ownerUse_[slot].stamp = ++useClock_;
}

UInt64 NVGspControl::ownerStampLocked(const void *owner) const {
    for (UInt32 i = 0; i < kOwnerUse; ++i)
        if (ownerUse_[i].owner == owner) return ownerUse_[i].stamp;
    return 0;
}

bool NVGspControl::evictOneLocked(const void *requester) {
    UInt32 best = kMaxMem;
    UInt64 bestOwner = 0;
    bool bestMine = true;
    for (UInt32 i = 0; i < kMaxMem; ++i) {
        const GpuMem &m = mem_[i];
        if (!m.owner || !m.client || m.domain != 0 || m.cpu || m.presented || m.saved) continue;
        if (presentHandle_ == i + 1) continue;
        const bool mine = m.owner == requester;
        const UInt64 os = ownerStampLocked(m.owner);
        if (best != kMaxMem) {
            if (mine != bestMine) { if (mine) continue; }
            else if (os != bestOwner ? os > bestOwner : m.stamp >= mem_[best].stamp) continue;
        }
        best = i; bestOwner = os; bestMine = mine;
    }
    if (best == kMaxMem) return false;
    if (evictObjectLocked(best)) return true;
    ++evictFails_;
    setProperty("NVGspControl-vram-evict-fails", evictFails_, 32);
    mem_[best].client = false;   // never pick it again
    return true;                 // let the caller try the next one
}

bool NVGspControl::evictObjectLocked(UInt32 index) {
    GpuMem &m = mem_[index];
    const UInt32 n = static_cast<UInt32>(m.bytes >> 21);
    UInt64 t0 = 0, t1 = 0;
    clock_get_uptime(&t0);
    if (!n || !drainEnginesLocked()) return false;
    auto **chunk = static_cast<IOBufferMemoryDescriptor **>(IOMalloc(n * sizeof(void *)));
    auto *phys = static_cast<uint64_t *>(IOMalloc(n * sizeof(uint64_t)));
    auto *batch = static_cast<nvgsp::EvictCopy *>(
        IOMalloc(nvgsp::kEvictBatchCopies * sizeof(nvgsp::EvictCopy)));
    UInt32 have = 0;
    bool ok = chunk && phys && batch;
    for (; ok && have < n; ++have) {
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous |
            kIOMemoryKernelUserShared, 0x200000, 0x000000FFFFE00000ULL);
        if (b && b->prepare() != kIOReturnSuccess) { b->release(); b = nullptr; }
        if (!b) { ok = false; break; }
        IOByteCount len = 0;
        chunk[have] = b;
        phys[have] = b->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
        ok = phys[have] && len >= 0x200000;
    }
    // everything is drained and lock_ is held, so the CE ring is ours alone
    UInt32 c = 0;
    for (UInt32 k = 0; ok && k < n; ++k) {
        batch[c++] = nvgsp::EvictCopy{m.phys + UInt64(k) * 0x200000, phys[k], false, true, 0x200000};
        if (c == nvgsp::kEvictBatchCopies || k + 1 == n) { ok = ceCopyBatchLocked(batch, c); c = 0; }
    }
    // point the owner's arena PTEs at the copy
    ArenaCtx *ctx = ok ? arenaForLocked(m.owner, false) : nullptr;
    nvgsp::ArenaRemapEntry *journal = nullptr;
    UInt32 entries = 0;
    nvgsp::ArenaRemapResult res = nvgsp::ArenaRemapResult::Success;
    if (ok && ctx && ctx->ready) {
        ArenaBackend b{this, ctx};
        const nvgsp::ResidencyTranslation tr{{m.phys, m.bytes, nullptr, 0}, {0, m.bytes, phys, n}};
        const UInt16 tag = static_cast<UInt16>(index + 1);
        ok = tr.valid() && ctx->map.planOwnerRemap(b, tag, tr, 0xC, nullptr, 0, &entries);
        if (ok && entries) {
            journal = static_cast<nvgsp::ArenaRemapEntry *>(IOMalloc(entries * sizeof(*journal)));
            UInt32 again = 0;
            ok = journal && ctx->map.planOwnerRemap(b, tag, tr, 0xC, journal, entries, &again) &&
                 again == entries;
            if (ok) res = nvgsp::applyArenaRemap(b, journal, entries);
            if (ok && res != nvgsp::ArenaRemapResult::Success) ok = false;
            // even a rolled-back attempt may have been seen by the MMU
            if (tlbInvalidate() != kIOReturnSuccess) ok = false;
        }
    }
    if (journal) IOFree(journal, entries * sizeof(*journal));
    if (batch) IOFree(batch, nvgsp::kEvictBatchCopies * sizeof(nvgsp::EvictCopy));
    if (phys) IOFree(phys, n * sizeof(uint64_t));
    if (!ok) {
        if (res == nvgsp::ArenaRemapResult::RollbackFailed) {
            // PTEs may point at either copy: keep both alive, leak the chunks
            IOLog("NVGspControl: evict of object %u could not roll back, sysmem kept\n", index + 1);
        } else {
            for (UInt32 k = 0; k < have && chunk; ++k) { chunk[k]->complete(); chunk[k]->release(); }
        }
        if (chunk) IOFree(chunk, n * sizeof(void *));
        return false;
    }
    vramHeap_.remove(m.phys);
    m.domain = 1;
    m.phys = 0;
    m.chunk = chunk;
    m.chunks = n;
    clock_get_uptime(&t1);
    UInt64 ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    ++evicted_;
    evictedBytes_ += m.bytes;
    evictNs_ += ns;
    setProperty("NVGspControl-vram-evicted", evicted_, 32);
    setProperty("NVGspControl-vram-evicted-mib", evictedBytes_ >> 20, 64);
    setProperty("NVGspControl-vram-evict-ms", evictNs_ / 1000000, 64);
    return true;
}

void NVGspControl::releaseGpuMemLocked(UInt32 index) {
    if (index >= kMaxMem || !mem_[index].owner) return;
    if (mem_[index].domain == 0) vramHeap_.remove(mem_[index].phys);   // 0.126.0
    releaseGpuMem(mem_[index]);
}

// 0.177.0: live clients by task, for memAdopt
void NVGspControl::clientLive(const void *owner, task_t task, bool live) {
    if (!lock_ || !owner) return;
    lk(__LINE__);
    for (UInt32 i = 0; i < 256; ++i) {
        if (live && !liveClients_[i].owner) { liveClients_[i] = LiveClient{owner, task}; break; }
        if (!live && liveClients_[i].owner == owner) { liveClients_[i] = LiveClient{}; break; }
    }
    ulk();
}

// 0.177.0: after a reset NVMTLDriver reopens and used to allocate new VRAM for
// every object, then closed the old client, which freed the objects gpuReset
// had just restored (WindowServer's layers came back black). The new client
// takes the object over instead: same pages, contents kept. Only from a live
// client of the same task, VRAM objects only.
IOReturn NVGspControl::memAdopt(const void *owner, task_t task, UInt32 handle, UInt64 *physOut) {
    if (!lock_ || !owner || !task || !handle || handle > kMaxMem || !physOut) return kIOReturnBadArgument;
    lk(__LINE__);
    waitThawLocked();
    GpuMem &m = mem_[handle - 1];
    bool ok = m.owner && m.owner != owner && m.domain == 0;
    bool sameTask = false;
    for (UInt32 i = 0; ok && i < 256 && !sameTask; ++i)
        sameTask = liveClients_[i].owner == m.owner && liveClients_[i].task == task;
    ok = ok && sameTask;
    if (ok) {
        m.owner = owner;
        m.stamp = ++useClock_;
        *physOut = m.phys;
        ++adopted_;
    }
    ulk();
    if (ok) setProperty("NVGspControl-mem-adopted", adopted_, 32);
    return ok ? kIOReturnSuccess : kIOReturnNotPermitted;
}

IOReturn NVGspControl::memFree(const void *owner, UInt32 handle) {
    if (!lock_ || !handle || handle > kMaxMem) return kIOReturnBadArgument;
    lk(__LINE__);
    waitThawLocked();                                // 0.127.0
    GpuMem &m = mem_[handle - 1];
    const bool mine = m.owner && m.owner == owner;
    bool unbound = true;
    GpuMem dead{};
    if (mine) {
        // 0.110.0: never scan out memory that is about to be released.
        if (presentOwner_ == owner && presentHandle_ == handle) presentStopLocked(owner);
        // 0.109.0: no GPU PTE may outlive the pages it points at.
        unbound = unbindObjectLocked(handle);
        // 0.147.0: the pages are unmapped and the TLB flushed; hand them back
        // to the kernel after dropping lock_ (freeing contiguous chunks and
        // descriptors under it was the other half of the long holds).
        if (m.domain == 0) vramHeap_.remove(m.phys);
        dead = m;
        bzero(&m, sizeof(m));
    }
    ulk();
    if (dead.owner) releaseGpuMem(dead);
    if (!mine) return kIOReturnNotPermitted;
    return unbound ? kIOReturnSuccess : kIOReturnIOError;
}

// 0.134.0: host memory straight into the GPU VA (Metal/Vulkan zero-copy).
// The pages stay wired until memFree / client close, and the PTEs go through
// the arena's small page tables (SYS coherent aperture).
IOReturn NVGspControl::userMemBind(const void *owner, task_t task, UInt64 uaddr, UInt64 bytes,
                                   UInt64 va, UInt32 flags, UInt32 *handleOut) {
    if (!lock_ || !owner || !task || !handleOut || !bytes || ((uaddr | bytes | va) & 0xfff) ||
        bytes > (1ULL << 30))
        return kIOReturnBadArgument;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(uaddr, bytes, kIODirectionInOut, task);
    if (!md) return kIOReturnNoMemory;
    if (md->prepare() != kIOReturnSuccess) { md->release(); return kIOReturnVMError; }
    const UInt32 n = static_cast<UInt32>(bytes >> 12);
    UInt64 *phys = static_cast<UInt64 *>(IOMalloc(n * sizeof(UInt64)));
    bool ok = phys != nullptr;
    for (UInt32 i = 0; ok && i < n; ++i) {
        IOByteCount len = 0;
        phys[i] = md->getPhysicalSegment(UInt64(i) << 12, &len, kIOMemoryMapperNone);
        ok = phys[i] && !(phys[i] & 0xfff);
    }
    if (!ok) {
        if (phys) IOFree(phys, n * sizeof(UInt64));
        md->complete(); md->release();
        return kIOReturnVMError;
    }
    lk(__LINE__);
    waitThawLocked();
    UInt32 h = 0;
    while (h < kMaxMem && mem_[h].owner) ++h;
    ArenaCtx *c = h < kMaxMem ? arenaForLocked(owner, true) : nullptr;
    IOReturn ret = kIOReturnNoResources;
    if (c) {
        GpuMem &m = mem_[h];
        bzero(&m, sizeof(m));
        m.owner = owner; m.bytes = bytes; m.domain = 3; m.user = md;
        md = nullptr;                                   // owned by the slot now
        ArenaBackend b{this, c};
        const UInt16 tag = static_cast<UInt16>(h + 1);
        const bool bound = c->map.bindPages(b, va, n, phys, arenaPteFlags((flags & 0x2ff) | 0x100), tag);
        const bool flushed = arenaFlushIfLiveLocked(c);
        if (bound && flushed) {
            *handleOut = h + 1;
            ret = kIOReturnSuccess;
            if ((flags & 0x200) && !sysCachedUsed_) {             // 0.163.0
                sysCachedUsed_ = true;
                setProperty("NVGspControl-sysmem-cached", true);
            }
        } else {
            unbindObjectLocked(h + 1);
            releaseGpuMemLocked(h);
            ret = bound ? kIOReturnIOError : kIOReturnBadArgument;
        }
    }
    ulk();
    IOFree(phys, n * sizeof(UInt64));
    if (md) { md->complete(); md->release(); }
    return ret;
}

// 0.155.0: wait until the GR and CE rings reach what was submitted up to
// now, WITHOUT holding lock_. 0.153.0 drained with lock_ held while a
// client closed: every other process's submit and fence wait stalled behind
// it for seconds, WindowServer froze at login and the GPU was reset.
void NVGspControl::waitSubmittedUnlocked(UInt32 timeoutUs) {
    UInt32 grTarget = 0, ceTarget = 0;
    bool gr = false, ce = false;
    lk(__LINE__);
    Ring r{};
    if (grRing(&r)) { gr = true; grTarget = *r.seq; }
    if (ceRing(&r)) { ce = true; ceTarget = *r.seq; }
    ulk();
    // 0.166.0: the targets are the shared rings' ends, everyone's work, which
    // under load (WindowServer + Safari's MPS CNN at login, 1 Oct 00:15) ran
    // past the old flat 2 s: the arena went while the closing client's batch
    // still ran, GR MMU fault at its staging, GPU reset. Now the wait goes on
    // while the rings move and gives up after timeoutUs without progress
    // (a real hang is the stall detector's job), at most kCloseWaitMaxUs.
    constexpr UInt32 kCloseWaitMaxUs = 20000000;
    UInt32 lastGr = 0, lastCe = 0, still = 0;
    for (UInt32 waited = 0; (gr || ce) && still < timeoutUs && waited < kCloseWaitMaxUs; waited += 100) {
        lk(__LINE__);
        UInt32 v = 0;
        bool moved = false;
        if (gr && grRing(&r) && readRingSem(r, &v)) {
            if (static_cast<SInt32>(v - grTarget) >= 0) gr = false;
            moved |= v != lastGr; lastGr = v;
        }
        if (ce && ceRing(&r) && readRingSem(r, &v)) {
            if (static_cast<SInt32>(v - ceTarget) >= 0) ce = false;
            moved |= v != lastCe; lastCe = v;
        }
        ulk();
        still = moved ? 0 : still + 100;
        if (gr || ce) IODelay(100);
    }
    if (gr || ce) {
        setProperty("NVGspControl-close-drain-timeout", true);
        lk(__LINE__);
        if (gr && grRing(&r)) captureStallLocked(r, grTarget, 2);
        else if (ce && ceRing(&r)) captureStallLocked(r, ceTarget, 3);
        ulk();
    }
}

void NVGspControl::memFreeAll(const void *owner) {
    if (!lock_ || !owner) return;
    waitSubmittedUnlocked(3000000);                  // 0.155.0: before taking lock_; 0.166.0: 3 s without progress
    lk(__LINE__);
    waitThawLocked();                                // 0.127.0
    presentStopLocked(owner);                        // 0.110.0
    for (UInt32 i = 0; i < kMaxMem; ++i)
        if (mem_[i].owner == owner) {
            unbindObjectLocked(i + 1);               // 0.109.0 (+ TLB flush)
            releaseGpuMemLocked(i);
        }
    arenaReleaseLocked(owner);                       // 0.111.0
    // 0.145.0: its state stays on the channel; a new client at the same
    // address must still see "someone else"
    if (lastGrOwner_ == owner) lastGrOwner_ = reinterpret_cast<const void *>(1);
    ulk();
}

IOReturn NVGspControl::vaBindObject(const void *owner, UInt32 handle, UInt64 va, UInt32 flags,
                                    UInt64 memOffset, UInt64 range) {
    if (!lock_ || !handle || handle > kMaxMem) return kIOReturnBadArgument;
    lk(__LINE__);
    waitThawLocked();                                // 0.127.0
    GpuMem &m = mem_[handle - 1];
    bool ok = m.owner && m.owner == owner && m.domain != 3;  // 0.134.0: user memory binds itself
    if (ok) m.stamp = ++useClock_;                           // 0.149.0
    if (ok && !range) range = m.bytes - memOffset;           // 0.108.0: partial binds
    // 0.114.0: 64 KiB granularity (was 2 MiB)
    ok = ok && range && !((memOffset | range) & 0xffff) && memOffset < m.bytes &&
         range <= m.bytes - memOffset;
    ArenaCtx *c = ok ? arenaForLocked(owner, true) : nullptr;   // 0.111.0: own arena
    if (ok && !c) {
        ulk();
        return kIOReturnNoResources;
    }
    const UInt16 tag = static_cast<UInt16>(handle);
    ArenaBackend b{this, c};
    if (ok && m.domain == 0) {
        ok = c->map.bind(b, va, range, m.phys + memOffset, arenaPteFlags(flags & 0xff), tag);
    } else if (ok) {
        // system memory: physically contiguous per 2 MiB chunk
        for (UInt64 o = 0; ok && o < range;) {
            const UInt64 at = memOffset + o, in = at & 0x1fffff;
            const UInt64 len = (0x200000 - in) < (range - o) ? (0x200000 - in) : (range - o);
            IOByteCount segLen = 0;
            const UInt64 phys = m.chunk[at >> 21]->getPhysicalSegment(0, &segLen,
                                                                       kIOMemoryMapperNone);
            ok = phys && c->map.bind(b, va + o, len, phys + in,
                                     arenaPteFlags((flags & 0xff) | 0x100), tag);
            o += len;
        }
    }
    // Pages written before a failure stay owned by the object, so memFree
    // still clears them.
    const bool flushed = arenaFlushIfLiveLocked(c);
    if (va == 0x2800000000ULL) {                     // 0.164.0: staging binds, for reset forensics
        int slot = -1;
        for (UInt32 i = 0; i < kMaxArenaCtx; ++i) if (arenaCtx_[i] == c) slot = static_cast<int>(i);
        IOLog("NVGspControl: staging bind owner %p slot %d pid %d gen %u ok %d\n", owner, slot,
              proc_selfpid(), resetGeneration(), ok ? 1 : 0);
    }
    ulk();
    if (!ok) return kIOReturnBadArgument;
    return flushed ? kIOReturnSuccess : kIOReturnIOError;
}

IOMemoryDescriptor *NVGspControl::memUserDescriptor(const void *owner, UInt32 handle,
                                                bool *isVram) {
    if (!lock_ || !handle || handle > kMaxMem) return nullptr;
    lk(__LINE__);
    const GpuMem &m = mem_[handle - 1];
    IOMemoryDescriptor *d = (m.owner == owner && m.user) ? m.user : nullptr;
    if (d) d->retain();
    if (isVram) *isVram = d && m.cpu;   // 0.123.0: BAR1 range
    ulk();
    return d;
}

// 0.106.0: exec user pushbuffer segments by GPU VA (NVK nvkmd exec / IOAccel
// command-queue submit): one GP entry per {va, dwords} segment, then the
// kernel fence tail from our PB ring (semaphore release + non-stall intr).
// Asynchronous; returns the fence sequence. engine 0 = GR, 1 = CE.
// 0.135.0: at most one boost request per second while clients submit; the
// 3 s duration lets GSP drop back to P8 shortly after the last submission.
void NVGspControl::noteGpuBusy() {
    if (!boostCall_) return;
    UInt64 now = 0, ns = 0;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now - lastBoostAbs_, &ns);
    if (lastBoostAbs_ && ns < 1000000000ULL) return;
    lastBoostAbs_ = now;
    thread_call_enter(boostCall_);
}

void NVGspControl::boostCallout(thread_call_param_t p, thread_call_param_t) {
    NVGspControl *self = static_cast<NVGspControl *>(p);
    constexpr UInt32 kClientHandle = 0xc0d00001, kSubdevice = 0xc0d02080;
    constexpr UInt32 kCmd = 0x20800a9a, kBytes = 8;      // INTERNAL_PERF_BOOST_SET_2X
    UInt8 control[24 + 8]{};
    __builtin_memcpy(control, &kClientHandle, 4);
    __builtin_memcpy(control + 4, &kSubdevice, 4);
    __builtin_memcpy(control + 8, &kCmd, 4);
    __builtin_memcpy(control + 16, &kBytes, 4);
    control[24] = 2;                                   // BOOST_TO_MAX
    const UInt32 seconds = 3;
    __builtin_memcpy(control + 28, &seconds, 4);
    UInt8 reply[64];
    UInt32 replyBytes = sizeof(reply), result = 0;
    if (self->userRpc(76, control, sizeof(control), reply, &replyBytes, &result) == kIOReturnSuccess) {
        self->boostSent_++;
        self->setProperty("NVGspControl-activity-boosts", self->boostSent_, 32);
    }
    else
        self->lastBoostAbs_ = 0;                       // busy: try again on the next submit
}

IOReturn NVGspControl::submitSegments(const void *owner, UInt32 engine, const UInt64 *segVa,
                                      const UInt32 *segDwords, const UInt32 *segFlags, UInt32 n,
                                      UInt32 *seqOut, UInt64 stampVa, UInt32 stampValue) {
    if (nvramBoostPinned_ == 0) noteGpuBusy();
    if (!lock_ || !pci_ || !segVa || !segDwords || !segFlags || !n || n > nvgsp::kExecMaxSegments || !seqOut)
        return kIOReturnBadArgument;
    for (UInt32 i = 0; i < n; ++i)
        if (!nvgsp::execSegmentValid(segVa[i], segDwords[i], segFlags[i]))
            return kIOReturnBadArgument;
    // 0.122.0: engine bit 8 = tail semaphore without RELEASE_WFI (diagnostic:
    // shows whether the PBDMA fetched the GP entries even if the engine hangs)
    // 0.125.0, video engines only (experiments for a ring that is never
    // fetched): bit 9 = also ring the runlist INTERNAL_DOORBELL (runlist +
    // 0x90 = chid, nouveau ga100_chan_start); bit 10 = set the channel's
    // CHRAM ENABLE (write 0x2, ga100_chan_start) before the doorbell.
    // 0.129.0: the internal doorbell made NVDEC0/NVENC0/OFA0 execute live
    // (0.128.0, flag 0x200), so video rings always get it now.
    const bool noWfi = engine & 0x100;
    const bool internalDoorbell = (engine & 0xff) >= 2 || (engine & 0x200);
    const bool chramEnable = engine & 0x400;
    engine &= 0xff;
    lk(__LINE__);
    if (stampVa) {  // 0.174.0: native submissions bypass externalMethod's generation gate
        auto *uc = OSDynamicCast(IOAccelNVGspUserClient,
                                static_cast<OSObject *>(const_cast<void *>(owner)));
        const IOReturn status = uc ? uc->generationStatus() : kIOReturnBadArgument;
        if (status != kIOReturnSuccess) { ulk(); return status; }
    }
    if (!waitChannelMutationLocked()) { ulk(); return kIOReturnNotReady; }
    Ring ring{};
    bool have = false;
    ClientChannel *own = engine == 0 ? clientChannelLocked(owner) : nullptr;   // 0.178.0
    if (own) {
        have = clientRingLocked(own, &ring);
    } else if (engine == 0) {
        have = grRing(&ring);
    } else if (engine == 1) {
        have = ceRing(&ring);
    } else if (engine < 2 + nvgsp::kVideoEngineCount) {
        have = videoRing(engine - 2, &ring);                 // 0.116.0
    }
    // 0.112.0: per-engine count of GP entries in flight
    UInt32 &outstanding = own ? own->outstanding : engine == 0 ? asyncOutstanding_
        : engine == 1 ? ceOutstanding_ : video_[have ? engine - 2 : 0].outstanding;
    IOReturn ret = !have ? kIOReturnNotReady
                         : own ? clientArenaLocked(own) : arenaSwitchLocked(owner);   // 0.111.0
    if (own && ret == kIOReturnSuccess && !clientRingLocked(own, &ring)) ret = kIOReturnNotReady;
    if (stampVa) {
        // arenaSwitchLocked may wait for thaw with lock_ dropped. A reset
        // during that wait invalidates both the client and the ring snapshot.
        auto *uc = OSDynamicCast(IOAccelNVGspUserClient,
                                static_cast<OSObject *>(const_cast<void *>(owner)));
        const IOReturn status = uc ? uc->generationStatus() : kIOReturnBadArgument;
        if (status != kIOReturnSuccess) ret = status;
    }
    if (ret == kIOReturnSuccess && engine == 0 && !own) noteGrOwnerLocked(owner);
    if (ret == kIOReturnSuccess && own) {
        ++cchanSubmits_;
        setProperty("NVGspControl-cchan-submits", cchanSubmits_, 32);
    }
    // Never fall back to unordered GPU stamp releases when the CPU queue is
    // full: a later channel could complete earlier work that is still running.
    if (ret == kIOReturnSuccess && stampVa && engine == 0 && anyClientChannelLocked()) {
        stampAdvanceLocked();
        if (stampQTail_ - stampQHead_ >= kStampQ) ret = kIOReturnNoResources;
    }
    if (ret == kIOReturnSuccess) noteUseLocked(owner);   // 0.149.0
    if (ret == kIOReturnSuccess) {
        ret = kIOReturnIOError;
        if (*ring.pbOff + 64 > ring.pbBytes || outstanding + n > 384) {
            if (!waitRingSem(ring, *ring.seq, 2000000)) { ulk(); return kIOReturnTimeout; }
            outstanding = 0;
        }
        if (*ring.pbOff + 64 > ring.pbBytes) *ring.pbOff = 0;
        const UInt32 seq = ++*ring.seq;
        // 0.163.0: with GPU-cacheable system memory bound anywhere, a prologue
        // (wait idle, invalidate the L2's clean sysmem lines: the CPU may have
        // written them since) and SET_REFERENCE before the releases (makes the
        // GPU's sysmem writes visible before the fence), the pre-Hopper
        // sequence NVK uses for host access barriers. GR and CE rings only.
        const bool coh = sysCachedUsed_ && engine <= 1;
        const UInt32 pro[3] = {0x80000014U, 0x2001000dU, 0x70000000U};   // IMMD SET_REFERENCE; MEM_OP_D L2_SYSMEM_INVALIDATE
        // 0.152.0: optional IOAccel stamp release first (native N1), then
        // the ring's own fence, then the non-stall interrupt
        UInt32 tail[26], tw = 0;
        if (coh) tail[tw++] = 0x80000014U;                                // IMMD SET_REFERENCE 0
        // 0.178.12: with client channels the GR stamp is written by the CPU
        // in submission order (stampAdvanceLocked), not by this channel's GPU
        const bool cpuStamp = stampVa && engine == 0 && anyClientChannelLocked() &&
                              stampQTail_ - stampQHead_ < kStampQ;
        if (stampVa && !cpuStamp) {
            tail[tw++] = 0x20050017;
            tail[tw++] = static_cast<UInt32>(stampVa & 0xfffffffcULL);
            tail[tw++] = static_cast<UInt32>((stampVa >> 32) & 0xff);
            tail[tw++] = stampValue; tail[tw++] = 0; tail[tw++] = 0x00100001U;
        }
        const UInt32 fence[8] = {
            0x20050017, static_cast<UInt32>(ring.semVa & 0xfffffffcULL),
            static_cast<UInt32>((ring.semVa >> 32) & 0xff), seq, 0,
            noWfi ? 0x00000001U : 0x00100001U, 0x20010008, 0};
        for (UInt32 i = 0; i < 8; ++i) tail[tw++] = fence[i];
        const UInt64 tailPhys = ring.pbPhys + *ring.pbOff, tailVa = ring.pbVa + *ring.pbOff;
        // the prologue sits right after the tail in the same 256-byte slot
        const UInt64 proPhys = tailPhys + UInt64(tw) * 4, proVa = tailVa + UInt64(tw) * 4;
        UInt32 put = 0, flush = 0;
        bool ok = ringWrite(tailPhys, tail, tw) && (!coh || ringWrite(proPhys, pro, 3)) &&
            ringRead(ring.userdPhys + 0x8c, &put);
        put &= 0x1ff;
        if (ok && coh) {
            const UInt32 entry[2] = {static_cast<UInt32>(proVa & 0xfffffffcULL), nvgsp::execEntryHigh(proVa, 3, 0)};
            ok = ringWrite(ring.gpfifoPhys + put * 8, entry, 2);
            put = (put + 1) & 0x1ff;
        }
        for (UInt32 i = 0; ok && i <= n; ++i) {
            const UInt64 va = i < n ? segVa[i] : tailVa;
            const UInt32 len = i < n ? segDwords[i] : tw;
            const UInt32 entry[2] = {static_cast<UInt32>(va & 0xfffffffcULL),
                                     nvgsp::execEntryHigh(va, len, i < n ? segFlags[i] : 0)};
            ok = ringWrite(ring.gpfifoPhys + put * 8, entry, 2);
            put = (put + 1) & 0x1ff;
        }
        // 0.151.0: read PUT back before the doorbell, so every posted BAR1
        // write (tail, entries, PUT) has reached VRAM when the host looks
        ok = ok && ringWrite(ring.userdPhys + 0x8c, &put, 1) &&
            ringRead(ring.userdPhys + 0x8c, &flush) && flush == put;
        IOMemoryMap *map = ok ? sharedBar0Map(pci_) : nullptr;
        if (map && map->getLength() >= 0x00BB0094) {
            Bar0Io bar0{map};
            const VideoChan *vc = engine >= 2 ? &video_[engine - 2] : nullptr;
            const UInt32 chid = vc ? nvgsp::videoEngine(engine - 2)->chid : 0;
            if (vc && chramEnable && vc->chram)
                bar0.write(vc->chram + chid * 4, 0x2);   // ENABLE_SET
            bar0.write(0x00BB0090, ring.token);
            if (vc && internalDoorbell && vc->runlist)
                bar0.write(vc->runlist + 0x90, chid);
            ret = kIOReturnSuccess;
        }
        if (map) map->release();
        *ring.pbOff += 256;
        outstanding += n + (coh ? 2 : 1);
        *seqOut = seq;
        if (cpuStamp && ret == kIOReturnSuccess) {
            PendingStamp &q = stampQ_[stampQTail_++ % kStampQ];
            q.value = stampValue;
            q.seq = seq;
            q.offset = static_cast<UInt32>(stampVa - (0x104000000ULL + kStampCtxOff));
            q.slot = own ? static_cast<UInt16>(own - cchan_) : 0xffff;
            q.serial = own ? own->serial : 0;
            stampQueued_ = stampValue;
        }
    }
    ulk();
    return ret;
}

// 0.140.0 (D3): identity output LUT at scratch + 0xFF10000 (after the two
// cursor buffers), own ctxdma at RAMIN+0x20C0, RAMHT at both hash slots.
IOReturn NVGspControl::olutSetup() {
    if (!dispInstOffset_ || !scratchOffset_) return kIOReturnNotReady;
    const UInt64 surf = scratchOffset_ + nvgsp::kOlutScratchDelta;
    constexpr UInt32 kWords = nvgsp::kIdentityLutWords;
    UInt32 *lut = static_cast<UInt32 *>(IOMalloc(kWords * 4));
    if (!lut) return kIOReturnNoMemory;
    nvgsp::buildIdentityDisplayLut(lut, kWords, false);
    IOReturn r = kIOReturnSuccess;
    for (UInt32 o = 0; o < kWords && r == kIOReturnSuccess; o += 1024) {
        const UInt32 n = kWords - o < 1024 ? kWords - o : 1024;
        r = vramAccess(surf + o * 4, lut + o, n, true);
    }
    IOFree(lut, kWords * 4);
    if (r != kIOReturnSuccess) return r;
    UInt32 h = kOlutCtxdma, hash = 0;
    while (h) { hash ^= h & 0x3ff; h >>= 10; }
    const UInt32 ent[2] = {kOlutCtxdma, (0xc0d00001 & 0x3fff) | (0x20C0U << 9)};
    const UInt32 dma[4] = {0x5, static_cast<UInt32>(surf >> 8), 0,
                           static_cast<UInt32>((surf + 0x2fff) >> 8)};
    lk(__LINE__);
    const bool ok = praminWriteWords(pci_, dispInstOffset_ + 0x20C0, dma, 4) &&
        praminWriteWords(pci_, dispInstOffset_ + hash * 8, ent, 2) &&
        praminWriteWords(pci_, dispInstOffset_ + (hash ^ ((0xc0d00001 & 0xff) << 2)) * 8, ent, 2);
    ulk();
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

// Window input identity is FP16, while the head output identity is fixed-point.
// Use a separate surface so programming one cannot overwrite the other.
IOReturn NVGspControl::ilutSetup() {
    if (!dispInstOffset_ || !scratchOffset_) return kIOReturnNotReady;
    const UInt64 surf = scratchOffset_ + nvgsp::kIlutScratchDelta;
    constexpr UInt32 kWords = nvgsp::kIdentityLutWords;
    UInt32 *lut = static_cast<UInt32 *>(IOMalloc(kWords * 4));
    if (!lut) return kIOReturnNoMemory;
    nvgsp::buildIdentityDisplayLut(lut, kWords, true);
    IOReturn r = kIOReturnSuccess;
    for (UInt32 o = 0; o < kWords && r == kIOReturnSuccess; o += 1024) {
        const UInt32 n = kWords - o < 1024 ? kWords - o : 1024;
        r = vramAccess(surf + o * 4, lut + o, n, true);
    }
    IOFree(lut, kWords * 4);
    if (r != kIOReturnSuccess) return r;
    constexpr UInt32 kChid = 1;
    UInt32 h = kIlutCtxdma, hash = 0;
    while (h) { hash ^= h & 0x3ff; h >>= 10; }
    hash ^= kChid << 6;
    const UInt32 ent[2] = {kIlutCtxdma, (kChid << 25) | (0xc0d00001 & 0x3fff) | (0x20E0U << 9)};
    const UInt32 dma[4] = {0x5, static_cast<UInt32>(surf >> 8), 0,
                           static_cast<UInt32>((surf + 0x2fff) >> 8)};
    lk(__LINE__);
    const bool ok = praminWriteWords(pci_, dispInstOffset_ + 0x20E0, dma, 4) &&
        praminWriteWords(pci_, dispInstOffset_ + hash * 8, ent, 2) &&
        praminWriteWords(pci_, dispInstOffset_ + (hash ^ ((0xc0d00001 & 0xff) << 2)) * 8, ent, 2);
    ulk();
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn NVGspControl::cursorSetup() {
    if (cursorReady_) return kIOReturnSuccess;
    if (!dispInstOffset_ || !scratchOffset_ || !corePut_ || !internalClient_)
        return kIOReturnNotReady;
    UInt8 reply[256];
    UInt32 replyBytes = sizeof(reply), result = ~0U;
    // set_pushbuf(C67A, head 0, valid 0) then the PIO channel alloc.
    nvgsp::NvDispChannelPushbufferParams pb{};
    pb.hclass = 0xc67a;
    pb.channelInstance = 0;
    pb.valid = 0;
    UInt8 ctrl[24 + sizeof(pb)]{};
    const UInt32 cmd = nvgsp::kDispSetChannelPushbuffer, pbBytes = sizeof(pb);
    __builtin_memcpy(ctrl, &internalClient_, 4);
    __builtin_memcpy(ctrl + 4, &internalSubdevice_, 4);
    __builtin_memcpy(ctrl + 8, &cmd, 4);
    __builtin_memcpy(ctrl + 16, &pbBytes, 4);
    __builtin_memcpy(ctrl + 24, &pb, sizeof(pb));
    IOReturn r = userRpc(76, ctrl, sizeof(ctrl), reply, &replyBytes, &result);
    UInt32 st = ~0U;
    if (r == kIOReturnSuccess && replyBytes >= 96) __builtin_memcpy(&st, reply + 92, 4);
    setProperty("NVGspControl-cursor-pushbuf-status", st, 32);
    UInt8 alloc[32 + 16]{};
    const UInt32 a[8] = {0xc0d00001, 0xc0d0c770, 0xc0d0c67a, 0xc67a, 0, 16, 0, 0};
    __builtin_memcpy(alloc, a, sizeof(a));
    replyBytes = sizeof(reply);
    r = userRpc(103, alloc, sizeof(alloc), reply, &replyBytes, &result);
    st = ~0U;
    if (r == kIOReturnSuccess && replyBytes >= 100) __builtin_memcpy(&st, reply + 96, 4);
    setProperty("NVGspControl-cursor-alloc-status", st, 32);
    if (st != 0 && st != 0x56) return kIOReturnIOError;  // 0x56: already ours
    // 0.138.0: own tight ctxdma (RAMIN+0x20A0) over the two 16 KiB cursor
    // buffers, like NVIDIA's per-surface ctxdmas, instead of the window's
    // [0, 16 GiB) one; RAMHT entry at both the nouveau slot and NVIDIA's
    // (disp_inst_mem_0300.c also hashes hClient[7:0] << 2).
    cursorBase_ = scratchOffset_ + 0xFF00000;
    UInt32 h = kCursorCtxdma, hash = 0;
    while (h) { hash ^= h & 0x3ff; h >>= 10; }
    const UInt32 ent[2] = {kCursorCtxdma, (0xc0d00001 & 0x3fff) | (0x20A0U << 9)};
    const UInt32 dma[4] = {0x5, static_cast<UInt32>(cursorBase_ >> 8), 0,
                           static_cast<UInt32>((cursorBase_ + 0x7fff) >> 8)};
    lk(__LINE__);
    const bool ok = praminWriteWords(pci_, dispInstOffset_ + 0x20A0, dma, 4) &&
        praminWriteWords(pci_, dispInstOffset_ + hash * 8, ent, 2) &&
        praminWriteWords(pci_, dispInstOffset_ + (hash ^ ((0xc0d00001 & 0xff) << 2)) * 8, ent, 2);
    ulk();
    if (!ok) return kIOReturnIOError;
    setProperty("NVGspControl-cursor-unstick", static_cast<UInt32>(coreUnstick()), 32);
    cursorBuf_ = 0;
    cursorReady_ = true;
    setProperty("NVGspControl-cursor-ready", true);
    return kIOReturnSuccess;
}

IOReturn NVGspControl::cursorImage(const UInt32 *argb64x64, UInt32 hotX, UInt32 hotY) {
    IOReturn r = cursorSetup();
    if (r != kIOReturnSuccess) return r;
    const UInt32 next = cursorBuf_ ^ 1;
    const UInt64 surf = cursorBase_ + next * 0x4000;
    r = vramAccess(surf, const_cast<UInt32 *>(argb64x64), 1024, true);
    for (UInt32 i = 1; r == kIOReturnSuccess && i < 4; ++i)
        r = vramAccess(surf + i * 4096, const_cast<UInt32 *>(argb64x64 + i * 1024), 1024, true);
    if (r != kIOReturnSuccess) return r;
    const UInt32 control = 0x80000000U | ((hotY & 0xff) << 20) | ((hotX & 0xff) << 12) |
        (1U << 8) | 0xCF;   // ENABLE | hotspot | W64_H64 | A8R8G8B8
    // Reserve at least64px while preserving any larger initialized capacity.
    // Enabled cursor also requires the normal composition/ILUT/OLUT path;
    // bounds alone did not resolve the bypass-path UPDATE rejection.
    UInt32 usage = 0x1110;
    {
        IOMemoryMap *m = sharedBar0Map(pci_);
        const bool ok = m && m->getLength() >= 0x68A034 && Bar0Io{m}.read(0x68A030, &usage);
        if (m) m->release();
        if (!ok || (usage & 7U) > 4U) return kIOReturnIOError;
    }
    const UInt32 capacity = (usage & 7U) < 2U ? 2U : usage & 7U;
    usage = (usage & ~7U) | capacity;
    const UInt32 relative = static_cast<UInt32>((surf - cursorBase_) >> 8);
    UInt32 expected[] = {kCursorCtxdma,kCursorCtxdma,relative,relative,0,
        cursorVisible_ ? control : (control & 0x7fffffffU),0x75ff};
    const UInt32 words[] = {
        (1U << 18) | 0x2030, usage,
        (1U << 18) | 0x2098, 0,                          // PRESENT_CONTROL_CURSOR mono
        (2U << 18) | 0x2088, kCursorCtxdma, kCursorCtxdma, // both cursor DMA slots
        // The cursor DMA object starts at cursorBase_; OFFSET is relative
        // to that object, not an absolute VRAM address (NVIDIA SetCursorSurfaceAddress).
        (2U << 18) | 0x2090, static_cast<UInt32>((surf - cursorBase_) >> 8),
        static_cast<UInt32>((surf - cursorBase_) >> 8),   // both cursor offsets
        (1U << 18) | 0x209C, cursorVisible_ ? control : (control & 0x7fffffffU),
        (1U << 18) | 0x20A0, 0x75FF,                     // non-premultiplied alpha
        (2U << 18) | 0x218, 0, 0,                        // no interlocks
        (1U << 18) | 0x200, 1,                           // UPDATE, RELEASE_ELV
    };
    r = submitCore(words, sizeof(words) / 4);
    if (r == kIOReturnSuccess) r = waitCursorArmed(usage, expected);
    if (r == kIOReturnSuccess) {
        cursorControl_ = control;
        cursorBuf_ = next;
    } else {
        // Restore a disabled image at the same capacity; never shrink live
        // cursor resources on this failure path. Report cleanup separately.
        expected[5] = control & 0x7fffffffU;
        const UInt32 off[] = {(1U << 18) | 0x209C, expected[5],
                              (2U << 18) | 0x218, 0, 0, (1U << 18) | 0x200, 1};
        IOReturn cleanup = submitCore(off, sizeof(off) / 4);
        if (cleanup == kIOReturnSuccess) cleanup = waitCursorArmed(usage, expected);
        setProperty("NVGspControl-cursor-failure-disable-result", static_cast<UInt32>(cleanup), 32);
        cursorVisible_ = false;
        cursorControl_ = 0; // another image must ARM before show is usable
    }
    return r;
}

IOReturn NVGspControl::cursorShow(bool visible) {
    if (!cursorReady_ || !cursorControl_) return kIOReturnNotReady;
    UInt32 usage = 0, expected[7] = {};
    if (peekBar0(0x68a030, 1, &usage) != kIOReturnSuccess ||
        peekBar0(0x68a088, 7, expected) != kIOReturnSuccess) return kIOReturnIOError;
    expected[5] = visible ? cursorControl_ : (cursorControl_ & 0x7fffffffU);
    const UInt32 words[] = {
        (1U << 18) | 0x209C, visible ? cursorControl_ : (cursorControl_ & 0x7fffffffU),
        (2U << 18) | 0x218, 0, 0,
        (1U << 18) | 0x200, 1,
    };
    IOReturn r = submitCore(words, sizeof(words) / 4);
    if (r == kIOReturnSuccess) r = waitCursorArmed(usage, expected);
    if (r == kIOReturnSuccess) cursorVisible_ = visible;
    else {
        expected[5] = cursorControl_ & 0x7fffffffU;
        const UInt32 off[] = {(1U << 18) | 0x209C, expected[5],
                              (2U << 18) | 0x218, 0, 0, (1U << 18) | 0x200, 1};
        IOReturn cleanup = submitCore(off, sizeof(off) / 4);
        if (cleanup == kIOReturnSuccess) cleanup = waitCursorArmed(usage, expected);
        setProperty("NVGspControl-cursor-failure-disable-result", static_cast<UInt32>(cleanup), 32);
        cursorVisible_ = false;
        cursorControl_ = 0; // require another verified image after a failed transition
    }
    return r;
}

// 0.138.0: D2 bring-up without NVDisplay. op 0 = setup only, 1 = 64x64
// test image (red frame, translucent blue inside) + show at 64,64, 2 = hide.
IOReturn NVGspControl::cursorTest(UInt32 op) {
    if (op == 2) return cursorShow(false);
    IOReturn r = cursorSetup();
    if (r != kIOReturnSuccess || op == 0) return r;
    UInt32 *img = static_cast<UInt32 *>(IOMalloc(64 * 64 * 4));
    if (!img) return kIOReturnNoMemory;
    for (UInt32 y = 0; y < 64; ++y)
        for (UInt32 x = 0; x < 64; ++x)
            img[y * 64 + x] = (x < 4 || y < 4 || x > 59 || y > 59) ? 0xffff0000U : 0x800000ffU;
    cursorVisible_ = true;
    r = cursorImage(img, 0, 0);
    IOFree(img, 64 * 64 * 4);
    if (r == kIOReturnSuccess) r = cursorMove(64, 64);
    setProperty("NVGspControl-cursor-test", static_cast<UInt32>(r), 32);
    return r;
}

IOReturn NVGspControl::cursorMove(SInt32 x, SInt32 y) {
    // Cursor PIO (user area 0x6D8000 + head*0x1000): hot-spot point out, then
    // UPDATE (nvkms MoveCursorC3). Each store needs its own free-space check.
    if (!cursorReady_ || !pci_) return kIOReturnNotReady;
    IOMemoryMap *map = sharedBar0Map(pci_);
    if (!map || map->getLength() < 0x6D9000) { if (map) map->release(); return kIOReturnNoMemory; }
    Bar0Io bar0{map};
    const auto waitFree = [&bar0]() -> IOReturn {
        for (UInt32 i = 0; i < 100; ++i) {
            UInt32 space = 0;
            if (!bar0.read(0x6D8008, &space)) return kIOReturnIOError;
            if (space & 0x3fU) return kIOReturnSuccess;
            IOSleep(1);
        }
        return kIOReturnTimeout;
    };
    IOReturn result = waitFree();
    if (result == kIOReturnSuccess &&
        !bar0.write(0x6D8208, (static_cast<UInt32>(y & 0xffff) << 16) |
                              static_cast<UInt32>(x & 0xffff))) result = kIOReturnIOError;
    if (result == kIOReturnSuccess) result = waitFree();
    if (result == kIOReturnSuccess && !bar0.write(0x6D8200, 0)) result = kIOReturnIOError;
    map->release();
    return result;
}

// 0.87.0: GSP LibOS log buffer read-out (LOGINIT 0, LOGINTR 1, LOGRM 2,
// LOGMNOC 3, LOGKRNL 4; 64 KiB each) for host-side decoding.
IOReturn NVGspControl::readGspLog(UInt32 index, UInt32 offset, UInt8 *out, UInt32 bytes) {
    // 0.99.2: index 8 = shared queue memory (516 KiB: PTEs, cmd, status).
    if (index == 8) {
        if (!lock_ || !out || !bytes || bytes > 4096 || offset + bytes > nvgsp::kGspSharedBytes)
            return kIOReturnBadArgument;
        lk(__LINE__);
        const bool okq = init_.snapshotQueue(offset, out, bytes);
        ulk();
        return okq ? kIOReturnSuccess : kIOReturnNotReady;
    }
    if (!lock_ || index >= 5 || !out || !bytes || bytes > 4096 || offset + bytes > 0x10000)
        return kIOReturnBadArgument;
    UInt8 *copy = static_cast<UInt8 *>(IOMalloc(0x10000));
    if (!copy) return kIOReturnNoMemory;
    lk(__LINE__);
    const bool ok = init_.snapshotLog(index, copy, 0x10000);
    ulk();
    if (ok) __builtin_memcpy(out, copy + offset, bytes);
    IOFree(copy, 0x10000);
    return ok ? kIOReturnSuccess : kIOReturnNotReady;
}

// 0.88.0: DPMS for the DP sink the way nvkms does it through DPLib
// (nvDPDeviceSetPowerState): DPCD 0x600 SET_POWER = 1 (D0) / 2 (D3), one
// native AUX write via NV0073_CTRL_CMD_DP_AUXCH_CTRL on display 0x200.
IOReturn NVGspControl::dpSetPower(bool on) {
    UInt8 ctrl[24 + 48]{};
    const UInt32 hdr[6] = {0xc0d00001, 0xc0d00073, 0x731341, 0, 48, 0};
    __builtin_memcpy(ctrl, hdr, sizeof(hdr));
    UInt8 *pp = ctrl + 24;
    const UInt32 displayId = 0x200, cmd = 0x8 /* AUX | WRITE */, addr = 0x600, size = 0;
    __builtin_memcpy(pp + 4, &displayId, 4);
    __builtin_memcpy(pp + 12, &cmd, 4);
    __builtin_memcpy(pp + 16, &addr, 4);
    pp[20] = on ? 1 : 2;
    __builtin_memcpy(pp + 36, &size, 4);
    UInt8 reply[256];
    UInt32 replyBytes = sizeof(reply), result = ~0U;
    IOReturn r = kIOReturnError;
    // 0.100.10: a sink leaving D3 may DEFER/NACK the first AUX transactions
    // (DP: up to 1 ms wake + retries); 10 attempts, 10 ms apart, with diag.
    UInt32 lastKr = 0, lastSt = ~0U, lastRep = ~0U, attempts = 0;
    for (UInt32 attempt = 0; attempt < 10; ++attempt) {
        ++attempts;
        replyBytes = sizeof(reply);
        r = userRpc(76, ctrl, sizeof(ctrl), reply, &replyBytes, &result);
        UInt32 st = ~0U, rep = ~0U;
        if (r == kIOReturnSuccess && replyBytes >= 148) {
            __builtin_memcpy(&st, reply + 92, 4);
            __builtin_memcpy(&rep, reply + 144, 4);
        }
        lastKr = static_cast<UInt32>(r); lastSt = st; lastRep = rep;
        if (r == kIOReturnSuccess && st == 0 && rep == 0) { r = kIOReturnSuccess; break; }
        r = kIOReturnIOError;
        IOSleep(10);
    }
    setProperty("NVGspControl-dpms-last-kr", lastKr, 32);
    setProperty("NVGspControl-dpms-last-status", lastSt, 32);
    setProperty("NVGspControl-dpms-last-reply", lastRep, 32);
    setProperty("NVGspControl-dpms-last-attempts", attempts, 32);
    ++dpmsCalls_;
    setProperty("NVGspControl-dpms-state", on);
    setProperty("NVGspControl-dpms-calls", dpmsCalls_, 32);
    setProperty("NVGspControl-dpms-last-result", static_cast<UInt32>(r), 32);
    return r;
}

// 0.93.0: NV04_DISPLAY_COMMON control helper — the modeset3.py ctl() header
// ({client, display-common, cmd, 0, paramBytes, 0} + params) with the same
// status extraction (reply+92) and optional params echo (reply+104).
UInt32 NVGspControl::displayCtrl(UInt32 cmd, const UInt8 *params, UInt32 paramBytes,
                                 UInt8 *echoOut, UInt32 echoBytes) {
    if (!params || !paramBytes || paramBytes > 256) return ~0U;
    UInt8 ctrl[24 + 256]{};
    const UInt32 hdr[6] = {0xc0d00001, 0xc0d00073, cmd, 0, paramBytes, 0};
    __builtin_memcpy(ctrl, hdr, sizeof(hdr));
    __builtin_memcpy(ctrl + 24, params, paramBytes);
    UInt8 reply[256];
    UInt32 replyBytes = sizeof(reply), result = ~0U;
    const IOReturn r = userRpc(76, ctrl, 24 + paramBytes, reply, &replyBytes, &result);
    UInt32 st = ~0U;
    if (r == kIOReturnSuccess && replyBytes >= 96) {
        __builtin_memcpy(&st, reply + 92, 4);
        if (echoOut && echoBytes && replyBytes > 104) {
            UInt32 n = replyBytes - 104;
            if (n > echoBytes) n = echoBytes;
            if (n > paramBytes) n = paramBytes;
            __builtin_memcpy(echoOut, reply + 104, n);
        }
    }
    return st;
}

namespace {
// 0.93.0: valid head-0 method offsets for the modeset snapshot — clc77d.h
// idx/idx2 methods in 0x2000-0x2400, live-filtered (the good_head.json key
// set from the passing 17:18 modeset3 run; 133 entries).
constexpr UInt32 kHeadAddrs[] = {
    0x2000, 0x2004, 0x2008, 0x200c, 0x2010, 0x2014, 0x2018, 0x201c,
    0x2020, 0x2024, 0x2028, 0x202c, 0x2030, 0x2034, 0x2044, 0x2048,
    0x204c, 0x2058, 0x205c, 0x2064, 0x2068, 0x206c, 0x2070, 0x2078,
    0x207c, 0x2080, 0x2088, 0x208c, 0x2090, 0x2094, 0x2098, 0x209c,
    0x20a0, 0x2180, 0x2184, 0x218c, 0x2194, 0x2198, 0x219c, 0x21a0,
    0x21a8, 0x21ac, 0x21b0, 0x21cc, 0x21d0, 0x21ec, 0x21f0, 0x2218,
    0x2220, 0x2224, 0x2228, 0x222c, 0x2238, 0x223c, 0x2240, 0x2244,
    0x2248, 0x224c, 0x2250, 0x2254, 0x2258, 0x225c, 0x2260, 0x2264,
    0x2268, 0x226c, 0x2270, 0x2280, 0x2284, 0x2288, 0x228c, 0x229c,
    0x22a0, 0x22a4, 0x22a8, 0x22ac, 0x22b0, 0x22b4, 0x22b8, 0x22bc,
    0x22c0, 0x22c4, 0x22c8, 0x22cc, 0x22d0, 0x22d4, 0x22d8, 0x22dc,
    0x22e0, 0x22e4, 0x22e8, 0x22ec, 0x22f0, 0x22f4, 0x22f8, 0x22fc,
    0x2300, 0x2304, 0x2308, 0x230c, 0x2310, 0x2314, 0x2318, 0x231c,
    0x2320, 0x2324, 0x2328, 0x232c, 0x2330, 0x2334, 0x2338, 0x233c,
    0x2340, 0x2344, 0x2348, 0x234c, 0x2350, 0x2354, 0x2358, 0x235c,
    0x2360, 0x2364, 0x2368, 0x236c, 0x2370, 0x2374, 0x2380, 0x2384,
    0x23a4, 0x23a8, 0x23ac, 0x23b0, 0x23b4,
};
}  // namespace

// 0.93.0: full driver-driven modeset cycle on head 0 in nouveau order
// (SET_MANUAL_DP, detach, DP release, ASSIGN_SOR, sink D0, train,
// CONFIG_STREAM, attach), byte-identical to the passing modeset3.py run.
// Reads the live ARMED head state (BAR0 0x688000 window) instead of a baked
// snapshot. Never holds lock_ itself; every step locks internally.
// 0.99.5: NVC372_CTRL_CMD_IS_MODE_POSSIBLE for head 0 (4000x2222 raster,
// 533.25 MHz, window 0 fmt 0x197, cursor none) — values from the live IMP
// generator (NVIDIA ctrlc372chnc.h layout, 2048 B). Returns status<<8|possible.
UInt32 NVGspControl::impCheckHead0() {
    static const UInt32 kImp[][2] = {
        {4, 0x101}, {12, 0x82302}, {16, 0xfa0}, {20, 0x8ae}, {24, 0xf6f},
        {28, 0x8aa}, {32, 0x6f}, {36, 0x3a}, {40, 0x1}, {64, 0x400},
        {68, 0x400}, {72, 0x2}, {80, 0x2}, {752, 0x197}, {760, 0xf00},
        {764, 0x400}, {768, 0x400}, {772, 0x2000002}};
    UInt8 alloc[32]{};
    const UInt32 a[8] = {0xc0d00001, 0xc0d00080, 0xc0d0c372, 0xc372, 0, 0, 0, 0};
    __builtin_memcpy(alloc, a, sizeof(a));
    UInt8 small[256];
    UInt32 smallBytes = sizeof(small), result = 0;
    userRpc(103, alloc, sizeof(alloc), small, &smallBytes, &result);   // 0x19 = exists
    constexpr UInt32 kParams = 2048;
    UInt8 *buf = static_cast<UInt8 *>(IOMalloc(24 + kParams));
    UInt8 *reply = static_cast<UInt8 *>(IOMalloc(4096));
    UInt32 ret = ~0U;
    if (buf && reply) {
        bzero(buf, 24 + kParams);
        const UInt32 hdr[6] = {0xc0d00001, 0xc0d0c372, 0xc3720101, 0, kParams, 0};
        __builtin_memcpy(buf, hdr, sizeof(hdr));
        for (const auto &kv : kImp) __builtin_memcpy(buf + 24 + kv[0], &kv[1], 4);
        // 0.137.1 (D4): the chosen mode's raster, not the firmware 4K60 one.
        if (mode_.valid) {
            const UInt32 hv[7] = {mode_.pclkHz / 1000, mode_.rasterSize & 0xffff,
                                  mode_.rasterSize >> 16, mode_.blankStart & 0xffff,
                                  mode_.blankStart >> 16, mode_.blankEnd & 0xffff,
                                  mode_.blankEnd >> 16};
            __builtin_memcpy(buf + 24 + 12, hv, sizeof(hv));
            const UInt16 idle[2] = {static_cast<UInt16>(mode_.minFrameIdle & 0xffff),
                                    static_cast<UInt16>(mode_.minFrameIdle >> 16)};
            __builtin_memcpy(buf + 24 + 76, idle, sizeof(idle));
            if (mode_.cursor64) buf[24 + 81] = 2;   // cursorSize32p: 64 px
        }
        UInt32 replyBytes = 4096;
        if (userRpc(76, buf, 24 + kParams, reply, &replyBytes, &result) == kIOReturnSuccess &&
            replyBytes >= 104 + 1905) {
            UInt32 st = 0;
            __builtin_memcpy(&st, reply + 92, 4);
            ret = (st << 8) | reply[104 + 1904];
        }
    }
    if (buf) IOFree(buf, 24 + kParams);
    if (reply) IOFree(reply, 4096);
    return ret;
}

// 0.137.0 (D4): EDID timing -> C77D head methods, the same way nvkms builds
// them (nvkms-evo3.c EvoSetRasterParams3, nvComputeMinFrameIdle) plus the
// SST blank symbols from dp_watermark.cpp for 4 lanes HBR2 at 30 bpp.
IOReturn NVGspControl::setMode(const UInt32 t[10], UInt32 *statusOut) {
    if (!t || !statusOut) return kIOReturnBadArgument;
    const UInt32 pclkKHz = t[0], hA = t[1], hB = t[2], hSO = t[3], hSW = t[4];
    const UInt32 vA = t[5], vB = t[6], vSO = t[7], vSW = t[8], flags = t[9];
    const UInt64 pclk = UInt64(pclkKHz) * 1000;
    constexpr UInt64 kLane = 540000000ULL;   // HBR2 8b/10b, bytes/s per lane
    if (!pclk || hA <= 60 || hA > kDesktopSurface.width || vA > kDesktopSurface.height ||
        !vA || !hSW || !vSW || hSO + hSW >= hB || vSO + vSW >= vB ||
        pclk * 30 >= 8 * kLane * 4 || pclk >= 0x80000000ULL ||
        pclk * 15 < 1400000000ULL)   // VPLL: P <= 15 and VCO >= 1.4 GHz
        return kIOReturnBadArgument;
    HeadMode m{};
    const UInt32 hT = hA + hB, vT = vA + vB;
    const UInt32 hBE = hB - hSO - 1, vBE = vB - vSO - 1;   // sync + back porch - 1
    m.pclkHz = static_cast<UInt32>(pclk);
    m.hActive = hA;
    m.vActive = vA;
    m.rasterSize = hT | (vT << 16);
    m.syncEnd = (hSW - 1) | ((vSW - 1) << 16);
    m.blankEnd = hBE | (vBE << 16);
    m.blankStart = (hBE + hA) | ((vBE + vA) << 16);
    const UInt32 lead = vBE + 1;
    if (lead < 2 || vT < lead + vA) return kIOReturnBadArgument;
    m.minFrameIdle = lead | ((vT - lead - vA) << 16);
    // OUTPUT_RESOURCE bit2/bit3 = hsync/vsync NEGATIVE_TRUE.
    m.polarity = ((flags & 1) ? 0 : 4) | ((flags & 2) ? 0 : 8);
    // dp_watermark.cpp isModePossibleSST, 4 lanes, enhanced framing, 30 bpp.
    const UInt32 steer = (hA % 4) ? (4 - hA % 4) * 30 : 0;
    const UInt32 blankBits = 3 * 8 * 4 * 2 + 3 * 8 * 4 + steer;
    const UInt32 minHBlank =
        static_cast<UInt32>(UInt64(blankBits) * 100000 / 32 * pclk / kLane / 100000) + 12;
    if (minHBlank > hB) return kIOReturnBadArgument;
    const SInt64 hs = SInt64((UInt64(hB - minHBlank) * kLane) / pclk) - 7;
    const SInt64 vs = SInt64((UInt64(hA - 40) * kLane) / pclk) - 13;
    m.hBlankSym = hs < 0 ? 0 : static_cast<UInt32>(hs);
    m.vBlankSym = vs < 0 ? 0 : static_cast<UInt32>(vs);
    m.valid = true;
    m.cursor64 = (flags & 4) != 0;
    m.olut = (flags & 8) != 0;
    m.composite = (flags & 0x10) != 0;
    // flags bit31 = dry run: publish the computed methods, touch nothing.
    if (flags & 0x80000000U) {
        const UInt32 dry[] = {m.pclkHz, m.hActive | (m.vActive << 16), m.rasterSize, m.syncEnd,
                              m.blankEnd, m.blankStart, m.minFrameIdle, m.polarity,
                              m.hBlankSym, m.vBlankSym};
        OSData *d = OSData::withBytes(dry, sizeof(dry));
        if (d) { setProperty("NVGspControl-mode-dry", d); d->release(); }
        *statusOut = 0;
        return kIOReturnSuccess;
    }
    const HeadMode prev = mode_;
    const WindowSurface prevSurf = desktopSurface_;
    mode_ = m;
    desktopSurface_ = WindowSurface{kDesktopSurface.pitch, hA, vA, kDesktopSurface.format};
    setProperty("NVGspControl-mode-raster", m.rasterSize, 32);
    setProperty("NVGspControl-mode-pclk", m.pclkHz, 32);
    setProperty("NVGspControl-mode-hblank-sym", m.hBlankSym, 32);
    setProperty("NVGspControl-mode-vblank-sym", m.vBlankSym, 32);
    UInt32 code = ~0U;
    const IOReturn r = modesetHead0(&code);
    if (r != kIOReturnSuccess && prev.valid != m.valid) {
        // first switch failed: keep the firmware mode for later modesets
        mode_ = prev;
        desktopSurface_ = prevSurf;
    }
    *statusOut = code;
    setProperty("NVGspControl-mode-result", code, 32);
    return r;
}

IOReturn NVGspControl::modesetHead0(UInt32 *statusOut, bool lightIfLit) {
    if (!statusOut || !pci_ || !lock_) return kIOReturnBadArgument;
    markBoot("modeset");
    constexpr UInt32 kHeads = sizeof(kHeadAddrs) / sizeof(kHeadAddrs[0]);
    UInt32 *vals = static_cast<UInt32 *>(IOMalloc(kHeads * 4));
    if (!vals) return kIOReturnNoMemory;
    UInt32 code = 0;
    // 1. snapshot live ARMED head state + SOR0.
    UInt32 sor = 0, head0 = 0, attachSor = 0x901;
    bool usedCache = false;
    {
        IOMemoryMap *map = sharedBar0Map(pci_);
        bool ok = map && map->getLength() >= 0x68A400;
        if (ok) {
            Bar0Io bar0{map};
            ok = bar0.read(0x688300, &sor) && bar0.read(0x612078, &head0);
            for (UInt32 i = 0; ok && i < kHeads; ++i)
                ok = bar0.read(0x688000 + kHeadAddrs[i], &vals[i]);
        }
        if (map) map->release();
        if (!ok) { code = 0x01000000; goto done; }
    }
    setProperty("NVGspControl-modeset-sor", sor, 32);
    setProperty("NVGspControl-modeset-head0", head0, 32);
    // Attach target: the snapshot SOR when head 0 was lit, else 0x901
    // (modeset3 restore rule — never re-attach to a detached 0x900).
    if (sor == 0x901 && head0 == 0x200) attachSor = sor;
    // 0.99.4: cache the lit state; after S3 (engine at reset defaults) re-use it.
    if (sor == 0x901 && head0 == 0x200) {
        UInt32 v[8] = {};
        if (peekBar0(0x00ef00, 8, v) == kIOReturnSuccess && v[1] != 0x00412001) {
            __builtin_memcpy(vpllCache_, v, sizeof(v));
            vpllCacheValid_ = true;
        }
        static_assert(sizeof(kHeadAddrs) <= 1024, "head cache size");
        if (!headCache_) headCache_ = static_cast<UInt32 *>(IOMalloc(1024));
        if (headCache_) {
            __builtin_memcpy(headCache_, vals, kHeads * 4);
            headCacheValid_ = true;
        }
    } else if (headCacheValid_ && headCache_) {
        __builtin_memcpy(vals, headCache_, kHeads * 4);
        usedCache = true;
        setProperty("NVGspControl-modeset-used-cache", true);
        // 0.100.2: re-program VPLL0 from the VBIOS values if it is at reset
        // (nouveau ga100_devinit_pll_set order: config, fN, P/N/M, update).
        UInt32 cur = 0;
        if (vpllCacheValid_ && peekBar0(0x00ef04, 1, &cur) == kIOReturnSuccess &&
            cur != vpllCache_[1]) {
            IOMemoryMap *vmap = sharedBar0Map(pci_);
            if (vmap) {
                Bar0Io vio{vmap};
                vio.write(0x00ef00, vpllCache_[0]);
                vio.write(0x00ef08, vpllCache_[2]);
                vio.write(0x00ef10, vpllCache_[4]);
                vio.write(0x00ef14, vpllCache_[5]);
                vio.write(0x00ef18, vpllCache_[6]);
                vio.write(0x00ef04, vpllCache_[1]);
                vio.write(0x00e9c0, 1);
                IODelay(1000);
                UInt32 after[2] = {};
                vio.read(0x00ef00, &after[0]);
                vio.read(0x00ef04, &after[1]);
                vmap->release();
                setProperty("NVGspControl-modeset-vpll-restored", true);
                setProperty("NVGspControl-modeset-vpll-ef00", after[0], 32);
                setProperty("NVGspControl-modeset-vpll-ef04", after[1], 32);
            }
        }
    }
    setProperty("NVGspControl-modeset-vpll-cache-valid", vpllCacheValid_);
    // 0.100.7/0.100.9: boot: the VBIOS/GOP already scans out this exact mode —
    // the caches are captured above; never detach/retrain (no flash). Lit =
    // head0 AWAKE + SOR owned (DP lanes may be down while the framebuffer's
    // early doze has the sink in D3 — that is not a reason to modeset).
    if (lightIfLit) {
        const bool lit = sor == 0x901 && head0 == 0x200;
        setProperty("NVGspControl-modeset-light", lit);
        code = lit ? 0 : (0x0B000000 | (head0 & 0xffff));
        goto done;
    }
    setProperty("NVGspControl-modeset-cache-valid", headCacheValid_);
    // 0.137.0 (D4): a chosen mode replaces the snapshot's timing methods.
    if (mode_.valid) {
        for (UInt32 i = 0; i < kHeads; ++i) {
            switch (kHeadAddrs[i]) {
            case 0x2004: vals[i] = (vals[i] & ~0xcU) | mode_.polarity; break;
            case 0x200c: case 0x2028: vals[i] = mode_.pclkHz; break;
            case 0x204c: case 0x2058: vals[i] = mode_.hActive | (mode_.vActive << 16); break;
            case 0x2064: vals[i] = mode_.rasterSize; break;
            case 0x2068: vals[i] = mode_.syncEnd; break;
            case 0x206c: vals[i] = mode_.blankEnd; break;
            case 0x2070: vals[i] = mode_.blankStart; break;
            case 0x2218: vals[i] = mode_.minFrameIdle; break;
            // 0.140.0: OLUT the nvkms way (EvoSetOutputLutC5): DIRECT10 +
            // interpolate, 4 VSS header + 1025 fixed-point entries, norm 1.0,
            // OCSC0 identity enabled after the LUT.
            // 0.165.0: interpolation off, as Linux's head 0 (0x40508)
            case 0x2280: if (mode_.olut) vals[i] = 0x40508; break;
            case 0x2284: if (mode_.olut) vals[i] = 0xffffffff; break;
            case 0x2288: if (mode_.olut) vals[i] = kOlutCtxdma; break;
            case 0x228c: if (mode_.olut) vals[i] = 0; break;
            case 0x2240: if (mode_.olut) vals[i] = 1; break;
            // nvCscCoefConvertS514 encodes 1.0 as 0x10000 (S5.14
            // shifted by two register bits). Replace the entire 3x4
            // matrix: snapshot coefficients may include gamut/offset terms.
            case 0x2244: case 0x2258: case 0x226c: if (mode_.olut) vals[i] = 0x10000; break;
            case 0x2248: case 0x224c: case 0x2250: case 0x2254:
            case 0x225c: case 0x2260: case 0x2264: case 0x2268:
            case 0x2270: if (mode_.olut) vals[i] = 0; break;
            case 0x2030:   // HEAD_USAGE_BOUNDS.CURSOR
                if (mode_.cursor64) vals[i] = (vals[i] & ~7U) | 2;
                // 0.165.0: OLUT_ALLOWED (bit 4, clc67d.h). The OLUT test in
                // 0.141.1 never set it and the head refused the LUT (Xid 56,
                // check 0x2e); the head's usage bounds must allow what its
                // state uses.
                // Linux head 0: 0x1110 = OLUT_ALLOWED, UPSCALING_ALLOWED, TAPS_2
                if (mode_.olut) vals[i] |= (1U << 4) | (1U << 8) | (1U << 12);
                break;
            default: break;
            }
        }
    }
    // 0.138.2: never start tearing the head down on a busy core. A pending
    // core UPDATE (state 0x0c at 0x610630; idle is 0x0b, nouveau
    // gv100_disp_core_idle) made the detach land and the SOR assign fail,
    // which left the screen black (27 Sep). Try to release it first.
    // Only on a lit engine: after S3 the cached path runs on a reset core.
    if (!usedCache) {
        UInt32 cs = 0;
        peekBar0(0x610630, 1, &cs);
        if (((cs >> 16) & 0x1f) != 0x0b) {
            coreUnstick();
            peekBar0(0x610630, 1, &cs);
        }
        setProperty("NVGspControl-modeset-core-state", cs, 32);
        if (((cs >> 16) & 0x1f) != 0x0b) { code = 0x0C000000 | ((cs >> 16) & 0x1f); goto done; }
    }
    if (mode_.olut || mode_.composite)
        setProperty("NVGspControl-olut-setup", static_cast<UInt32>(olutSetup()), 32);
    if (mode_.composite)
        setProperty("NVGspControl-ilut-setup", static_cast<UInt32>(ilutSetup()), 32);
    wndComposite_ = mode_.valid && mode_.composite;
    // 2. manual DP mode (subDeviceInstance 0).
    {
        const UInt8 p[4] = {};
        const UInt32 st = displayCtrl(0x731365, p, sizeof(p));
        setProperty("NVGspControl-modeset-manual", st, 32);
        if (st) { code = 0x02000000 | st; goto done; }
    }
    // 3. detach the SOR (owner none) + UPDATE.
    {
        const UInt32 w[] = {(1U << 18) | 0x300, sor & ~0xfU,
                            (2U << 18) | 0x218, 0, 0,
                            (1U << 18) | 0x200, 1};
        const IOReturn r = submitCore(w, sizeof(w) / 4);
        setProperty("NVGspControl-modeset-detach", static_cast<UInt32>(r), 32);
        if (r != kIOReturnSuccess) { code = 0x03000000 | static_cast<UInt32>(r); goto done; }
    }
    IOSleep(500);
    // 4. DP link release (cmd 0x2003, data 0x0600 = RBR, lanes 0).
    {
        UInt8 p[28] = {};
        const UInt32 d = 0x200, c = 0x2003, dt = 0x0600;
        __builtin_memcpy(p + 4, &d, 4);
        __builtin_memcpy(p + 8, &c, 4);
        __builtin_memcpy(p + 12, &dt, 4);
        const UInt32 st = displayCtrl(0x731343, p, sizeof(p));
        setProperty("NVGspControl-modeset-release", st, 32);
        if (st) { code = 0x04000000 | st; goto done; }
    }
    // 5. assign display 0x200 to SOR0.
    {
        UInt8 p[80] = {};
        const UInt32 d = 0x200;
        __builtin_memcpy(p + 4, &d, 4);
        const UInt32 st = displayCtrl(0x731152, p, sizeof(p));
        setProperty("NVGspControl-modeset-assign", st, 32);
        if (st) { code = 0x05000000 | st; goto done; }
    }
    // 6. sink D0 (DPCD 0x600 = 1, same AUX write as DPMS-on).
    {
        const IOReturn r = dpSetPower(true);
        if (r != kIOReturnSuccess) { code = 0x06000000 | static_cast<UInt32>(r); goto done; }
    }
    // 7. train (cmd 0x2083, data 0x1404 = 4 lanes HBR2), up to 3 tries.
    {
        UInt32 st = ~0U;
        for (UInt32 t = 0; t < 3; ++t) {
            UInt8 p[28] = {};
            const UInt32 d = 0x200, c = 0x2083, dt = 0x1404;
            __builtin_memcpy(p + 4, &d, 4);
            __builtin_memcpy(p + 8, &c, 4);
            __builtin_memcpy(p + 12, &dt, 4);
            UInt8 echo[28] = {};
            st = displayCtrl(0x731343, p, sizeof(p), echo, sizeof(echo));
            UInt32 err = ~0U;
            __builtin_memcpy(&err, echo + 16, 4);
            setProperty("NVGspControl-modeset-train", st, 32);
            setProperty("NVGspControl-modeset-train-err", err, 32);
            if (st == 0 && err == 0) break;
            if (t == 2) { code = 0x07000000 | st; goto done; }
            IOSleep(200);
        }
    }
    // 8. SST stream config (dpLink 1, override 1, hblank 134, vblank 3835,
    // enhanced framing 1, TU 64, watermark 20 — nouveau watermark values).
    {
        UInt8 p[84] = {};
        // 0.137.0: hblank/vblank symbols follow the mode (dp_watermark.cpp
        // SST, HBR2); TU 64 / watermark 20 hold for every mode up to 4K60.
        const UInt32 one = 1, tu = 64, wm = 20;
        const UInt32 hb = mode_.valid ? mode_.hBlankSym : 134;
        const UInt32 vb = mode_.valid ? mode_.vBlankSym : 3835;
        __builtin_memcpy(p + 12, &one, 4);  // dpLink
        p[16] = 1;                          // bEnableOverride (NvBool)
        __builtin_memcpy(p + 24, &hb, 4);
        __builtin_memcpy(p + 28, &vb, 4);
        p[68] = 1;                          // SST.bEnhancedFraming
        __builtin_memcpy(p + 72, &tu, 4);
        __builtin_memcpy(p + 76, &wm, 4);
        const UInt32 st = displayCtrl(0x731362, p, sizeof(p));
        setProperty("NVGspControl-modeset-stream", st, 32);
        if (st) { code = 0x08000000 | st; goto done; }
    }
    // 0.137.2 (D4): GSP-RM here does not retune the head's VPLL during the
    // supervisor (216 Hz at 1080p on the 4K clock), so program VPLL0 like
    // the VBIOS does. Decoded from the VBIOS state and the Linux captures:
    // VCO = 27 MHz * (N + (fN + 4096) / 8192) / M, pclk = VCO / P, with
    // 0xef04 = 0x40|P << 16 | N << 8 | M and 0xef14[31:16] = fN (signed).
    // P is the smallest divider that puts the VCO at >= 1.4 GHz (what the
    // Linux captures show: P 3/5/6/10 for 4k60/4k30/1440p/1080p).
    if (mode_.valid) {
        const UInt64 pclk = mode_.pclkHz;
        UInt32 P = 1;
        while (P < 15 && pclk * P < 1400000000ULL) ++P;
        const UInt64 vco = pclk * P;
        // X * 8192 = vco * 8192 / 27 MHz; N = floor(X), fN = frac*8192 - 4096.
        const UInt64 x8192 = (vco * 8192 + 13500000) / 27000000;
        const UInt32 N = static_cast<UInt32>(x8192 / 8192);
        const SInt32 fN = static_cast<SInt32>(x8192 % 8192) - 4096;
        const UInt32 ef04 = ((0x40U | P) << 16) | (N << 8) | 1U;
        IOMemoryMap *vmap = sharedBar0Map(pci_);
        if (vmap && P < 15 && vco <= 1700000000ULL) {
            Bar0Io vio{vmap};
            UInt32 ef14 = 0;
            vio.read(0x00ef14, &ef14);
            ef14 = (ef14 & 0xffffU) | (static_cast<UInt32>(fN & 0xffff) << 16);
            vio.write(0x00ef14, ef14);
            vio.write(0x00ef04, ef04);
            vio.write(0x00e9c0, 1);
            IODelay(1000);
            UInt32 after = 0;
            vio.read(0x00ef04, &after);
            setProperty("NVGspControl-mode-vpll-ef04", after, 32);
            setProperty("NVGspControl-mode-vpll-ef14", ef14, 32);
        }
        if (vmap) vmap->release();
    }
    // 0.99.5: post-S3 (cached state): IMP first — cold boot inherits the
    // VBIOS-validated config, a fresh engine does not.
    // 0.137.1: a new mode goes through IMP as well (nvkms validates every
    // modeset with IS_MODE_POSSIBLE before the core UPDATE).
    if (usedCache || mode_.valid) setProperty("NVGspControl-modeset-imp", impCheckHead0(), 32);
    // 9. attach: SOR + all head methods + no interlocks + UPDATE, chunked
    // (submitCore takes <= 256 words per call; the PB stream continues).
    // 0.99.5: post-S3 also window 0 owner head 0 + VBIOS usage bounds
    // (WINDOW_SET_CONTROL(0) is 0xF/NONE on a reset engine ⇒ head SNOOZE).
    {
        // 0.165.0: a LUT mode also writes window 0's usage bounds (Linux:
        // ILUT_ALLOWED, TAPS_2, TMO_LUT_ALLOWED; captures disp-4k60.json)
        const bool lutBounds = !usedCache && mode_.valid && (mode_.olut || mode_.composite);
        const UInt32 extra = usedCache || lutBounds ? 4 : 0;
        const UInt32 total = 2 + kHeads * 2 + extra + 5;
        UInt32 *w = static_cast<UInt32 *>(IOMalloc(total * 4));
        if (!w) { code = 0x09000001; goto done; }
        w[0] = (1U << 18) | 0x300;
        w[1] = attachSor;
        for (UInt32 i = 0; i < kHeads; ++i) {
            w[2 + i * 2] = (1U << 18) | (kHeadAddrs[i] & 0x3ffc);
            w[3 + i * 2] = vals[i];
        }
        if (usedCache || lutBounds) {
            w[2 + kHeads * 2] = (1U << 18) | 0x1000;   // WINDOW_SET_CONTROL(0)
            w[3 + kHeads * 2] = 0;                     // OWNER HEAD0
            w[4 + kHeads * 2] = (1U << 18) | 0x1010;   // WINDOW_SET_WINDOW_USAGE_BOUNDS(0)
            w[5 + kHeads * 2] = lutBounds ? 0x10110f00 : 0x110f00;   // VBIOS value (+ TMO_LUT_ALLOWED)
        }
        w[total - 5] = (2U << 18) | 0x218;
        w[total - 4] = 0;
        w[total - 3] = mode_.valid ? 1 : 0;   // 0.137.0: interlock window 0
        if (mode_.valid) {
            lk(__LINE__);
            presentOwner_ = nullptr;
            presentHandle_ = 0;
            presentSurface_ = WindowSurface{};
            const IOReturn fr = flipWindowLocked(0, true, desktopSurface_, true);
            ulk();
            setProperty("NVGspControl-modeset-window", static_cast<UInt32>(fr), 32);
        }
        w[total - 2] = (1U << 18) | 0x200;
        w[total - 1] = 1;
        IOReturn r = kIOReturnSuccess;
        for (UInt32 off = 0; off < total && r == kIOReturnSuccess; off += 240) {
            const UInt32 n = (total - off > 240) ? 240 : (total - off);
            r = submitCore(w + off, n);
        }
        IOFree(w, total * 4);
        setProperty("NVGspControl-modeset-attach", static_cast<UInt32>(r), 32);
        if (r != kIOReturnSuccess) { code = 0x09000000 | static_cast<UInt32>(r); goto done; }
    }
    IOSleep(1000);
    // 0.138.0: don't leave window 0 interlocked with the core once the
    // mode's paired update has landed (later core UPDATEs would stall).
    if (mode_.valid) setProperty("NVGspControl-modeset-unlock", static_cast<UInt32>(coreUnstick()), 32);
    // 10. verify arming.
    peekBar0(0x612078, 1, &head0);
    peekBar0(0x688300, 1, &sor);
    setProperty("NVGspControl-modeset-head0-after", head0, 32);
    setProperty("NVGspControl-modeset-sor-after", sor, 32);
    {
        UInt32 dpcfg = 0;
        peekBar0(0x61c168, 1, &dpcfg);
        setProperty("NVGspControl-modeset-sor-dpcfg", dpcfg, 32);
    }
    // 0.99.5: post-S3, re-present window 0 (GOP/desktop surface at VRAM 0).
    if (usedCache) setProperty("NVGspControl-modeset-flip",
                               static_cast<UInt32>(flipWindow(0, true)), 32);
    if (head0 != 0x200 || sor != attachSor) code = 0x0A000000 | (head0 & 0xffffff);
done:
    IOFree(vals, kHeads * 4);
    *statusOut = code;
    setProperty("NVGspControl-modeset-result", code, 32);
    return code == 0 ? kIOReturnSuccess : kIOReturnIOError;
}

// 0.96.0: S3 sleep/wake interest. WillSleep quiesces and is acked (XNU
// delivers it via tellClientsWithResponse); CanSystemSleep is acked with no
// work (always sleepable, never veto); HasPoweredOn is one-way
// (tellClients) so it must NOT be acked — its messageArgument is not a
// refcon. Wake restores PCI bus master and clears sleeping_ so the
// daemon's 5 s drain sees phase 0 and re-boots the chain.
IOReturn NVGspControl::sleepWakeHandler(void *target, void *, UInt32 messageType,
                                        IOService *, void *messageArgument, vm_size_t) {
    auto *self = static_cast<NVGspControl *>(target);
    if (!self) return kIOReturnBadArgument;
    if (messageType == kIOMessageSystemWillSleep) {
        self->quiesceForSleep();
        acknowledgeSleepWakeNotification(messageArgument);
    } else if (messageType == kIOMessageCanSystemSleep) {
        acknowledgeSleepWakeNotification(messageArgument);
    } else if (messageType == kIOMessageSystemHasPoweredOn) {
        if (self->pci_ && (self->pci_->configRead16(kIOPCIConfigCommand) & 4) == 0)
            self->pci_->setBusMasterEnable(true);
        // 0.130.0: IOPCIFamily restored its own (256 MiB) BAR setup on wake
        if (self->rebar_.active) {
            IOService *p = self->pci_ ? self->pci_->getProvider() : nullptr;
            IOPCIDevice *br = OSDynamicCast(IOPCIDevice, p ? p->getProvider() : nullptr);
            if (br)
            {
                const Rebar &r = self->rebar_;
                const UInt64 wb = r.bar3Base < r.bar1Base ? r.bar3Base : r.bar1Base;
                const UInt64 we = (r.bar3Base + r.bar3Bytes > r.bar1Base + r.bar1Bytes
                                   ? r.bar3Base + r.bar3Bytes : r.bar1Base + r.bar1Bytes) - 1;
                self->rebarProgram(br, r.ctrl, r.bar1Base, r.bar3Base, wb, we);
            }
        }
        self->resumeFromSleep();
    }
    return kIOReturnSuccess;
}

// 0.98.0: pre-sleep quiesce (minimal — staging/queues/MSI stay resident so
// the wake fast path can re-light in seconds; S3 preserves DRAM). Sink off
// first (NVIDIA suspend order), then the sleeping_ fence (userRpc/ping fail
// fast until resume completes). The 0.96 full teardown survives only as the
// slow path inside resumeFromSleep (dead GSP).
// 0.100.8: SR is the default S3 path (proven 23:23); NVRAM nvgsp-sr=0 (or
// "off") disables it and falls back to the cold re-boot path.
// 0.113.0: NVRAM nvgsp-registry -> packed SET_REGISTRY table. False (send
// the empty table) when unset, empty or malformed beyond repair.
static bool registryFromNvram(UInt8 *out, UInt32 cap, UInt32 *bytes, UInt32 *entries) {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!options) return false;
    bool ok = false;
    OSObject *o = options->getProperty("nvgsp-registry");
    const char *spec = nullptr;
    UInt32 len = 0;
    if (OSData *d = OSDynamicCast(OSData, o)) {
        spec = static_cast<const char *>(d->getBytesNoCopy());
        len = d->getLength();
    } else if (OSString *str = OSDynamicCast(OSString, o)) {
        spec = str->getCStringNoCopy();
        len = str->getLength();
    }
    if (spec && len)
        ok = nvgsp::buildRegistry(spec, len, out, cap, bytes, entries) && *entries;
    options->release();
    return ok;
}

static bool srEnabledByNvram() {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    bool on = true;
    if (options) {
        OSObject *o = options->getProperty("nvgsp-sr");
        OSData *d = OSDynamicCast(OSData, o);
        OSString *str = OSDynamicCast(OSString, o);
        if (d && d->getLength() >= 1) {
            const char c = static_cast<const char *>(d->getBytesNoCopy())[0];
            on = !(c == '0' || c == 'o' || c == 0);
        } else if (str) {
            on = !(str->isEqualTo("0") || str->isEqualTo("off"));
        }
        options->release();
    }
    return on;
}

void NVGspControl::quiesceForSleep() {
    if (!lock_ || !pci_) return;
    const bool sr = srEnabledByNvram();
    // 0.127.0: clients wait from here until wake; with SR, save the kext-heap
    // VRAM (FBSR does not cover it) while the CE channel still runs.
    lk(__LINE__);
    vramFrozen_ = true;
    const bool saved = sr && saveVramLocked();
    ulk();
    setProperty("NVGspControl-sleep-vram-bytes", vramSavedBytes_, 64);
    setProperty("NVGspControl-sleep-vram-saved", saved);
    dpSetPower(false);
    // 0.100.0: NVIDIA-style GSP suspend (NVRAM nvgsp-sr gate until proven).
    if (sr) {
        const IOReturn r = srSuspend();
        setProperty("NVGspControl-sr-suspend-kr", static_cast<UInt32>(r), 32);
    }
    lk(__LINE__);
    sleeping_ = true;
    userRpcOutstanding_ = false;
    pingOutstanding_ = false;
    ulk();
    setProperty("NVGspControl-sleeping", true);
}

// 0.98.0: wake resume. Re-arm MSI on the stored vectors, unfence, then
// probe GSP with a self-driving RPC. Alive (the common case on this
// board — short sleeps keep firmware resident): modeset and done, display
// back in seconds, no WindowServer wedge. Dead: full reset and the daemon
// re-boots the chain (phase 0 + 0.97 trigger).
void NVGspControl::resumeFromSleep() {
    if (!lock_ || !pci_) return;
    lk(__LINE__);
    resetPrivateDiagnostic_ = false; // diagnostic applies only to the first global reset, never S3
    setProperty("NVGspControl-ownchannel-reset-diagnostic", false);
    if (gpuResets_) {
        setProperty("NVGspControl-ownchannel-reset-fallback", true);
        setProperty("NVGspControl-ownchannel-auto", false);
    }
    if (wl_ && irq_) wl_->removeEventSource(irq_);
    OSSafeReleaseNULL(irq_);
    OSSafeReleaseNULL(wl_);
    intrArmed_ = false;
    const bool rearmed = armInterrupts(vec_);
    setProperty("NVGspControl-resume-rearmed", rearmed);
    ulk();
    // 0.100.0: SR resume (GSP-RM restores its state); cold re-boot as fallback.
    if (srSuspended_) {
        const IOReturn r = srResume();
        setProperty("NVGspControl-sr-resume-kr", static_cast<UInt32>(r), 32);
        if (r == kIOReturnSuccess) {
            // 0.124.0: GSP-RM restored its own VRAM (FBSR), not the user heap
            // objects or the per-client arena tables: clients from before the
            // sleep get kIOReturnOffline (Vulkan: DEVICE_LOST) instead of
            // running on lost memory; their arenas are rebuilt on next use.
            // 0.127.0: unless the copy saved at sleep goes back first; then
            // the clients just continue (PD1 is rewritten on the next switch).
            lk(__LINE__);
            const bool restored = restoreVramLocked();
            if (restored) {
                arenaActive_ = nullptr;
                arenaInstalled_ = false;
            } else {
                resetPrivateDiagnostic_ = false;
                ++gpuResets_;
                arenaDropAllLocked(true);
            }
            freeSavedVramLocked();
            thawLocked();
            ulk();
            setProperty("NVGspControl-resume-vram-restored", restored);
            setProperty("NVGspControl-gpu-reset-count", gpuResets_, 32);
            if (!restored) {
                setProperty("NVGspControl-ownchannel-reset-diagnostic", false);
                setProperty("NVGspControl-ownchannel-reset-fallback", true);
                setProperty("NVGspControl-ownchannel-auto", false);
            }
            UInt32 code = ~0U;
            modesetHead0(&code);
            setProperty("NVGspControl-resume-fast", code == 0);
            setProperty("NVGspControl-resume-code", code, 32);
            return;
        }
        srSuspended_ = false;
    }
    lk(__LINE__);
    sleeping_ = false;
    ulk();
    setProperty("NVGspControl-sleeping", false);
    constexpr UInt32 kClientHandle = 0xc0d00001;
    constexpr UInt32 kSubdevice = 0xc0d02080;
    constexpr UInt32 kCmd = 0x20802068, kBytes = 4;  // PERF_GET_CURRENT_PSTATE
    UInt8 control[24 + 4]{};
    __builtin_memcpy(control, &kClientHandle, 4);
    __builtin_memcpy(control + 4, &kSubdevice, 4);
    __builtin_memcpy(control + 8, &kCmd, 4);
    __builtin_memcpy(control + 16, &kBytes, 4);
    UInt8 reply[256];
    UInt32 replyBytes = sizeof(reply), result = ~0U;
    const bool alive =
        userRpc(76, control, sizeof(control), reply, &replyBytes, &result) == kIOReturnSuccess;
    setProperty("NVGspControl-resume-gsp-alive", alive);
    if (alive) {
        // GSP stayed up: VRAM was never powered down, nothing to copy back
        lk(__LINE__);
        freeSavedVramLocked();
        thawLocked();
        ulk();
        UInt32 code = ~0U;
        modesetHead0(&code);
        setProperty("NVGspControl-resume-fast", code == 0);
        setProperty("NVGspControl-resume-code", code, 32);
        return;
    }
    setProperty("NVGspControl-resume-fast", false);
    // 0.124.0: cold re-boot after sleep = everything on the GPU is new, like
    // gpuReset(): bump the reset generation for pre-sleep clients.
    resetPrivateDiagnostic_ = false; // S3 never enables the global-reset diagnostic
    ++gpuResets_;
    setProperty("NVGspControl-gpu-reset-count", gpuResets_, 32);
    setProperty("NVGspControl-ownchannel-reset-diagnostic", false);
    setProperty("NVGspControl-ownchannel-reset-fallback", true);
    setProperty("NVGspControl-ownchannel-auto", false);
    lk(__LINE__);                               // 0.127.0: waiters see the new generation
    freeSavedVramLocked();
    thawLocked();
    ulk();
    resetForResume();
    // 0.99.0: self-drive the re-boot (async — the PM thread must return).
    // The daemon trigger stays as backup; stage/execute guards make a
    // concurrent daemon --boot fail gracefully without corrupting state.
    if (resumeCall_) thread_call_enter(resumeCall_);
}

// 0.99.0: slow-path chain driver — executeBoot on the RETAINED staging
// plus the daemon's poll loop, in-kernel. No file, no daemon, no trigger
// race: staging objects survive quiesce (S3 preserves DRAM), so the resume
// re-boot is identical to a cold boot's.
void NVGspControl::resumeDriveCallout(thread_call_param_t param, thread_call_param_t) {
    NVGspControl *self = static_cast<NVGspControl *>(param);
    if (!self || self->resumeAbort_) return;
    self->resumeActive_ = true;
    self->setProperty("NVGspControl-resume-drive", true);
    UInt32 polls = 0;
    // 0.99.2: re-stage init_ from scratch (fresh queues, rmargs, LibOS args and
    // log buffers — exactly the cold-boot state). The 0.99.0 snapshot restore
    // left the LibOS logs of the previous session in place and the resumed
    // GSP answered with UNLOADING_GUEST_DRIVER (47) and halted. Snapshot
    // restore stays as the fallback if re-staging fails.
    bool restaged = false;
    if (self->lock_ && self->systemInfoValid_) {
        self->lk(__LINE__);
        self->init_.reset();
        restaged = self->init_.stage(self->systemInfo_);
        self->ulk();
    }
    self->setProperty("NVGspControl-resume-restaged", restaged);
    const bool queuesOk = restaged ||
        (self->queueSnap_ && self->init_.restoreQueues(self->queueSnap_));
    self->setProperty("NVGspControl-resume-queues", queuesOk);
    if (queuesOk && self->executeBoot() == kIOReturnSuccess) {
        for (; polls < 900 && !self->resumeAbort_; ++polls) {
            self->pollStatus();
            UInt32 phase = 0;
            if (self->lock_) {
                self->lk(__LINE__);
                phase = self->postInitPhase_;
                self->ulk();
            }
            if (phase == 33 || phase == 32) break;
            IOSleep(200);
        }
    }
    self->setProperty("NVGspControl-resume-polls", polls, 32);
    UInt32 code = ~0U, phase = 0;
    if (self->lock_) {
        self->lk(__LINE__);
        phase = self->postInitPhase_;
        self->ulk();
    }
    if (!self->resumeAbort_ && phase == 33) {
        self->modesetHead0(&code);
        self->setProperty("NVGspControl-resume-code", code, 32);
    }
    self->setProperty("NVGspControl-resume-drive", false);
    // 0.145.0: once in a CTS night a reset came back at phase 33 with the
    // GR bring-up not done (gr-persistent No), and the GPU sat there until
    // someone ran nvrun --reset. Such a reset gets up to two more tries.
    bool retry = false;
    if (self->resetDriven_) {
        self->resetDriven_ = false;
        if (self->grPersistent_) {
            self->resetRetries_ = 0;
        } else if (!self->resumeAbort_ && self->autoResetCall_ && self->resetRetries_ < 2) {
            ++self->resetRetries_;
            retry = true;
        }
        self->setProperty("NVGspControl-reset-retries", self->resetRetries_, 32);
    }
    // 0.164.0: clients that reopened while the GPU was still coming back
    // (NVMTLDriver recovers on the first kIOReturnOffline) bound their pages
    // into tables resetForResume then dropped; WindowServer's next CE push ran
    // from a VA no client mapped (Xid 31 right after the reset, then the
    // reset after it failed). One more generation step now sends them
    // through the recovery again, into the new tables.
    // 0.176.0 (T3): the client objects back from the copy gpuReset took, before
    // resetBusy_ lets the clients in again; their tables are rebuilt as before
    if (self->lock_ && self->resetVramSaved_ && !retry) {
        self->kceEnsure();   // the copies run on the kernel CE channel, dropped by resetForResume
        self->lk(__LINE__);
        const bool kce = self->kce_.ready && !self->kce_.dead;
        UInt32 skipped = 0;
        const bool had = self->vramSaved_;
        const bool restored = self->grPersistent_ && kce && self->restoreVramLocked(&skipped);
        self->setProperty("NVGspControl-reset-vram-skipped", skipped, 32);
        self->setProperty("NVGspControl-reset-vram-had-copy", had);
        self->freeSavedVramLocked();
        self->resetVramSaved_ = false;
        self->ulk();
        self->setProperty("NVGspControl-reset-vram-restored", restored);
        self->setProperty("NVGspControl-reset-vram-kce", kce);
    }
    if (self->lock_) {
        self->lk(__LINE__);
        if (self->gpuResets_ != self->genSeenAtEnd_) {
            self->genSeenAtEnd_ = self->gpuResets_;
            ++self->genEnd_;
        }
        self->ulk();
        self->setProperty("NVGspControl-client-generation", self->resetGeneration(), 32);
    }
    if (self->lock_) {                                   // 0.170.0
        char e[160];
        snprintf(e, sizeof(e), "resume end phase %u gr-persistent %d retry %d", phase, self->grPersistent_ ? 1 : 0,
                 retry ? 1 : 0);
        self->lk(__LINE__);
        self->noteGpuEventLocked(e);
        self->ulk();
    }
    if (!retry) self->resetBusy_ = false;                // 0.164.0: clients may come back now
    if (!retry && self->grPersistent_)                   // 0.173.0
        for (UInt32 c = 0; c < 4; ++c)
            if (ResetDoneFn fn = self->resetDoneFn_[c]) fn(self->resetDoneRef_[c]);
    self->setProperty("NVGspControl-reset-busy", static_cast<bool>(self->resetBusy_));
    self->resumeActive_ = false;
    if (retry) {
        UInt64 deadline = 0;
        clock_interval_to_deadline(1, kSecondScale, &deadline);
        thread_call_enter_delayed(self->autoResetCall_, deadline);
    }
}

// 0.96.0: chain-state reset for S3 resume. Mirrors stop()'s member list
// (keep the two in sync) but leaves pci_/lock_/service attached and keeps
// the NVDisplay hotplug callback for re-arm at the next phase 33.
// 0.98.0: slow path only (dead GSP); MSI torn down too so the re-boot
// chain re-arms fresh (the wake re-arm targeted dead firmware).
void NVGspControl::resetForResume() {
    if (!lock_) return;
    lk(__LINE__);
    if (wl_ && irq_) wl_->removeEventSource(irq_);
    OSSafeReleaseNULL(irq_);
    OSSafeReleaseNULL(wl_);
    intrArmed_ = false;
    hotplugArmed_ = false;
    hotplugDeliver_ = false;
    hotplugPlug_ = hotplugUnplug_ = 0;
    vblanks_ = 0;
    // 0.99.0: staging SURVIVES (gsp_/booter_/init_ hold the parsed firmware;
    // S3 preserves DRAM) so the self-drive re-boots without any file.
    executed_ = false;
    statusSequence_ = 0;
    initDone_ = false;
    initResult_ = initPrivateResult_ = ~0U;
    postInitPhase_ = 0;
    internalClient_ = internalDevice_ = internalSubdevice_ = 0;
    virtualOffset_ = localMemoryOffset_ = pte4KAddress_ = originalPte_ = 0;
    gpfifoBackingOffset_ = userdBackingOffset_ = 0;
    instanceBackingOffset_ = methodBackingOffset_ = 0;
    gpfifoBackingSize_ = userdBackingSize_ = 0;
    instanceBackingSize_ = methodBackingSize_ = 0;
    methodBufferBytes_ = 0;
    errBackingOffset_ = errBackingSize_ = 0;
    channelGpFifoVa_ = 0;
    channelCid_ = 0;
    channelTsgHandle_ = 0;
    tsgHwId_ = 0;
    ceSizeStallPolls_ = 0;
    stallStartAbs_ = 0;
    kickToken_ = kickPolls_ = 0;
    kickTimeOk_ = kickRung_ = false;
    pbRung_ = false;
    pbPolls_ = 0;
    ctxBackingOffset_ = 0;
    ctxPtesInstalled_ = 0;
    pd0Address_ = 0;
    ctxHugeInstalled_ = 0;
    gr3dOk_ = false;
    lastGrOwner_ = nullptr;                  // 0.145.0
    rcEvents_ = mmuFaultEvents_ = otherEvents_ = 0;
    bar1Finished_ = false;
    bar1Live_ = false;                       // 0.123.0
    grPersistent_ = false;
    subPbOff_ = subSeq_ = 0;
    scratchOffset_ = 0;
    scratchHuge_ = 0;
    pdbAddress_ = 0;
    vramVaTables_ = 0;
    arenaDropAllLocked(true);                // 0.111.0: tables die with the VAS
    scratchTry_ = 0;
    scratchRetry_ = false;
    fbFreeBase_ = fbFreeLimit_ = 0;
    ceChunk_ = 0;
    ceMapped_ = ceStarted_ = cePersistent_ = false;
    ceToken_ = cePbOff_ = ceSeq_ = 0;
    ceOutstanding_ = 0;                      // 0.112.0
    ceUserdOffset_ = ceInstOffset_ = ceMthdOffset_ = 0;
    videoDropLocked();                       // 0.116.0
    clientChannelsDropLocked();              // 0.178.0
    if (!resetVramSaved_) freeSavedVramLocked();   // 0.127.0; 0.176.4: gpuReset's copy stays
    thawLocked();
    wndOwnsScreen_ = false;
    wndPut_ = 0;
    flips_ = 0;
    fbHugeInstalled_ = 0;
    grRung_ = false;
    grPolls_ = 0;
    channelBackingComplete_ = channelAllocOk_ = false;
    gpfifoPteInstalled_ = gpfifoPteRestored_ = false;
    dmaMapCompleted_ = false;
    bar2MapUnsupported_ = false;
    praminPteReadOk_ = false;
    hostPteMapped_ = hostPteValidated_ = hostPteRestored_ = false;
    pteMapInvalidateOk_ = pteRestoreInvalidateOk_ = false;
    gopSurfaceHashPre_ = 0;
    gopSurfaceHashPreOk_ = false;
    for (unsigned i = 0; i < 8; ++i) gopSurfaceBytesPre_[i] = 0;
    gopSurfaceHashMid_ = 0;
    gopSurfaceHashMidOk_ = false;
    bisectPoll_ = 0;
    auxStep_ = auxRetry_ = auxLastStatus_ = auxLastReply_ = 0;
    auxOk_ = false;
    dpmsCalls_ = 0;
    corePut_ = 0;
    coreInitWords_ = 2;
    cursorReady_ = false;
    cursorBuf_ = cursorControl_ = 0;
    cursorBase_ = 0;
    dispInstOffset_ = 0;
    // NOTE: init_/booter_/gsp_ intentionally NOT reset (0.99.0 staging
    // retention for the self-drive re-boot).
    ulk();
    setProperty("NVGspControl-resumed", true);
}

IOReturn NVGspControl::pollStatus() {
    if (!lock_) return kIOReturnNotReady;
    const uint64_t t0 = mach_absolute_time();
    lk(__LINE__);
    const uint64_t t1 = mach_absolute_time();
    const UInt32 phaseIn = postInitPhase_;
    if (phaseIn == 33) stampAdvanceLocked();   // 0.178.12: a missed fence interrupt cannot hold stamps
    profT0_ = t1; profFirst_ = profDrainEnd_ = 0; profRecords_ = profIdle_ = 0;
    // 0.100.6: run the chain back-to-back inside one call. With the GSP
    // doorbell, replies land in ~ms; a step that queued no RPC (local phase)
    // lets the next drain skip the 4 s empty wait. Stops on no progress, on
    // phase 32/33, or after a 3 s budget (lock_ is held).
    IOReturn ret = kIOReturnSuccess;
    uint64_t budget = 0;
    nanoseconds_to_absolutetime(3000ULL * 1000 * 1000, &budget);
    for (UInt32 step = 0; step < 64; ++step) {
        const UInt32 before = postInitPhase_;
        const uint32_t txBefore = init_.txSequence();
        ret = pollStatusLocked();
        if (postInitPhase_ != before && bootTlLen_ + 60 < sizeof(bootTl_)) {   // 0.146.7
            const UInt32 p = postInitPhase_;
            if (p == 1 || p == 34 || p == 100 || p == 120 || p == 200 || p == 207 || p == 208) {
                char t[12];
                snprintf(t, sizeof(t), "p%u", p);
                markBoot(t);
            }
        }
        const bool queued = init_.txSequence() != txBefore;
        drainNoWait_ = !queued;
        if (postInitPhase_ == 33 || postInitPhase_ == 32 || before == 33) break;
        if (postInitPhase_ == before && !queued) break;   // waiting on something
        if (mach_absolute_time() - t1 > budget) break;
        if (bar1EarlyPending_) {   // 0.146.6: console visible again ASAP
            bar1EarlyPending_ = false;
            finishBar1();
            bar1Early_ = true;
            markBoot("bar1-early");
        }
        // 0.146.2: let the interrupt handler (vblank, fences) in between
        // steps. Holding lock_ for the whole chain (up to 3 s at boot, phase
        // 1xx) froze the boot logo animation and delayed vblanks by seconds.
        // IOLock is not fair: re-taking it at once starves a waiting
        // interrupt handler (it still waited 1.15 s), so give it a moment.
        ulk();
        IODelay(50);
        lk(__LINE__);
    }
    if (bar1EarlyPending_) {   // 0.146.6 (the loop may have stopped first)
        bar1EarlyPending_ = false;
        finishBar1();
        bar1Early_ = true;
        markBoot("bar1-early");
    }
    const uint64_t t2 = mach_absolute_time();
    // 0.100.4: record chain polls (not the parked phase-33 drain).
    if (phaseIn != 33 || postInitPhase_ != 33) {
        auto us = [](uint64_t a, uint64_t b) -> UInt32 {
            if (!a || b < a) return 0;
            uint64_t ns = 0;
            absolutetime_to_nanoseconds(b - a, &ns);
            return static_cast<UInt32>(ns / 1000);
        };
        PollProf &e = pollProf_[pollProfCount_ % 160];
        e.phaseIn = static_cast<UInt16>(phaseIn);
        e.phaseOut = static_cast<UInt16>(postInitPhase_);
        e.records = static_cast<UInt16>(profRecords_);
        e.idle = static_cast<UInt16>(profIdle_);
        e.lockUs = us(t0, t1);
        e.firstUs = us(t1, profFirst_);
        e.drainUs = us(t1, profDrainEnd_);
        e.totalUs = us(t1, t2);
        ++pollProfCount_;
    }
    ulk();
    if (phaseIn != 33 && postInitPhase_ == 33) {
        OSData *d = OSData::withBytes(pollProf_, sizeof(pollProf_));
        if (d) { setProperty("NVGspControl-poll-prof", d); d->release(); }
        setProperty("NVGspControl-poll-prof-count", pollProfCount_, 32);
    }
    deliverHotplug();
    return ret;
}

// 0.89.0: hotplug. armHotplug() subscribes NV2080_NOTIFIERS_HOTPLUG the way
// nouveau r535_gsp_device_event_ctor does (NV01_EVENT_KERNEL_CALLBACK_EX 0x7E
// under the subdevice, notifyIndex NV01_EVENT_CLIENT_RM|1, then
// EVENT_SET_NOTIFICATION REPEAT). Events are delivered outside lock_.
IOReturn NVGspControl::armHotplug() {
    if (hotplugArmed_) return kIOReturnSuccess;
    UInt8 reply[256];
    UInt32 replyBytes = sizeof(reply), result = ~0U, st = ~0U;
    UInt8 alloc[32 + 24]{};
    const UInt32 a[8] = {0xc0d00001, 0xc0d02080, 0xc0d0e001, 0x7e, 0, 24, 0, 0};
    const UInt32 prm[4] = {0xc0d00001, 0, 0x7e, 0x04000000U | 1};
    __builtin_memcpy(alloc, a, sizeof(a));
    __builtin_memcpy(alloc + 32, prm, sizeof(prm));
    IOReturn r = userRpc(103, alloc, sizeof(alloc), reply, &replyBytes, &result);
    if (r == kIOReturnSuccess && replyBytes >= 100) __builtin_memcpy(&st, reply + 96, 4);
    setProperty("NVGspControl-hotplug-event-alloc", st, 32);
    if (st != 0 && st != 0x56) return kIOReturnIOError;
    UInt8 ctrl[24 + 20]{};
    const UInt32 hdr[6] = {0xc0d00001, 0xc0d02080, 0x20800301, 0, 20, 0};
    const UInt32 ev[2] = {1, 2};   // HOTPLUG, ACTION_REPEAT
    __builtin_memcpy(ctrl, hdr, sizeof(hdr));
    __builtin_memcpy(ctrl + 24, ev, sizeof(ev));
    replyBytes = sizeof(reply);
    st = ~0U;
    r = userRpc(76, ctrl, sizeof(ctrl), reply, &replyBytes, &result);
    if (r == kIOReturnSuccess && replyBytes >= 96) __builtin_memcpy(&st, reply + 92, 4);
    setProperty("NVGspControl-hotplug-notify-status", st, 32);
    hotplugArmed_ = st == 0;
    setProperty("NVGspControl-hotplug-armed", hotplugArmed_);
    return hotplugArmed_ ? kIOReturnSuccess : kIOReturnIOError;
}

void NVGspControl::deliverHotplug() {
    if (!lock_) return;
    lk(__LINE__);
    const bool deliver = hotplugDeliver_;
    const UInt32 plug = hotplugPlug_, unplug = hotplugUnplug_;
    hotplugDeliver_ = false;
    hotplugPlug_ = hotplugUnplug_ = 0;
    HotplugFn fn = hotplugFn_;
    void *ref = hotplugRef_;
    ulk();
    // 0.96.0: re-arm hotplug after an S3 resume (quiesce cleared it;
    // NVDisplay's one-shot register already ran at first boot). Runs on
    // every drain, not just on delivery, so a quiet resume still re-arms.
    if (postInitPhase_ == 33 && fn && !hotplugArmed_) armHotplug();
    if (!deliver) return;
    if (plug) dpReadEdid();           // refresh NVGspControl-edid before telling clients
    // 0.94.0: a replug leaves head0 armed but the DP link down (no picture
    // until retrain). If the lane status is not locked, run the modeset —
    // the DPCD gate doubles as debounce (link up => no-op, no blind blink).
    if (plug) {
        UInt8 lanes[2] = {};
        const bool locked = dpAuxRead(0x202, lanes, sizeof(lanes)) == kIOReturnSuccess &&
            lanes[0] == 0x77 && lanes[1] == 0x77;
        setProperty("NVGspControl-hotplug-lanes-ok", locked);
        if (!locked) {
            UInt32 code = ~0U;
            modesetHead0(&code);
            setProperty("NVGspControl-hotplug-modeset", code, 32);
        }
    }
    if (fn) fn(ref, plug, unplug);
}

// 0.94.0: native DPCD read (single AUX transaction, cmd 0x9 — the dpcd.py
// read path). Reply: status@92, bytes-done@140, reply-code@144, data@124.
IOReturn NVGspControl::dpAuxRead(UInt32 addr, UInt8 *out, UInt32 bytes) {
    if (!out || !bytes || bytes > 16) return kIOReturnBadArgument;
    UInt8 ctrl[24 + 48]{};
    const UInt32 hdr[6] = {0xc0d00001, 0xc0d00073, 0x731341, 0, 48, 0};
    __builtin_memcpy(ctrl, hdr, sizeof(hdr));
    UInt8 *pp = ctrl + 24;
    const UInt32 displayId = 0x200, cmd = 0x9, size = bytes - 1;
    __builtin_memcpy(pp + 4, &displayId, 4);
    __builtin_memcpy(pp + 12, &cmd, 4);
    __builtin_memcpy(pp + 16, &addr, 4);
    __builtin_memcpy(pp + 36, &size, 4);
    UInt8 reply[256];
    for (UInt32 retry = 0; retry < 5; ++retry) {
        UInt32 replyBytes = sizeof(reply), result = ~0U, st = ~0U, done = 0, rep = ~0U;
        if (userRpc(76, ctrl, sizeof(ctrl), reply, &replyBytes, &result) == kIOReturnSuccess &&
            replyBytes >= 148) {
            __builtin_memcpy(&st, reply + 92, 4);
            __builtin_memcpy(&done, reply + 140, 4);
            __builtin_memcpy(&rep, reply + 144, 4);
        }
        if (st == 0 && rep == 0) {
            if (done > bytes) done = bytes;
            __builtin_memcpy(out, reply + 124, done);
            return done == bytes ? kIOReturnSuccess : kIOReturnUnderrun;
        }
        IOSleep(5);
    }
    return kIOReturnIOError;
}

// 0.89.0: runtime EDID re-read over DP AUX (same transaction list as chain
// phase 224: I2C-over-AUX @0x50, offset write + 16 x 16 B reads).
IOReturn NVGspControl::dpReadEdid() {
    UInt8 edid[256]{};
    UInt32 got = 0;
    UInt8 reply[256];
    for (UInt32 step = 3; step < 20; ++step) {
        UInt8 ctrl[24 + 48]{};
        const UInt32 hdr[6] = {0xc0d00001, 0xc0d00073, 0x731341, 0, 48, 0};
        __builtin_memcpy(ctrl, hdr, sizeof(hdr));
        UInt8 *pp = ctrl + 24;
        const UInt32 displayId = 0x200, addr = 0x50;
        const UInt32 cmd = step == 3 ? 0x4 : (step < 19 ? 0x5 : 0x1);
        const UInt32 size = step == 3 ? 0 : 15;
        __builtin_memcpy(pp + 4, &displayId, 4);
        __builtin_memcpy(pp + 12, &cmd, 4);
        __builtin_memcpy(pp + 16, &addr, 4);
        __builtin_memcpy(pp + 36, &size, 4);
        bool ok = false;
        for (UInt32 retry = 0; retry < 5 && !ok; ++retry) {
            UInt32 replyBytes = sizeof(reply), result = ~0U, st = ~0U, done = 0, rep = ~0U;
            if (userRpc(76, ctrl, sizeof(ctrl), reply, &replyBytes, &result) == kIOReturnSuccess &&
                replyBytes >= 148) {
                __builtin_memcpy(&st, reply + 92, 4);
                __builtin_memcpy(&done, reply + 140, 4);
                __builtin_memcpy(&rep, reply + 144, 4);
            }
            ok = st == 0 && rep == 0;
            if (ok && step >= 4) {
                if (done > 16) done = 16;
                __builtin_memcpy(edid + (step - 4) * 16, reply + 124, done);
                got = (step - 4) * 16 + done;
            }
            if (!ok) IOSleep(2);
        }
        if (!ok) { if (step >= 12) break; setProperty("NVGspControl-edid-reread", false); return kIOReturnIOError; }
    }
    if (got >= 128) setProperty("NVGspControl-edid", edid, got);
    setProperty("NVGspControl-edid-reread", got >= 128);
    return got >= 128 ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn NVGspControl::flipWindow(UInt64 offsetBytes, bool fullInit) {
    if (!lock_ || !pci_) return kIOReturnNotReady;
    if (offsetBytes & 0xff) return kIOReturnBadArgument;
    lk(__LINE__);
    const IOReturn ret = flipWindowLocked(offsetBytes, fullInit, desktopSurface_);
    // 0.110.0: the window no longer shows a Vulkan presenter's surface (and
    // may carry desktop state), so the next present programs everything.
    presentOwner_ = nullptr;
    presentHandle_ = 0;
    presentSurface_ = WindowSurface{};
    ulk();
    return ret;
}

// C++14: odr-used (bound to const references), so it needs a definition.
constexpr NVGspControl::WindowSurface NVGspControl::kDesktopSurface;

// 0.110.0: window 0 flip with explicit surface state (NVK/Vulkan present).
// Full init programs SIZE/STORAGE/PARAMS/PLANAR_STORAGE/SIZE_IN/SIZE_OUT
// from `surf` (clc67e.h); the ISO ctxdma is pitch-kind VRAM [0, 16 GiB).
// Caller holds lock_.
IOReturn NVGspControl::flipWindowLocked(UInt64 offsetBytes, bool fullInit,
                                        const WindowSurface &surf, bool interlockCore) {
    if (offsetBytes & 0xff) return kIOReturnBadArgument;
    IOReturn ret = kIOReturnNotReady;
    // OFFSET(0), interlock flags (none), UPDATE(1): 6 dwords, 24 bytes.
    // 0.102.0: PRESENT_CONTROL MIN_PRESENT_INTERVAL 1 (non-tearing, at most
    // one flip per vblank; was 0 -> 292 flips/s queued).
    const UInt32 pbFlip[] = {
        (1U << 18) | 0x308, 0x1,
        (1U << 18) | 0x260, static_cast<UInt32>(offsetBytes >> 8),
        (2U << 18) | 0x370, 0x0, 0x0,
        (1U << 18) | 0x200 };
    // 0.100.1: full window state (the chain's 0.55.0 init) — after GSP SR the
    // window's surface state is back at defaults and an OFFSET-only flip
    // scans an empty surface (signal, black screen).
    const UInt32 wh = surf.width | (surf.height << 16);
    const UInt32 pbFull[] = {
        (1U << 18) | 0x308, 0x1,
        (4U << 18) | 0x224, wh, 0x0, surf.format, surf.pitch >> 6,
        (1U << 18) | 0x240, 0xc0d0d001,
        (1U << 18) | 0x260, static_cast<UInt32>(offsetBytes >> 8),
        (1U << 18) | 0x290, 0x0,
        (1U << 18) | 0x298, wh,
        (1U << 18) | 0x2A4, wh,
        // 0.141.1: composition stays BYPASS (the reset default, 0x10000).
        // Normal composition (depth 8, src*1 + dst*0, the Linux window) is
        // experiment bit 0x100 only: on its own the window UPDATE is refused
        // (Xid 56 chid 1 check 0x2d) and the head goes dark. Bypass is the
        // suspect for the cursor (0x0e) and OLUT (0x41) refusals.
        (1U << 18) | 0x2EC, wndComposite_ ? 0x80U : 0x10000U,
        (1U << 18) | 0x2F4, wndComposite_ ? 0x11U : 0U,
        // 0.142.0: the Linux window pairs composition with an input LUT
        // (DIRECT10, 1029 entries, same identity table as the OLUT).
        (1U << 18) | 0x440, wndComposite_ ? 0x40508U : 0U,
        (1U << 18) | 0x444, wndComposite_ ? kIlutCtxdma : 0U,
        (1U << 18) | 0x448, 0,
        (2U << 18) | 0x370, interlockCore ? 1U : 0U, 0x0,
        (1U << 18) | 0x200 };
    // 0.137.0: with interlockCore the window UPDATE waits for the next core
    // UPDATE, so a new SIZE and a new raster land in the same frame.
    const UInt32 *pb = fullInit ? pbFull : pbFlip;
    const UInt32 pbWords = fullInit ? sizeof(pbFull) / 4 : sizeof(pbFlip) / 4;
    const UInt32 tail[] = {0x1};
    const UInt32 kBytes = (pbWords + 1) * 4;
    if (wndOwnsScreen_ && dispInstOffset_) {
        // 0.79.0: wrap the 4 KiB window PB with a DMA JUMP (opcode 1,
        // clc67e.h) instead of failing after ~146 flips: JUMP 0 at the
        // current PUT (engine is idle there: GET == PUT), methods at 0.
        UInt32 base = wndPut_;
        bool wrapped = false;
        if (base + kBytes + 4 > 4096) {
            const UInt32 jump[1] = {0x20000000U};
            wrapped = praminWriteWords(pci_, dispInstOffset_ + 0x8000 + base,
                                       jump, 1);
            base = 0;
        }
        ret = kIOReturnNoSpace;
        if (base + kBytes + 4 <= 4096 && (base == wndPut_ || wrapped)) {
            ret = kIOReturnIOError;
            if (praminWritePattern(pci_, dispInstOffset_ + 0x8000 + base,
                                   pb, pbWords, 0, 0, tail, 1)) {
                IOMemoryMap *map = sharedBar0Map(pci_);
                if (map && map->getLength() >= 0x00690008) {
                    Bar0Io bar0{map};
                    const UInt32 put = base + kBytes;
                    UInt32 get = 0;
                    bar0.write(0x00690000, put);
                    for (UInt32 i = 0; i < 4000; ++i) {
                        if (bar0.read(0x00690004, &get) && get == put) {
                            wndPut_ = put;
                            ++flips_;
                            ret = kIOReturnSuccess;
                            break;
                        }
                        IODelay(50);
                    }
                    setProperty("NVGspControl-flip-get", get, 32);
                }
                if (map) map->release();
            }
        }
    }
    setProperty("NVGspControl-flip-last-offset", offsetBytes, 64);
    setProperty("NVGspControl-flip-count", flips_, 32);
    setProperty("NVGspControl-flip-last-result", static_cast<UInt32>(ret), 32);
    return ret;
}

// 0.110.0: scan out a client's VRAM memory object on window 0 (zero-copy
// Vulkan present). The first flip of a presentation (or any surface change)
// programs the full window state; later flips only move OFFSET. The window
// channel throttles to one flip per vblank, so when this returns the
// previous presented surface has been latched.
IOReturn NVGspControl::presentObject(const void *owner, UInt32 handle, UInt64 offset,
                                     UInt32 pitch, UInt32 width, UInt32 height,
                                     UInt32 format) {
    if (!lock_ || !pci_ || !owner || !handle || handle > kMaxMem) return kIOReturnBadArgument;
    if (!nvgsp::windowSurfaceValid(pitch, width, height, format, desktopSurface_.width,
                                   desktopSurface_.height))
        return kIOReturnBadArgument;
    lk(__LINE__);
    GpuMem &m = mem_[handle - 1];
    IOReturn ret = kIOReturnBadArgument;
    if (m.owner == owner) m.presented = true;   // 0.149.0: scanout stays in VRAM
    if (m.owner == owner && m.domain == 0 && !(offset & 0xff) && offset < m.bytes &&
        UInt64(pitch) * (height - 1) + UInt64(width) * 4 <= m.bytes - offset) {
        const WindowSurface surf{pitch, width, height, format};
        const bool full = presentOwner_ != owner || !(surf == presentSurface_);
        ret = flipWindowLocked(m.phys + offset, full, surf);
        if (ret == kIOReturnSuccess) {
            presentOwner_ = owner;
            presentHandle_ = handle;
            presentSurface_ = surf;
            ++presents_;
        }
    }
    ulk();
    setProperty("NVGspControl-present-count", presents_, 32);
    setProperty("NVGspControl-present-last-result", static_cast<UInt32>(ret), 32);
    return ret;
}

// 0.110.0: give the screen back to the desktop (GOP/WindowServer surface at
// VRAM 0) if `owner` (nullptr = anyone) is presenting. Caller holds lock_.
IOReturn NVGspControl::presentStopLocked(const void *owner) {
    if (!presentOwner_ || (owner && presentOwner_ != owner)) return kIOReturnSuccess;
    presentOwner_ = nullptr;
    presentHandle_ = 0;
    presentSurface_ = WindowSurface{};
    return flipWindowLocked(0, true, desktopSurface_);
}

IOReturn NVGspControl::presentStop(const void *owner) {
    if (!lock_ || !owner) return kIOReturnBadArgument;
    lk(__LINE__);
    const IOReturn ret = presentStopLocked(owner);
    ulk();
    return ret;
}

// 0.103.0: fence semaphore read (PRAMIN window saved/restored). Caller holds lock_.
bool NVGspControl::readRingSem(const Ring &ring, UInt32 *value) {
    if (volatile UInt32 *p = bar1Ptr(ring.semPhys, 4)) { *value = *p; return true; }   // 0.151.0
    IOMemoryMap *map = sharedBar0Map(pci_);
    bool ok = false;
    if (map && map->getLength() >= 0x00710000) {
        Bar0Io bar0{map};
        UInt32 before = 0;
        ok = bar0.read(0x1700, &before) &&
            bar0.write(0x1700, (before & 0xfc000000U) |
                       static_cast<UInt32>((ring.semPhys >> 16) & 0xffffff)) &&
            bar0.read(0x00700000 + static_cast<UInt32>(ring.semPhys & 0xffff), value);
        bar0.write(0x1700, before);
    }
    if (map) map->release();
    return ok;
}

// 0.118.0 (B7): fence wait that sleeps on the engine's non-stall interrupt
// (every submission tail ends in NON_STALL_INTERRUPT) instead of spinning
// for up to the whole timeout. A short spin covers small jobs; then 1 ms
// sleep slices (a lost or misrouted interrupt only costs latency). lock_ is
// dropped while asleep, so the ring is looked up again every round.
// 0.119.0: GR (0) and CE (1). Caller holds lock_.
bool NVGspControl::waitFenceSleep(UInt32 engine, UInt32 seq, UInt32 timeoutUs,
                                  const void *owner) {
    const UInt32 k = engine ? kVecCeNs : kVecGrNs;
    Ring ring{};
    if (!(engine ? ceRing(&ring) : grRingFor(owner, &ring))) return false;
    if (waitRingSem(ring, seq, timeoutUs < 50 ? timeoutUs : 50)) return true;
    UInt64 start = 0;
    clock_get_uptime(&start);
    // 0.146.0: most kernels finish within a few hundred us, but the GR
    // non-stall interrupt only woke ~13 % of the sleeps (the rest ran into
    // the 1 ms deadline, ~3.4 ms with timer coalescing), so every small
    // Metal dispatch cost 3.4 ms. Poll a little longer first, dropping
    // lock_ between polls so other clients can still submit.
    for (UInt32 us = 0; us < 300 && us < timeoutUs; us += 5) {
        ulk();
        IODelay(5);
        lk(__LINE__);
        if (!(engine ? ceRing(&ring) : grRingFor(owner, &ring))) return false;
        UInt32 v = 0;
        if (readRingSem(ring, &v) && static_cast<SInt32>(v - seq) >= 0) return true;
    }
    for (;;) {
        if (!(engine ? ceRing(&ring) : grRingFor(owner, &ring))) return false;
        UInt32 v = 0;
        if (readRingSem(ring, &v) && static_cast<SInt32>(v - seq) >= 0) return true;
        UInt64 now = 0, ns = 0;
        clock_get_uptime(&now);
        absolutetime_to_nanoseconds(now - start, &ns);
        if (ns >= UInt64(timeoutUs) * 1000) return false;
        UInt64 deadline = 0;
        clock_interval_to_deadline(200, kMicrosecondScale, &deadline);   // 0.146.0: was 1 ms
        const UInt32 before = vecCount_[k];
        ++fenceSleeps_;
        sleepLk(&vecCount_[k], deadline, THREAD_UNINT);
        if (vecCount_[k] != before) ++fenceWakes_;
    }
}

// Caller holds lock_. True once the fence reached seq (wrap-safe compare).
bool NVGspControl::waitRingSem(const Ring &ring, UInt32 seq, UInt32 timeoutUs) {
    const UInt32 asked = timeoutUs;
    timeoutUs = stallCapUs(ring, timeoutUs);                    // 0.164.0
    for (UInt32 i = 0; i <= timeoutUs; ++i) {
        UInt32 v = 0;
        if (readRingSem(ring, &v) && static_cast<SInt32>(v - seq) >= 0) {
            if (asked >= 20000) noteRingWaitLocked(ring, true);
            return true;
        }
        if (i > 64) IODelay(1);
    }
    if (timeoutUs >= 100000) captureStallLocked(ring, seq, 1);   // 0.156.0
    if (asked >= 20000) noteRingWaitLocked(ring, false);
    return false;
}

// 0.164.0: a reset brings VRAM back from the suspend copy, the fence
// semaphores included, while subSeq_/ceSeq_ start again at 0: every wait then
// passed at once (wrap-safe compare) and the first kernels after a reset
// "finished" before they ran (metal_fault_test good: 4096 wrong, wait 3 us).
// The counters go on from what the semaphores hold. Caller holds lock_.
void NVGspControl::syncRingSeqLocked(bool gr, bool ce) {
    Ring r{};
    UInt32 v = 0;
    if (gr && grRing(&r) && readRingSem(r, &v)) { subSeq_ = v; setProperty("NVGspControl-gr-seq-start", v, 32); }
    if (ce && ceRing(&r) && readRingSem(r, &v)) { ceSeq_ = v; setProperty("NVGspControl-ce-seq-start", v, 32); }
}

NVGspControl::RingStall *NVGspControl::stallFor(const Ring &ring) {
    return ring.seq == &subSeq_ ? &grStall_ : ring.seq == &ceSeq_ ? &ceStall_ : nullptr;
}

// Caller holds lock_. The wait a caller may spend on this ring.
UInt32 NVGspControl::stallCapUs(const Ring &ring, UInt32 timeoutUs) {
    RingStall *st = stallFor(ring);
    if (!st || !st->stalled || timeoutUs <= 20000) return timeoutUs;
    UInt32 v = 0;
    if (st->hung || (readRingSem(ring, &v) && v == st->sem)) return 20000;
    return timeoutUs;
}

// Caller holds lock_. After a wait of 20 ms or more on the ring.
void NVGspControl::noteRingWaitLocked(const Ring &ring, bool reached) {
    RingStall *st = stallFor(ring);
    if (!st) return;
    UInt32 v = 0;
    readRingSem(ring, &v);
    if (reached) {
        if (!st->stalled || v != st->sem) st->stalled = false;   // moved: healthy again
        return;
    }
    UInt64 now = 0, ns = 0;
    clock_get_uptime(&now);
    if (!st->stalled || v != st->sem) { st->stalled = true; st->sem = v; st->since = now; return; }
    absolutetime_to_nanoseconds(now - st->since, &ns);
    if (!st->hung && ns >= 5000000000ULL) ringHungLocked(st);
}

// Caller holds lock_. GR: the channel is given up like after an RC; CE:
// ceRing() stops handing the ring out. Both come back with the reset.
void NVGspControl::ringHungLocked(RingStall *st) {
    st->stalled = st->hung = true;
    {   // 0.170.0
        char e[160];
        snprintf(e, sizeof(e), "%s ring hung, sem %u, arena pid %d", st == &grStall_ ? "GR" : "CE", st->sem,
                 arenaActive_ ? arenaActive_->pid : -1);
        noteGpuEventLocked(e);
    }
    if (st == &grStall_) {
        grPersistent_ = false;
        setProperty("NVGspControl-gr-persistent", false);
        setProperty("NVGspControl-gr-hung", true);
    } else {
        setProperty("NVGspControl-ce-hung", true);
    }
    scheduleAutoResetLocked();
}

// 0.156.0: a ring that stops moving without any GR exception (native path,
// user session, 28 Sep 20:02 and 20:08). Snapshot everything we can read
// about it once per 5 s: ring semaphore vs the wanted value, USERD GP_GET /
// GP_PUT and the GPFIFO entry at GET, PGRAPH/FECS/MMU-fault registers,
// the active VA arena and the last GR owner. Caller holds lock_.
void NVGspControl::captureStallLocked(const Ring &ring, UInt32 want, UInt32 where) {
    UInt64 now = 0, ns = 0;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now - lastStallAbs_, &ns);
    if (lastStallAbs_ && ns < 5000000000ULL) return;
    lastStallAbs_ = now;
    UInt32 d[40] = {};
    UInt32 n = 0;
    UInt64 abs = 0;
    absolutetime_to_nanoseconds(now, &abs);
    d[n++] = where; d[n++] = static_cast<UInt32>(abs / 1000000);   // ms since boot
    UInt32 sem = 0, get = 0, put = 0;
    readRingSem(ring, &sem);
    ringRead(ring.userdPhys + 0x88, &get);
    ringRead(ring.userdPhys + 0x8c, &put);
    d[n++] = want; d[n++] = sem; d[n++] = *ring.seq; d[n++] = get; d[n++] = put;
    // 0.157.0: the two entries before GET, the ones the host fetched last
    // (the stuck one is among them), not the stale slot at GET
    UInt32 e[4] = {};
    ringRead(ring.gpfifoPhys + ((get - 1) & 0x1ff) * 8, &e[0]);
    ringRead(ring.gpfifoPhys + ((get - 1) & 0x1ff) * 8 + 4, &e[1]);
    ringRead(ring.gpfifoPhys + ((get - 2) & 0x1ff) * 8, &e[2]);
    ringRead(ring.gpfifoPhys + ((get - 2) & 0x1ff) * 8 + 4, &e[3]);
    d[n++] = e[0]; d[n++] = e[1]; d[n++] = e[2]; d[n++] = e[3];
    UInt32 g[kGrDiagCount] = {};
    readGrDiag(pci_, g);
    for (UInt32 i = 0; i < kGrDiagCount; ++i) d[n++] = g[i];
    d[n++] = static_cast<UInt32>(reinterpret_cast<uintptr_t>(arenaActive_));
    d[n++] = static_cast<UInt32>(reinterpret_cast<uintptr_t>(lastGrOwner_));
    d[n++] = arenaSwitches_;
    d[n++] = asyncOutstanding_;
    d[n++] = mmuFaultEvents_;                                       // 0.158.0
    ++stalls_;
    setProperty("NVGspControl-stall", d, n * 4);
    setProperty("NVGspControl-stall-count", stalls_, 32);
    IOLog("NVGspControl: ring stall %u: want %u sem %u seq %u get %u put %u entry %08x %08x pgraph %08x fecs %08x/%08x arena %p\n",
          where, want, sem, *ring.seq, get, put, e[0], e[1], g[6], g[9], g[10], arenaActive_);
}

IOReturn NVGspControl::submitRing(const Ring &ring, const UInt32 *words,
                                  UInt32 count, UInt64 *nsOut, bool async) {
    // Caller holds lock_. Appends a host SEM_EXECUTE release (WFI) of the
    // next sequence number, writes the GP entry at USERD PUT, publishes
    // PUT (dword only, never GET), rings the doorbell, polls the semaphore.
    // 0.74.0: + NON_STALL_INTERRUPT (0x20) after the release -> engine
    // non-stall vector fires when the fence lands.
    const UInt32 bytes = (count + 8) * 4;
    if (bytes > ring.pbBytes) return kIOReturnNoSpace;
    // 0.103.0: async submissions may still be in flight. Before reusing PB
    // space (wrap) or when many GP entries are outstanding, drain the ring.
    // 0.149.0: the outstanding count is the GR ring's; other rings only wrap
    const bool gr = ring.pbOff == &subPbOff_;
    if (*ring.pbOff + bytes > ring.pbBytes || (gr && asyncOutstanding_ > 384)) {
        if (!waitRingSem(ring, *ring.seq, 2000000)) return kIOReturnTimeout;
        if (gr) asyncOutstanding_ = 0;
    }
    if (*ring.pbOff + bytes > ring.pbBytes) *ring.pbOff = 0;
    const UInt32 seq = ++*ring.seq;
    const UInt32 tail[8] = {
        0x20050017, static_cast<UInt32>(ring.semVa & 0xfffffffcULL),
        static_cast<UInt32>((ring.semVa >> 32) & 0xff), seq, 0, 0x00100001,
        0x20010008, 0};
    const UInt64 pbPhys = ring.pbPhys + *ring.pbOff;
    const UInt64 pbVa = ring.pbVa + *ring.pbOff;
    UInt64 getPut = 0;
    PraminPteResult r{};
    bool ok = praminWriteWords(pci_, pbPhys, words, count) &&
        praminWriteWords(pci_, pbPhys + UInt64(count) * 4, tail, 8) &&
        praminPteAccess(pci_, ring.userdPhys + 0x88, &getPut, false, false,
                        &r);
    const UInt32 put = static_cast<UInt32>(getPut >> 32) & 0x1ff;
    const UInt32 entry[2] = {
        static_cast<UInt32>(pbVa & 0xfffffffcULL),
        static_cast<UInt32>(((pbVa >> 32) & 0xff) |
                            (UInt64(count + 8) << 10))};
    const UInt32 newPut = (put + 1) & 0x1ff;
    ok = ok && praminWriteWords(pci_, ring.gpfifoPhys + put * 8, entry, 2) &&
        praminWriteWords(pci_, ring.userdPhys + 0x8c, &newPut, 1);
    if (!ok) return kIOReturnIOError;
    IOReturn ret = kIOReturnIOError;
    IOMemoryMap *map = sharedBar0Map(pci_);
    if (map && map->getLength() >= 0x00BB0094) {
        Bar0Io bar0{map};
        UInt64 t0 = 0, t1 = 0;
        UInt32 before = 0, v = 0;
        bar0.read(0x1700, &before);
        bar0.write(0x1700, (before & 0xfc000000U) |
                   static_cast<UInt32>((ring.semPhys >> 16) & 0xffffff));
        const UInt32 at = 0x00700000 +
            static_cast<UInt32>(ring.semPhys & 0xffff);
        clock_get_uptime(&t0);
        bar0.write(0x00BB0090, ring.token);
        ret = kIOReturnTimeout;
        if (async) {                                    // 0.103.0
            bar0.write(0x1700, before);
            map->release();
            *ring.pbOff += (bytes + 255) & ~255U;
            ++asyncOutstanding_;
            *nsOut = 0;
            return kIOReturnSuccess;
        }
        // 0.75.0: with the GR non-stall vector armed, sleep until the fence
        // interrupt (NON_STALL_INTERRUPT after the release) instead of
        // spinning; IOLockSleepDeadline drops lock_ so the MSI handler runs.
        // The spin below stays as the completion check / fallback.
        if (intrArmed_ && vec_[kVecGrNs] != ~0U && !inIntr_ &&
            ring.pbOff == &subPbOff_) {  // GR ring only
            UInt64 deadline = 0;
            clock_interval_to_deadline(100, kMillisecondScale, &deadline);
            for (UInt32 w = 0; w < 4; ++w) {
                if (bar0.read(at, &v) && v == seq) break;
                const UInt32 before = vecCount_[kVecGrNs];
                sleepLk(&vecCount_[kVecGrNs], deadline,
                                    THREAD_UNINT);
                ++fenceSleeps_;
                if (vecCount_[kVecGrNs] != before) ++fenceWakes_;
            }
        }
        const UInt32 spins = stallCapUs(ring, 4000000);         // 0.164.0
        for (UInt32 i = 0; i < spins; ++i) {
            if (bar0.read(at, &v) && v == seq) {
                clock_get_uptime(&t1);
                ret = kIOReturnSuccess;
                break;
            }
            if (i > 2000) IODelay(1);
        }
        bar0.write(0x1700, before);
        noteRingWaitLocked(ring, ret == kIOReturnSuccess);
        if (ret == kIOReturnSuccess) {
            UInt64 ns = 0;
            absolutetime_to_nanoseconds(t1 - t0, &ns);
            *nsOut = ns;
        }
    }
    if (map) map->release();
    *ring.pbOff += (bytes + 255) & ~255U;
    return ret;
}

IOReturn NVGspControl::submitGr(const void *owner, const UInt32 *words, UInt32 count,
                                UInt64 *nsOut) {
    if (nvramBoostPinned_ == 0) noteGpuBusy();          // 0.135.0
    if (!lock_ || !pci_ || !words || !count || !nsOut)
        return kIOReturnBadArgument;
    lk(__LINE__);
    if (!waitChannelMutationLocked()) { ulk(); return kIOReturnNotReady; }
    IOReturn ret = kIOReturnNotReady;
    // GR PB ring: ctx block spare +0x2990000 (384 KiB), sem +0x29FF000.
    ClientChannel *own = clientChannelLocked(owner);   // 0.178.0
    Ring ownRing{};
    if (own && clientRingLocked(own, &ownRing) &&
        (ret = clientArenaLocked(own)) == kIOReturnSuccess && clientRingLocked(own, &ownRing)) {
        ret = submitRing(ownRing, words, count, nsOut);
    } else if (own) {
        if (ret == kIOReturnSuccess) ret = kIOReturnNotReady;
    } else if (grPersistent_ && ctxBackingOffset_ &&
        (ret = arenaSwitchLocked(owner)) == kIOReturnSuccess) {   // 0.111.0
        noteGrOwnerLocked(owner);
        const Ring ring{ctxBackingOffset_ + 0x2990000,
                        0x104000000ULL + 0x2990000, 0x60000,
                        ctxBackingOffset_ + 0x29FF000,
                        0x104000000ULL + 0x29FF000, gpfifoBackingOffset_,
                        userdBackingOffset_, kickToken_, &subPbOff_,
                        &subSeq_};
        ret = submitRing(ring, words, count, nsOut);
    }
    setProperty("NVGspControl-submit-count", subSeq_, 32);
    setProperty("NVGspControl-submit-last-result", static_cast<UInt32>(ret), 32);
    setProperty("NVGspControl-submit-last-ns", *nsOut, 64);
    setProperty("NVGspControl-fence-sleeps", fenceSleeps_, 32);
    setProperty("NVGspControl-fence-wakes", fenceWakes_, 32);
    ulk();
    return ret;
}

bool NVGspControl::grRing(Ring *out) {
    if (!grPersistent_ || !ctxBackingOffset_) return false;
    *out = Ring{ctxBackingOffset_ + 0x2990000, 0x104000000ULL + 0x2990000, 0x60000,
                ctxBackingOffset_ + 0x29FF000, 0x104000000ULL + 0x29FF000,
                gpfifoBackingOffset_, userdBackingOffset_, kickToken_, &subPbOff_,
                &subSeq_};
    return true;
}

// 0.112.0: the persistent copy-engine channel's ring (PB, semaphore, GPFIFO).
bool NVGspControl::ceRing(Ring *out) {
    if (!cePersistent_ || !ceChunk_ || ceStall_.hung) return false;   // 0.164.0: hung until the reset
    constexpr UInt64 kCeVa = 0x108E00000ULL;
    *out = Ring{ceChunk_ + 0x10000, kCeVa + 0x10000, 0x1E0000,
                ceChunk_ + 0x1F0000, kCeVa + 0x1F0000, ceChunk_,
                ceUserdOffset_, ceToken_, &cePbOff_, &ceSeq_};
    return true;
}

void NVGspControl::noteGrOwnerLocked(const void *owner) {
    if (lastGrOwner_ == owner) return;
    lastGrOwner_ = owner;
    ++grOwnerSwitches_;
}

bool NVGspControl::grLastOwnerIsOther(const void *owner) {
    if (!lock_) return false;
    lk(__LINE__);
    const bool other = !clientChannelLocked(owner) && lastGrOwner_ && lastGrOwner_ != owner;
    ulk();
    return other;
}

IOReturn NVGspControl::submitGrAsync(const void *owner, const UInt32 *words, UInt32 count,
                                     UInt32 *seqOut) {
    if (nvramBoostPinned_ == 0) noteGpuBusy();          // 0.135.0
    if (!lock_ || !pci_ || !words || !count || !seqOut) return kIOReturnBadArgument;
    lk(__LINE__);
    Ring ring{};
    UInt64 ns = 0;
    ClientChannel *own = clientChannelLocked(owner);   // 0.178.0
    IOReturn ret = own ? (clientRingLocked(own, &ring) ? clientArenaLocked(own) : kIOReturnNotReady)
                       : !grRing(&ring) ? kIOReturnNotReady : arenaSwitchLocked(owner);   // 0.111.0
    if (ret == kIOReturnSuccess && own && !clientRingLocked(own, &ring)) ret = kIOReturnNotReady;
    if (ret == kIOReturnSuccess && !own) noteGrOwnerLocked(owner);
    if (ret == kIOReturnSuccess) ret = submitRing(ring, words, count, &ns, true);
    *seqOut = ring.seq ? *ring.seq : subSeq_;
    ulk();
    return ret;
}

IOReturn NVGspControl::waitGrFence(UInt32 seq, UInt32 timeoutUs, UInt32 *completedOut,
                                   const void *owner) {
    return waitFence(0, seq, timeoutUs, completedOut, owner);
}

// 0.112.0: fence wait on the GR (0) or CE (1) ring; 0.116.0: 2+ = video engine.
IOReturn NVGspControl::waitFence(UInt32 engine, UInt32 seq, UInt32 timeoutUs,
                                 UInt32 *completedOut, const void *owner) {
    if (!lock_ || !pci_ || !completedOut || engine >= 2 + nvgsp::kVideoEngineCount)
        return kIOReturnBadArgument;
    lk(__LINE__);
    Ring ring{};
    IOReturn ret = kIOReturnNotReady;
    const bool have = engine == 0 ? grRingFor(owner, &ring) : engine == 1 ? ceRing(&ring)
                                                                         : videoRing(engine - 2, &ring);
    const bool own = engine == 0 && have && ring.seq != &subSeq_;   // 0.178.0
    if (have) {
        const bool sleepy = engine < 2 && intrArmed_ && !inIntr_ &&
                            vec_[engine ? kVecCeNs : kVecGrNs] != ~0U;
        const UInt32 t = engine < 2 ? stallCapUs(ring, timeoutUs) : timeoutUs;   // 0.164.0
        ret = (sleepy ? waitFenceSleep(engine, seq, t, owner) : waitRingSem(ring, seq, t))
            ? kIOReturnSuccess : kIOReturnTimeout;
        if (sleepy && !(engine ? ceRing(&ring) : grRingFor(owner, &ring))) {   // reset while asleep
            ulk();
            return kIOReturnNotReady;
        }
        if (sleepy && t >= 20000) noteRingWaitLocked(ring, ret == kIOReturnSuccess);
        UInt32 v = 0;
        readRingSem(ring, &v);
        *completedOut = v;
        if (ret == kIOReturnTimeout && engine >= 2) videoMarkDeadLocked(engine - 2);   // 0.122.0
        if (engine == 0) stampAdvanceLocked();   // 0.178.12
        // 0.178.5: the owner's channel may have closed while we slept
        // (another thread of the process exiting): never dereference it blind
        ClientChannel *oc = own ? clientChannelLocked(owner) : nullptr;
        if (ret == kIOReturnSuccess && v == *ring.seq && (!own || oc))
            (oc ? oc->outstanding : engine == 0 ? asyncOutstanding_
                : engine == 1 ? ceOutstanding_ : video_[engine - 2].outstanding) = 0;
    }
    ulk();
    return ret;
}

IOReturn NVGspControl::submitCe(const void *owner, const UInt32 *words, UInt32 count,
                                UInt64 *nsOut) {
    if (!lock_ || !pci_ || !words || !count || !nsOut)
        return kIOReturnBadArgument;
    lk(__LINE__);
    IOReturn ret = kIOReturnNotReady;
    if (cePersistent_ && ceChunk_ &&
        (ret = arenaSwitchLocked(owner)) == kIOReturnSuccess) {   // 0.111.0
        constexpr UInt64 kCeVa = 0x108E00000ULL;
        const Ring ring{ceChunk_ + 0x10000, kCeVa + 0x10000, 0x1E0000,
                        ceChunk_ + 0x1F0000, kCeVa + 0x1F0000, ceChunk_,
                        ceUserdOffset_, ceToken_, &cePbOff_, &ceSeq_};
        ret = submitRing(ring, words, count, nsOut);
    }
    setProperty("NVGspControl-ce-submit-count", ceSeq_, 32);
    setProperty("NVGspControl-ce-submit-last-result", static_cast<UInt32>(ret), 32);
    setProperty("NVGspControl-ce-submit-last-ns", *nsOut, 64);
    ulk();
    return ret;
}

// 0.116.0 (V1): video engine channels. A channel per engine (NVDEC0 /
// NVENC0 / OFA0) is brought up on first use from a user client, with
// synchronous RPCs (userRpc), the way the CE channel is at boot:
//   GET_CONSTRUCTED_FALCON_INFO (context buffer size) -> 2 MiB VRAM chunk
//   (GPFIFO, fence semaphore, tail PB, falcon context; GPU VA = VRAM window
//   + phys) -> USERD / instance / method buffer (RPC memory) -> channel
//   alloc (chid 5/6/7) -> BIND -> SCHEDULE -> work submit token ->
//   PROMOTE_CTX of the falcon context (CPU-RM owns it in the GSP split,
//   kernel_falcon.c) -> engine object (NVC9B0 / NVC9B7 / NVC9FA).
// Failure frees what was made; stage/status say where it stopped.
// 0.123.0: read-only runlist/CHRAM snapshot of a channel (nouveau ga100:
// DEVICE_INFO2 at 0x22800 gives each engine's runlist PRI base; runlist +0x4
// = CHRAM base | log2(channels), +0x8 bits 31:16 = doorbell id, +0x10/0x14 =
// PBDMA config; CHRAM[chid] bits: ENABLE 1, NEXT 2, BUSY 3, PBDMA_FAULTED 22,
// ENG_FAULTED 23, ON_PBDMA 24, ON_ENG 25, PENDING 26, CTX_RELOAD 27).
// devType 0x10 NVDEC, 0x0e NVENC, 0x16 OFA, 0x13 CE; published as
// {runlist base, chcfg, dbcfg, pbdma0, pbdma1, CHRAM[chid], doorbell id << 16 | chid,
//  RM work-submit token}. Caller holds lock_.
void NVGspControl::runlistDiagLocked(const char *key, UInt32 devType, UInt32 inst,
                                     UInt32 chid, UInt32 token, UInt32 *runlistOut,
                                     UInt32 *chramOut, UInt32 *tokenOut) {
    IOMemoryMap *map = sharedBar0Map(pci_);
    if (!map) return;
    UInt32 out[8] = {~0U, ~0U, ~0U, ~0U, ~0U, ~0U, ~0U, token};
    if (map->getLength() >= 0x01000000) {
        Bar0Io bar0{map};
        UInt32 cfg = 0;
        bar0.read(0x0224fc, &cfg);
        const UInt32 rows = cfg >> 20;
        UInt32 n = 0, type = ~0U, in = 0, runlist = 0;
        for (UInt32 i = 0; i < rows && i < 256; ++i) {
            UInt32 d = 0;
            if (!bar0.read(0x022800 + i * 4, &d)) break;
            if (!d && n == 0) continue;
            if (n == 0) { type = (d >> 24) & 0x3f; in = (d >> 16) & 0xf; runlist = 0; }
            if (n == 2) runlist = d & 0x00fffc00;
            ++n;
            if (d & 0x80000000) continue;
            n = 0;
            if (type == devType && in == inst && runlist) { out[0] = runlist; break; }
        }
        if (out[0] != ~0U) {
            const UInt32 rl = out[0];
            bar0.read(rl + 0x004, &out[1]);
            bar0.read(rl + 0x008, &out[2]);
            bar0.read(rl + 0x010, &out[3]);
            bar0.read(rl + 0x014, &out[4]);
            const UInt32 chram = out[1] & 0xfffffff0;
            if (chram && chram < 0x01000000 && chid < (1U << (out[1] & 0xf)))
                bar0.read(chram + chid * 4, &out[5]);
            out[6] = (out[2] & 0xffff0000) | chid;
        }
    }
    map->release();
    setProperty(key, out, sizeof(out));
    if (runlistOut) *runlistOut = out[0] != ~0U ? out[0] : 0;
    if (chramOut) *chramOut = out[0] != ~0U ? (out[1] & 0xfffffff0) : 0;
    if (tokenOut && out[0] != ~0U) *tokenOut = out[6];
}

// 0.122.0: a video ring whose fence did not arrive in time: publish its
// USERD GP_GET/GP_PUT (did the PBDMA fetch anything?) and the semaphore,
// and leave it out of engine drains from now on. Submits stay allowed
// (diagnostics); a GPU reset clears it. Caller holds lock_.
void NVGspControl::videoMarkDeadLocked(UInt32 index) {
    if (index >= nvgsp::kVideoEngineCount || !video_[index].ready) return;
    VideoChan &v = video_[index];
    v.dead = true;
    UInt32 gp[2] = {~0U, ~0U};
    UInt64 raw = 0;
    PraminPteResult r{};
    if (praminPteAccess(pci_, v.userd + 0x88, &raw, false, false, &r)) {
        gp[0] = static_cast<UInt32>(raw);          // GP_GET
        gp[1] = static_cast<UInt32>(raw >> 32);    // GP_PUT
    }
    Ring ring{};
    UInt32 sem = ~0U;
    if (videoRing(index, &ring)) readRingSem(ring, &sem);
    char key[48]{};
    size_t pos = 0;
    appendStr(key, sizeof(key), &pos, "NVGspControl-video-ring-");
    appendStr(key, sizeof(key), &pos, nvgsp::videoEngine(index)->name);
    key[pos] = '\0';
    // {GP_GET, GP_PUT, fence semaphore, last submitted seq}
    UInt32 diag[4] = {gp[0], gp[1], sem, v.seq};
    setProperty(key, diag, sizeof(diag));
    videoRunlistDiagLocked(index, "-dead");
}

// 0.123.0: runlist/CHRAM snapshot of video channel `index` and, for
// comparison, of our working CE channel (COPY0 = DEVICE_INFO type 0x13 inst 0, chid 4).
void NVGspControl::videoRunlistDiagLocked(UInt32 index, const char *suffix) {
    static const UInt32 kDevType[nvgsp::kVideoEngineCount] = {0x10, 0x0e, 0x16};
    const nvgsp::VideoEngineDesc *e = nvgsp::videoEngine(index);
    if (!e) return;
    char key[64]{};
    size_t pos = 0;
    appendStr(key, sizeof(key), &pos, "NVGspControl-video-runlist-");
    appendStr(key, sizeof(key), &pos, e->name);
    appendStr(key, sizeof(key), &pos, suffix);
    key[pos] = '\0';
    // 0.129.0: RM's work-submit token for the video channels is the bare chid
    // (0x5), but their runlist has doorbell id 3 (nouveau ga100: token =
    // runlist doorbell << 16 | chid); ring the usermode doorbell with that.
    VideoChan &v = video_[index];
    if (!v.rmToken) v.rmToken = v.token;
    UInt32 calc = 0;
    runlistDiagLocked(key, kDevType[index], 0, e->chid, v.rmToken,
                      &v.runlist, &v.chram, &calc);
    if (calc && (calc & 0xffff) == e->chid) v.token = calc;
    if (cePersistent_)
        runlistDiagLocked("NVGspControl-ce-runlist", 0x13, 0, 4, ceToken_, nullptr, nullptr);
}

bool NVGspControl::kceRing(Ring *out) {
    if (!kce_.ready || kce_.dead) return false;
    constexpr UInt64 kVramVa = 0x2000000000ULL;
    VideoChan &v = kce_;
    *out = Ring{v.chunk + nvgsp::kVideoPbOff, kVramVa + v.chunk + nvgsp::kVideoPbOff,
                nvgsp::kVideoPbBytes, v.chunk + nvgsp::kVideoSemOff,
                kVramVa + v.chunk + nvgsp::kVideoSemOff, v.chunk + nvgsp::kVideoGpfifoOff,
                v.userd, v.token, &v.pbOff, &v.seq};
    return true;
}

// Bring the kernel copy channel up once per GPU lifetime, early (its 2 MiB
// chunk has to come from VRAM before the heap is full). Takes lock_ itself.
void NVGspControl::kceEnsure() {
    if (!lock_ || !pci_) return;
    lk(__LINE__);
    const bool run = !kce_.ready && !videoBusy_ && vramVaTables_ && cePersistent_ &&
                     kceFailGen_ != gpuResets_;
    if (run) videoBusy_ = true;
    const UInt32 gen = gpuResets_;
    ulk();
    if (!run) return;
    videoSetup(nvgsp::kVideoEngineCount, gen);
    lk(__LINE__);
    videoBusy_ = false;
    ulk();
}

bool NVGspControl::videoRing(UInt32 index, Ring *out) {
    if (index >= nvgsp::kVideoEngineCount || !video_[index].ready) return false;
    VideoChan &v = video_[index];
    constexpr UInt64 kVramVa = 0x2000000000ULL;
    *out = Ring{v.chunk + nvgsp::kVideoPbOff, kVramVa + v.chunk + nvgsp::kVideoPbOff,
                nvgsp::kVideoPbBytes, v.chunk + nvgsp::kVideoSemOff,
                kVramVa + v.chunk + nvgsp::kVideoSemOff, v.chunk + nvgsp::kVideoGpfifoOff,
                v.userd, v.token, &v.pbOff, &v.seq};
    return true;
}

IOReturn NVGspControl::videoOpen(UInt32 index, UInt32 *stageOut, UInt32 *statusOut) {
    if (!lock_ || !pci_ || !stageOut || !statusOut || index >= nvgsp::kVideoEngineCount)
        return kIOReturnBadArgument;
    lk(__LINE__);
    VideoChan &v = video_[index];
    IOReturn ret = kIOReturnSuccess;
    if (!v.ready)
        ret = videoBusy_ ? kIOReturnBusy : !vramVaTables_ ? kIOReturnNotReady : kIOReturnSuccess;
    const bool run = !v.ready && ret == kIOReturnSuccess;
    if (run) videoBusy_ = true;
    const UInt32 gen = gpuResets_;
    ulk();
    if (run) ret = videoSetup(index, gen);
    lk(__LINE__);
    if (run) videoBusy_ = false;
    *stageOut = v.stage;
    *statusOut = v.status;
    ulk();
    return ret;
}

// Runs without lock_ (userRpc takes it per call).
static const nvgsp::VideoEngineDesc kKernelCe = {"kce", nvgsp::kEngineTypeCopy0, 0xc7b5, 0,
                                                 0xc0d20300, 8};

IOReturn NVGspControl::videoSetup(UInt32 index, UInt32 gen) {
    const bool kce = index == nvgsp::kVideoEngineCount;   // 0.149.0
    const nvgsp::VideoEngineDesc &e = kce ? kKernelCe : *nvgsp::videoEngine(index);
    constexpr UInt32 kClient = 0xc0d00001, kDevice = 0xc0d00080, kSubdev = 0xc0d02080;
    constexpr UInt32 kVas = 0xc0d090f1;
    constexpr UInt64 kVramVa = 0x2000000000ULL;
    constexpr UInt32 kReplyCap = 104 + nvgsp::kFalconInfoBytes + 28;
    constexpr UInt32 kReqCap = 32 + sizeof(nvgsp::NvChannelAllocParams) + nvgsp::kPromoteCtxBytes;
    UInt8 *reply = static_cast<UInt8 *>(IOMalloc(kReplyCap + kReqCap));
    if (!reply) return kIOReturnNoMemory;
    UInt8 *req = reply + kReplyCap;
    const UInt32 hChan = nvgsp::videoChannelHandle(e);
    VideoChan v{};
    UInt32 stage = 0, status = 0, rb = 0;
    bool chanMade = false, backMade[3] = {};
    // One RPC; ok = transport, RPC result and RM status all zero.
    auto call = [&](UInt32 fn, UInt32 bytes, UInt32 statusAt, UInt32 step) -> bool {
        UInt32 result = ~0U, st = ~0U;
        rb = kReplyCap;
        stage = step;
        const IOReturn r = userRpc(fn, req, bytes, reply, &rb, &result);
        if (r == kIOReturnSuccess && rb >= statusAt + 4) __builtin_memcpy(&st, reply + statusAt, 4);
        status = r != kIOReturnSuccess ? static_cast<UInt32>(r) : result ? result : st;
        return r == kIOReturnSuccess && result == 0 && st == 0;
    };
    auto control = [&](UInt32 hObject, UInt32 cmd, UInt32 bytes) {
        bzero(req, 24 + bytes);
        const UInt32 h[6] = {kClient, hObject, cmd, 0, bytes, 0};
        __builtin_memcpy(req, h, sizeof(h));
        return req + 24;
    };
    auto alloc = [&](UInt32 parent, UInt32 handle, UInt32 cls, UInt32 bytes) {
        bzero(req, 32 + bytes);
        const UInt32 h[8] = {kClient, parent, handle, cls, 0, bytes, 0, 0};
        __builtin_memcpy(req, h, sizeof(h));
        return req + 32;
    };
    bool ok = true;
    // 1: context buffer size of the engine's falcon
    // (a copy engine is not a falcon: no context buffer)
    if (!kce) {
        control(kSubdev, nvgsp::kCmdGetConstructedFalconInfo, nvgsp::kFalconInfoBytes);
        ok = call(76, 24 + nvgsp::kFalconInfoBytes, 92, 1) &&
             nvgsp::falconCtxBytes(reply + 104, rb > 104 ? rb - 104 : 0, e.engDesc, &v.ctxBytes);
    }
    if (ok && status == 0 && v.ctxBytes > 0x1000000) ok = false;   // sanity: <= 16 MiB
    if (!ok && status == 0) status = 0xffff0001;                    // engine not constructed
    // 2: chunk
    if (ok) {
        stage = 2;
        v.chunkBytes = nvgsp::videoChunkBytes(v.ctxBytes);
        v.ctxOff = nvgsp::videoCtxOffset(v.ctxBytes);
        lk(__LINE__);
        ok = memAllocLocked(this, v.chunkBytes, 0, &v.memHandle, &v.chunk) == kIOReturnSuccess;
        ok = ok && praminZeroRange(pci_, v.chunk, nvgsp::kVideoPbOff + nvgsp::kVideoPbBytes) &&
             (!v.ctxBytes || praminZeroRange(pci_, v.chunk + v.ctxOff, v.ctxBytes));
        ulk();
        if (!ok) status = 0xffff0002;
    }
    // 3..5: USERD, instance, method buffer (NV01_MEMORY_LOCAL_USER, GSP heap)
    for (UInt32 i = 0; ok && i < 3; ++i) {
        UInt8 *p = alloc(kSubdev, nvgsp::videoBackingHandle(e, i), 0x40, 128);
        constexpr UInt32 kDmaType = 6, kAttr = 0x10800000;
        const UInt64 bytes = i == 2
            ? UInt64((methodBufferBytes_ ? methodBufferBytes_ : 20480) + 0xfff) & ~0xfffULL
            : 4096;
        __builtin_memcpy(p, &kClient, 4);
        __builtin_memcpy(p + 4, &kDmaType, 4);
        __builtin_memcpy(p + 24, &kAttr, 4);
        __builtin_memcpy(p + 64, &bytes, 8);
        ok = call(103, 32 + 128, 96, 3 + i);
        UInt32 pb = 0;
        UInt64 off = 0;
        if (ok && rb >= 112 + 88) {
            __builtin_memcpy(&pb, reply + 100, 4);
            if (pb == 128) __builtin_memcpy(&off, reply + 112 + 80, 8);
        }
        backMade[i] = ok;
        ok = ok && off;
        if (backMade[i] && !off) status = 0xffff0003;
        (i == 0 ? v.userd : i == 1 ? v.inst : v.mthd) = off;
    }
    if (ok) {
        lk(__LINE__);
        ok = praminZeroRange(pci_, v.userd, 4096) && praminZeroRange(pci_, v.inst, 4096);
        ulk();
        if (!ok) status = 0xffff0004;
    }
    // 6: channel (AMPERE_CHANNEL_GPFIFO_A on the engine's runlist)
    if (ok) {
        nvgsp::NvChannelAllocParams chan{};
        ok = nvgsp::buildChannelAllocParams(kVas, nvgsp::videoBackingHandle(e, 0),
                                            kVramVa + v.chunk + nvgsp::kVideoGpfifoOff, 512,
                                            e.engineType, &chan);
        chan.hObjectError = 0;
        chan.flags = nvgsp::videoChannelFlags(e);
        chan.internalFlags = nvgsp::kChannelInternalFlagsNotifierNone |
                             (kce ? 1u : 0u);   // PRIVILEGE ADMIN (bits 1:0)
        chan.instanceMem = {v.inst, 4096, 2, 1};
        chan.ramfcMem = {v.inst, 512, 2, 1};
        chan.userdMem = {v.userd, 512, 2, 1};
        chan.mthdbufMem = {v.mthd, methodBufferBytes_ ? methodBufferBytes_ : 20480, 2, 0};
        UInt8 *p = alloc(kDevice, hChan, nvgsp::kAmpereChannelGpfifoA, sizeof(chan));
        __builtin_memcpy(p, &chan, sizeof(chan));
        ok = ok && call(103, 32 + sizeof(chan), 96, 6);
        chanMade = ok;
    }
    // 7: bind to the engine, 8: schedule, 9: work submit token
    if (ok) {
        UInt8 *p = control(hChan, 0xa06f0104, 4);
        __builtin_memcpy(p, &e.engineType, 4);
        ok = call(76, 28, 92, 7);
    }
    if (ok) {
        control(hChan, 0xa06f0103, 2)[0] = 1;   // bEnable
        ok = call(76, 26, 92, 8);
    }
    if (ok) {
        control(hChan, 0xc36f0108, 4);
        ok = call(76, 28, 92, 9) && rb >= 108;
        if (ok) __builtin_memcpy(&v.token, reply + 104, 4);
        if (ok && !v.token) { ok = false; status = 0xffff0009; }
    }
    // 10: falcon context promote (none when the engine reports size 0).
    // 0.120.0: 0.116.0's layout got NV_ERR_INVALID_STATE live; try the
    // layouts of NVGspVideo.hpp in order, then (all refused) the object
    // without a promote. promote = variant used (3 = none) << 32 | first status.
    UInt32 promoteVariant = nvgsp::kPromoteVariants, promoteFirst = 0, promoteVaStatus = ~0U;
    if (ok && v.ctxBytes) {
        bool promoted = false;
        for (UInt32 var = 0; var < nvgsp::kPromoteVariants && !promoted; ++var) {
            UInt8 *p = control(kSubdev, nvgsp::kCmdPromoteCtx, nvgsp::kPromoteCtxBytes);
            promoted = nvgsp::buildFalconPromote(e.engineType, kClient, e.chid, hChan,
                                                 v.chunk + v.ctxOff, kVramVa + v.chunk + v.ctxOff,
                                                 v.ctxBytes, p, nvgsp::kPromoteCtxBytes, var) &&
                       call(76, 24 + nvgsp::kPromoteCtxBytes, 92, 10);
            if (promoted) promoteVariant = var;
            else if (!promoteFirst) promoteFirst = status;
        }
        status = promoted ? 0 : promoteFirst;
        // 0.121.0: after the physical-only promote, bind the context VA
        // (UVM's second promote); live 0.120.0 object alloc without it = 0x57.
        if (promoted && promoteVariant == nvgsp::kPromoteRmExternal) {
            UInt8 *p = control(kSubdev, nvgsp::kCmdPromoteCtx, nvgsp::kPromoteCtxBytes);
            const bool bound = nvgsp::buildFalconPromoteVa(e.engineType, kClient, hChan,
                                                           kVramVa + v.chunk + v.ctxOff, p,
                                                           nvgsp::kPromoteCtxBytes) &&
                               call(76, 24 + nvgsp::kPromoteCtxBytes, 92, 10);
            promoteVaStatus = bound ? 0 : status;
            status = 0;
        }
    }
    // 11: engine object
    if (ok) {
        if (kce) {
            alloc(hChan, nvgsp::videoObjectHandle(e), e.objClass, 0);   // as the CE object
            ok = call(103, 32, 96, 11);
        } else {
            nvgsp::buildVideoObjectParams(0, alloc(hChan, nvgsp::videoObjectHandle(e), e.objClass, 12));
            ok = call(103, 32 + 12, 96, 11);
        }
    }
    lk(__LINE__);
    if (ok && gen != gpuResets_) { ok = false; status = 0xffff000c; }
    ulk();
    if (!ok) {
        // free the channel (and with it the object + context binding), then
        // the backing memory; 16-byte NVOS00 {hRoot, hParent, hObject, status}
        UInt32 st = stage, sv = status;
        for (UInt32 i = 0; i < 4; ++i) {
            if (i == 0 ? !chanMade : !backMade[i - 1]) continue;
            const UInt32 f[4] = {kClient, i == 0 ? kDevice : kSubdev,
                                 i == 0 ? hChan : nvgsp::videoBackingHandle(e, i - 1), 0};
            bzero(req, 16);
            __builtin_memcpy(req, f, sizeof(f));
            call(10, 16, 96, 12);
        }
        stage = st;
        status = sv;
    }
    lk(__LINE__);
    VideoChan &live = kce ? kce_ : video_[index];
    if (!ok && v.memHandle) releaseGpuMemLocked(v.memHandle - 1);
    if (ok) {
        live = v;
        live.ready = true;
        stage = 12;
        if (!kce) videoRunlistDiagLocked(index, "");   // 0.123.0
    }
    if (kce && !ok) kceFailGen_ = gpuResets_;
    live.stage = stage;
    live.status = status;
    char key[48]{};
    size_t pos = 0;
    appendStr(key, sizeof(key), &pos, "NVGspControl-video-");
    appendStr(key, sizeof(key), &pos, e.name);
    key[pos] = '\0';
    setProperty(key, (UInt64(stage) << 32) | status, 64);
    if (v.ctxBytes && stage >= 10) {
        pos = 0;
        appendStr(key, sizeof(key), &pos, "NVGspControl-video-promote-");
        appendStr(key, sizeof(key), &pos, e.name);
        key[pos] = '\0';
        setProperty(key, (UInt64(promoteVariant) << 32) | promoteFirst, 64);
        pos = 0;   // 0.121.0: VA bind promote status (~0 = not sent)
        appendStr(key, sizeof(key), &pos, "NVGspControl-video-promote-va-");
        appendStr(key, sizeof(key), &pos, e.name);
        key[pos] = '\0';
        setProperty(key, promoteVaStatus, 32);
    }
    if (ok) {
        pos = 0;
        appendStr(key, sizeof(key), &pos, "NVGspControl-video-ctx-");
        appendStr(key, sizeof(key), &pos, e.name);
        key[pos] = '\0';
        setProperty(key, v.ctxBytes, 32);
    }
    ulk();
    IOFree(reply, kReplyCap + kReqCap);
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

// Caller holds lock_. The channels die with the GSP/VAS (reset, stop,
// resume); only the chunk bookkeeping is ours to drop.
void NVGspControl::videoDropLocked() {
    for (UInt32 i = 0; i < nvgsp::kVideoEngineCount; ++i) {
        if (video_[i].memHandle) releaseGpuMemLocked(video_[i].memHandle - 1);
        video_[i] = VideoChan{};
    }
    if (kce_.memHandle) releaseGpuMemLocked(kce_.memHandle - 1);   // 0.149.0
    kce_ = VideoChan{};
}

// 0.160.0: memory per client, refreshed from the status poll (every 2 s):
// "pid N: objs O, vram V MiB, sys S MiB, user U MiB" plus totals, so a
// process that keeps allocating (or VRAM filling up) shows at a glance.
void NVGspControl::publishMemByClientLocked() {
    struct Row { const void *owner; int pid; UInt32 objs; UInt64 vram, sys, user; };
    Row rows[24] = {};
    UInt32 nrows = 0, used = 0;
    UInt64 tv = 0, ts = 0, tu = 0;
    for (UInt32 i = 0; i < kMaxMem; ++i) {
        const GpuMem &m = mem_[i];
        if (!m.owner) continue;
        ++used;
        UInt32 r = 0;
        while (r < nrows && rows[r].owner != m.owner) ++r;
        if (r == nrows && nrows < 24) {
            rows[nrows].owner = m.owner;
            for (UInt32 c = 0; c < kMaxArenaCtx; ++c)
                if (arenaCtx_[c] && (arenaCtx_[c] == m.owner || arenaCtx_[c]->owner == m.owner)) rows[nrows].pid = arenaCtx_[c]->pid;
            ++nrows;
        }
        if (r == nrows) continue;
        rows[r].objs++;
        UInt64 &b = m.domain == 0 ? rows[r].vram : m.domain == 3 ? rows[r].user : rows[r].sys;
        b += m.bytes;
        (m.domain == 0 ? tv : m.domain == 3 ? tu : ts) += m.bytes;
    }
    char s[1400];
    int n = snprintf(s, sizeof s, "slots %u/%u, vram %llu MiB, sys %llu MiB, user %llu MiB;", used, kMaxMem,
                     tv >> 20, ts >> 20, tu >> 20);
    for (UInt32 r = 0; r < nrows && n < (int)sizeof s - 80; ++r)
        n += snprintf(s + n, sizeof s - n, " [pid %d objs %u vram %llu sys %llu user %llu MiB]", rows[r].pid,
                      rows[r].objs, rows[r].vram >> 20, rows[r].sys >> 20, rows[r].user >> 20);
    setProperty("NVGspControl-mem-by-client", s);
    setProperty("NVGspControl-mem-slots-used", used, 32);
    setProperty("NVGspControl-mem-vram-bytes", tv, 64);
    setProperty("NVGspControl-mem-sys-bytes", ts + tu, 64);   // 0.162.1: NVAccelerator's PerformanceStatistics
}

IOReturn NVGspControl::pollStatusLocked() {
    publishMemByClientLocked();
    if (!executed_) return kIOReturnNotReady;
    // 0.81.0: the 0.9.6 per-poll bisect64 properties (4 new keys per
    // poll, unbounded) are gone.
    nvgsp::MsgqTxHeader header{};
    UInt32 readPtr = 0;
    UInt8 first[64]{};
    const bool snap = init_.snapshotStatus(&header, &readPtr, first, sizeof(first));
    const bool linked = snap && header.version == 0 &&
        header.size == nvgsp::kGspQueueBytes && header.msgSize == 4096 &&
        header.msgCount == 63 && header.rxHdrOff >= sizeof(header) &&
        header.entryOff == 4096 && readPtr < header.msgCount &&
        header.writePtr < header.msgCount;
    setProperty("NVGspControl-status-snapshot-ok", snap);
    setProperty("NVGspControl-status-linked", linked);
    setProperty("NVGspControl-status-version", header.version, 32);
    setProperty("NVGspControl-status-size", header.size, 32);
    setProperty("NVGspControl-status-msg-size", header.msgSize, 32);
    setProperty("NVGspControl-status-msg-count", header.msgCount, 32);
    setProperty("NVGspControl-status-write-ptr", header.writePtr, 32);
    setProperty("NVGspControl-status-read-ptr", readPtr, 32);
    setProperty("NVGspControl-status-flags", header.flags, 32);
    setProperty("NVGspControl-status-rx-hdr-off", header.rxHdrOff, 32);
    setProperty("NVGspControl-status-entry-off", header.entryOff, 32);
    if (snap) setProperty("NVGspControl-status-first64", first, sizeof(first));
    if (!linked) return kIOReturnNotReady;

    constexpr UInt32 kRpcHeaderVersion = 0x03000000;
    constexpr UInt32 kRpcSignature = 0x43505256;
    constexpr UInt32 kInitDone = 0x1001;
    constexpr UInt32 kRunCpuSequencer = 0x1002;
    constexpr UInt32 kOsErrorLog = 0x1006;
    constexpr UInt32 kLibosPrint = 0x100c;
    constexpr UInt32 kLockdownNotice = 0x101c;
    constexpr UInt32 kPostNocat = 0x1020;
    constexpr UInt32 kMaxRecords = 256;
    auto *records = static_cast<nvgsp::GspStatusRecordSummary *>(
        IOMalloc(kMaxRecords * sizeof(nvgsp::GspStatusRecordSummary)));
    if (!records) return kIOReturnNoMemory;
    bzero(records, kMaxRecords * sizeof(*records));
    constexpr UInt32 kStaticInfoBytes = 1656;
    UInt8 *staticInfo = static_cast<UInt8 *>(IOMalloc(kStaticInfoBytes));
    if (!staticInfo) {
        IOFree(records, kMaxRecords * sizeof(nvgsp::GspStatusRecordSummary));
        return kIOReturnNoMemory;
    }
    bzero(staticInfo, kStaticInfoBytes);

    UInt32 recordCount = 0, consumed = 0, badReason = 0, blockedFunction = 0;
    UInt32 libosPrints = 0, osErrors = 0, lockdownNotices = 0;
    UInt8 *blockedData = nullptr;
    UInt32 blockedBytes = 0;
    bool initDone = initDone_;
    UInt32 initResult = initResult_, initPrivateResult = initPrivateResult_;
    bool guestInfoResponse = false, guestInfoExtResponse = false;
    bool staticInfoResponse = false, deviceInfoResponse = false;
    bool classListResponse = false;
    bool clientAllocResponse = false, clientFreeResponse = false;
    bool deviceIdsResponse = false, attachedIdsResponse = false;
    bool ownedClientResponse = false, deviceAllocResponse = false;
    bool subdeviceAllocResponse = false, vaspaceAllocResponse = false;
    bool localMemoryAllocResponse = false, virtualMemoryAllocResponse = false;
    bool gpfifoBackingAllocResponse = false, userdBackingAllocResponse = false;
    bool methodSizeResponse = false, instanceBackingAllocResponse = false;
    bool methodBackingAllocResponse = false;
    // 0.7.0: answered-but-refused is distinct from no-answer (stall).
    bool methodSizeAnswered = false, instanceBackingAnswered = false;
    bool methodBackingAnswered = false;
    bool errBackingAllocResponse = false;
    bool errCtxAllocReturned = false, errCtxAllocResponse = false;
    bool channelAllocReturned = false, channelAllocResponse = false;
    // 0.7.1: bind + schedule keep a successfully allocated channel alive.
    bool bindReturned = false, bindResponse = false;
    bool scheduleReturned = false, scheduleResponse = false;
    // 0.7.2: TSG-level bind + schedule (channel-level schedule is 0x1f).
    bool tsgBindReturned = false, tsgBindResponse = false;
    bool tsgScheduleReturned = false, tsgScheduleResponse = false;
    // 0.7.3: TSG GET_INFO diagnostic between bind and schedule.
    bool tsgInfoReturned = false, tsgInfoResponse = false;
    // 0.7.5: SET_TIMESLICE policy discriminator (replaces the 0.7.4
    // disable probe — disable also 0x1f, matrix resolved).
    bool tsgTimesliceReturned = false, tsgTimesliceResponse = false;
    // 0.8.0: FIFO intel (58-61) + token (62) + DMA flush (63). No schedule.
    bool fifoInfoReturned = false, fifoInfoResponse = false;
    bool userdLocReturned = false, userdLocResponse = false;
    bool partnerReturned = false, partnerResponse = false;
    bool priBaseReturned = false, priBaseResponse = false;
    bool tokenReturned = false, tokenResponse = false;
    bool flushReturned = false, flushResponse = false;
    // 0.8.1: display parent (64) + PB backing (65) + ctxdma (66) + c77d (67).
    // 0.8.4: + notifier backing (67) + notifier ctxdma (68); c77d moves to 69.
    // 0.8.5: phase 69 is the split-RM physical pushbuffer control instead.
    // 0.8.6: its function-76 response is admitted by expectedPostInit below.
    bool dispReturned = false, dispResponse = false;
    bool pbBackingReturned = false, pbBackingResponse = false;
    bool pbCtxdmaReturned = false, pbCtxdmaResponse = false;
    bool notifyBackingReturned = false, notifyBackingResponse = false;
    bool notifyCtxdmaReturned = false, notifyCtxdmaResponse = false;
    bool dispPbProgramReturned = false, dispPbProgramResponse = false;
    bool dispKickFlushReturned = false, dispKickFlushResponse = false;
    // 0.8.9: handler-validation probes (71: valid=0/ch0, 72: valid=1/ch7).
    bool dispHandlerProbe1Returned = false, dispHandlerProbe1Response = false;
    bool dispHandlerProbe2Returned = false, dispHandlerProbe2Response = false;
    // 0.9.0: function-97 schedule retry (73: channel, 74: TSG).
    bool sched97chanReturned = false, sched97chanResponse = false;
    bool sched97tsgReturned = false, sched97tsgResponse = false;
    // 0.9.1: C372 display-SW alloc probe (75) + post-FWSEC head survey.
    bool dispC372Returned = false, dispC372Response = false;
    // 0.9.2: display-common (0x73) alloc probe (76).
    bool dispCommonReturned = false, dispCommonResponse = false;
    // 0.9.7: display-common queries on the 0x73 object (77: GET_SUPPORTED,
    // 78: GET_NUM_HEADS, 79: GET_ACTIVE head0). First RM display queries.
    bool dispSysSupportedReturned = false, dispSysSupportedResponse = false;
    bool dispSysNumHeadsReturned = false, dispSysNumHeadsResponse = false;
    bool dispSysActiveReturned = false, dispSysActiveResponse = false;
    // 0.9.8: GET_ACTIVE heads 1-3 (80-82), GET_CONNECT_STATE (83),
    // GET_BOOT_DISPLAYS (84). All read-only on the 0x73 object.
    bool dispSysActive1Returned = false, dispSysActive1Response = false;
    bool dispSysActive2Returned = false, dispSysActive2Response = false;
    bool dispSysActive3Returned = false, dispSysActive3Response = false;
    bool dispConnectReturned = false, dispConnectResponse = false;
    bool dispBootDisplaysReturned = false, dispBootDisplaysResponse = false;
    // 0.9.9: SCANLINE head0 (85), VBLANK_COUNTER head0 (86),
    // HEAD_ROUTING_MAP (87). Read-only scanout-state proof.
    bool dispScanlineReturned = false, dispScanlineResponse = false;
    bool dispVblankReturned = false, dispVblankResponse = false;
    bool dispRoutingReturned = false, dispRoutingResponse = false;
    // 0.10.0: first C372 control call — GET_ACTIVE_VIEWPORT_POINT_IN
    // (0xc3720104) window 0 on the 0xc0d0c372 object (88). Read-only.
    bool dispViewportReturned = false, dispViewportResponse = false;
    // 0.11.0: DFP_GET_INFO (0x731140) on display 0x200 via the 0x73
    // object (nvkms GetDfpInfo pattern, phase 89). Read-only.
    bool dispDfpReturned = false, dispDfpResponse = false;
    // 0.12.0: SPECIFIC_GET_EDID_V2 (0x730245) cached read on display
    // 0x200 via the 0x73 object (nvkms ReadEdidFromResman pattern,
    // phase 90). 2064B params; EDID hashed + first bytes captured.
    bool dispEdidReturned = false, dispEdidResponse = false;
    // 0.14.0: SPECIFIC_GET_PCLK_LIMIT (0x73028a, phase 91) +
    // SPECIFIC_OR_GET_INFO (0x73028b, phase 92, index 0) on display
    // 0x200. Pixel-clock bounds + SOR/OR assignment + VBIOS-lit flag.
    bool dispPclkReturned = false, dispPclkResponse = false;
    bool dispOrReturned = false, dispOrResponse = false;
    // 0.15.0: SYSTEM_GET_CAPS_V2 (0x730101, phase 93, 2B caps table)
    // + SYSTEM_GET_VBLANK_ENABLE head0 (0x730106, phase 94).
    bool dispCapsReturned = false, dispCapsResponse = false;
    bool dispVbEnReturned = false, dispVbEnResponse = false;
    // 0.16.0: C372 IS_MODE_POSSIBLE (0xc3720101, phase 95) with a
    // zeroed 2048B struct (numHeads=0; host-verified sizeof against
    // the open headers). Validation-path liveness, no fabricated
    // timing. bIsPossible @1904.
    bool dispImpReturned = false, dispImpResponse = false;
    // 0.17.0: IS_MODE_POSSIBLE head0 CTA-861 4K60 (phase 96).
    // Same 2048B struct; head0 = {594 MHz, 3840x2160, blank
    // (4016,2168)-(4400,2250)}, numWindows=0. Read-only validation
    // of real timing (GOP mode), no windows/surfaces.
    bool dispModeReturned = false, dispModeResponse = false;
    // 0.18.0: SPECIFIC_GET_CONNECTOR_DATA (0x730250, phase 97,
    // 72B) on display 0x200 — physical connector index/type/
    // location from firmware (no engine init needed, hopefully).
    bool dispConnReturned = false, dispConnResponse = false;
    // 0.19.0: subdevice discovery — MC_GET_ARCH_INFO (0x20801701,
    // phase 98, 13B) + GPU_GET_NAME_STRING ASCII (0x20800110,
    // phase 99, 68B) on 0xc0d02080. Read-only engine/chip IDs.
    bool dispArchReturned = false, dispArchResponse = false;
    bool dispNameReturned = false, dispNameResponse = false;
    // 0.20.0: FIFO_GET_PHYSICAL_CHANNEL_COUNT (0x20801108, phase
    // 100, 8B) on the subdevice — wall test: is the 0x1F wall
    // object-wide (close subdevice branch) or interface-specific?
    bool dispFifoReturned = false, dispFifoResponse = false;
    // 0.21.0: FIFO_GET_ALLOCATED_CHANNELS (0x20801119, phase 101,
    // 516B, runlist 0) — is our cid-3 channel in RM's runlist
    // accounting? popcount + first words published.
    bool dispRunlistReturned = false, dispRunlistResponse = false;
    // 0.22.0: GR BIND (0xa06f0104, engineType=1) on the COPY-bound
    // channel (phase 102). BIND configures for scheduling; a GR bind
    // may succeed where COPY-schedule refused. Empty GPFIFO idles.
    bool dispGrBindReturned = false, dispGrBindResponse = false;
    // 0.23.0: runlist RE-QUERY after the GR bind (phase 103, same
    // 516B command, runlist 0) — did our channel get scheduled?
    // Compare popcount/w0 against 0.21.0 (58/0x0).
    bool dispReRunReturned = false, dispReRunResponse = false;
    // 0.24.0: runlist isolation — phase 104 = runlist 0 third
    // sample (bind-vs-time), 105/106 = runlists 1/2 (is our
    // channel on another runlist?). Same 516B command.
    bool dispRl0cReturned = false, dispRl0cResponse = false;
    bool dispRl1Returned = false, dispRl1Response = false;
    bool dispRl2Returned = false, dispRl2Response = false;
    // 0.26.0: post-GR-bind GPFIFO_SCHEDULE (0xa06f0103, phase 107,
    // exact 2B {bEnable=1, bSkipSubmit=0} per header) + rl0 re-query
    // (phase 108). Schedule never retried after GR bind — new
    // condition. Empty GPFIFO idles if enabled.
    bool dispSchedReturned = false, dispSchedResponse = false;
    bool dispPostReturned = false, dispPostResponse = false;
    // 0.27.0: rl1/rl2 POST-schedule samples (phases 109/110,
    // same 516B command) — did schedule place us off rl0?
    // Publish popcount + hash + w0/w1 only (telemetry trim).
    bool dispPs1Returned = false, dispPs1Response = false;
    bool dispPs2Returned = false, dispPs2Response = false;
    // 0.29.0: FIFO_GET_DEVICE_INFO_TABLE (0x20801112, phase 111,
    // 3212B: baseIndex=0, numEntries=32) — engine entries with
    // PBDMA ids + names. Read-only PBDMA identity for doorbell.
    bool dispDevInfoReturned = false, dispDevInfoResponse = false;
    // 0.31.0: local VRAM surveys (no RPC) — USERD slot baseline
    // (phase 112, 8x u64 incl. GET) + compute GPFIFO head
    // (phase 113, 4x u64 entries) via the proven PRAMIN window.
    // Decides kick design: engine touched anything?
    bool dispUserdReturned = false, dispUserdResponse = false;
    bool dispGpfifoReturned = false, dispGpfifoResponse = false;
    // 0.32.0: RAMFC/instance survey (phase 114, 8x u64 at
    // instanceBackingOffset_) — PUT/GET shadows as RM sees them.
    bool dispRamfcReturned = false, dispRamfcResponse = false;
    // 0.33.0: DEVICE-object FIFO plane (new wall surface) —
    // FIFO_GET_CAPS_V2 (0x801713, phase 115, 2B) +
    // GET_ENGINE_CONTEXT_PROPERTIES GRAPHICS (0x801707, phase 116,
    // 12B) on device 0xc0d00080. Read-only.
    bool dispFifoCapsReturned = false, dispFifoCapsResponse = false;
    bool dispCtxPropReturned = false, dispCtxPropResponse = false;
    // 0.34.0: GR_GET_TPC_PARTITION_MODE (0x801107, phase 117,
    // 32B: hTSG + zeros) on device — TPC partition state for OUR
    // TSG (affects GR units available). GET only.
    bool dispTpcReturned = false, dispTpcResponse = false;
    // 0.35.0: GPFIFO_GET_WORK_SUBMIT_TOKEN (0xc36f0108, phase 118,
    // 4B) on the GR-bound scheduled channel, then local NOP kick
    // (phase 119: GP_GET poll). Control entry, LENGTH 0: no PB fetch.
    bool dispTokenReturned = false, dispTokenResponse = false;
    // 0.37.0: INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO (0x20800a32,
    // phase 121, 1664B) on the internal subdevice — GR0 ctx buffer
    // sizes/alignments (26 ids) for client-RM ctx alloc + PROMOTE_CTX.
    bool dispCtxBufReturned = false, dispCtxBufResponse = false;
    // 0.38.0: ctx VRAM alloc (103, phase 122), PROMOTE_CTX (0x2080012b,
    // phase 123), ADA_COMPUTE_A object on the channel (103, phase 124),
    // then local SET_OBJECT + WFI semaphore kick poll (phase 125).
    bool ctxMemReturned = false, ctxMemResponse = false;
    bool promoteReturned = false, promoteResponse = false;
    bool grObjReturned = false, grObjResponse = false;
    // 0.51.0: display core-channel probe (experiment bit1): 64 KiB
    // display instance VRAM (103, phase 200), INTERNAL_DISPLAY_WRITE_INST_MEM
    // (0x20800a49, phase 201), then after set_pushbuf the C77D core
    // channel alloc with nouveau r535 params (103, phase 202).
    bool dispInstMemReturned = false, dispInstMemResponse = false;
    bool dispInstWriteReturned = false, dispInstWriteResponse = false;
    bool coreChanReturned = false, coreChanResponse = false;
    // 0.53.0: post-kick display health (display common 0x73 alloc 203,
    // GET_ACTIVE head0 204, GET_SCANLINE head0 x2 205/206).
    bool dispHealthReturned = false, dispHealthOk = false;
    // 0.54.0: window 0 channel (experiment bit2): set_pushbuf C67E (207),
    // C67E alloc (208), then a local flip kick.
    bool wndPbReturned = false, wndPbOk = false;
    bool wndChanReturned = false, wndChanOk = false;
    // 0.57.0: 2D object alloc reply (phase 209).
    bool twoDReturned = false;
    bool scratchReturned = false;
    // 0.63.0: CE chain replies (212 alloc, 213 bind, 214 schedule,
    // 215 token, 216 CE object).
    bool ceReturned = false, ceOk = false;
    bool ceUserdReturned = false, ceBackingOk = false;
    bool perfReturned = false;
    UInt32 ceTokenReply = 0;
    UInt32 twoDStatus = ~0U;
    bool vaCapsResponse = false, pdeInfoResponse = false;
    bool bar2MapResponse = false, bar2MapUnsupportedResponse = false;
    bool bar2VerifyResponse = false;
    bool bar2UnmapResponse = false;
    bool pteMapInvalidateResponse = false;
    bool pteInfoReturned = false;
    bool pteRestoreInvalidateResponse = false;
    bool dmaMapResponse = false, dmaMapUnsupportedResponse = false;
    bool dmaUnmapResponse = false;
    bool ownedTreeFreeResponse = false;
    UInt8 *deviceInfo = nullptr;
    UInt32 deviceInfoBytes = 0, deviceInfoStatus = ~0U;
    UInt8 classList[404]{};
    UInt32 classListStatus = ~0U;
    UInt32 deviceIds = 0, attachedIds[32]{};
    // Draining NOCAT journal records is safe: NVIDIA's host handler only copies
    // them into an optional diagnostic journal. Releasing them lets GSP-RM
    // publish the remaining boot events into its 63-entry ring.
    UInt32 idleWaits = 0;
    for (UInt32 wait = 0; wait < 4000 && recordCount < kMaxRecords; ++wait) {
        if (!init_.snapshotStatus(&header, &readPtr, first, sizeof(first))) {
            badReason = 1; break;
        }
        UInt32 available = header.writePtr + header.msgCount - readPtr;
        if (available >= header.msgCount) available -= header.msgCount;
        if (!available) {
            // 0.81.0: once the chain is parked (phase 33) an empty queue
            // means "nothing to do" — return instead of busy-waiting 4 s
            // with lock_ held (0.80.0: every daemon poll took 4.3 s and
            // stalled vblank/flip/submit behind it). Waiters (userRpc,
            // ping) loop on pollStatusLocked themselves.
            if (postInitPhase_ == 33) break;
            // 0.90.0: during the chain, stop after 150 ms without a new
            // message (replies normally land within a few ms); the daemon's
            // next 1 s poll picks up slow ones. Was a fixed 4 s busy-wait
            // per poll → ~7 minutes for the 114-poll chain.
            // 0.92.0: idle-exit reverted — local (no-RPC) chain phases rely
            // on this loop continuing; 0.90/0.91 parked the chain at
            // pb-backing / disp-sched. Only the watchdog stays time-based.
            // 0.95.0: exit once a collected batch goes quiet (50 ms) instead
            // of burning the ~4 s tail; an empty drain keeps waiting exactly
            // like before, so the 0.90/0.91 park class cannot recur.
            ++idleWaits;
            // 0.100.6: nothing outstanding (previous step queued no RPC) ⇒ no
            // reply to wait for; let the phase machine run now.
            if (drainNoWait_ && recordCount == 0) {
                profDrainEnd_ = mach_absolute_time();
                profIdle_ = idleWaits;
                break;
            }
            if (nvgsp::drainShouldExit(recordCount, idleWaits, postInitPhase_ == 33)) {
                profDrainEnd_ = mach_absolute_time();
                profIdle_ = idleWaits;
                break;
            }
            // 0.146.4: wait for the reply with lock_ dropped (each pass
            // re-snapshots the queue and nothing is half-consumed here);
            // this 1 ms busy-wait under the lock was the remaining 10-100 ms
            // interrupt/vblank stalls at boot.
            ulk();
            IODelay(1000);
            lk(__LINE__);
            continue;
        }

        const UInt8 *entry = init_.statusEntry(readPtr);
        nvgsp::GspQueueElementHeader q{};
        nvgsp::RpcMessageHeader rpc{};
        if (!entry) { badReason = 2; break; }
        __builtin_memcpy(&q, entry, sizeof(q));
        __builtin_memcpy(&rpc, entry + sizeof(q), sizeof(rpc));
        if (!q.elementCount || q.elementCount > 16 || q.elementCount > available) {
            badReason = 3; break;
        }
        uint64_t messageBytes = static_cast<uint64_t>(sizeof(q)) + rpc.length;
        // 0.100.0: a large reply (e.g. ALLOC_MEMORY memlist echo) announces its
        // full length in the first element; the rest follows as CONTINUATION
        // records (fn 71, consumed below). Keep only the first part.
        if (largeReplyOk_ && rpc.length >= sizeof(rpc) &&
            messageBytes > q.elementCount * 4096ULL)
            messageBytes = q.elementCount * 4096ULL;
        if (rpc.length < sizeof(rpc) || messageBytes > q.elementCount * 4096ULL) {
            badReason = 4; break;
        }
        UInt64 checksum = 0;
        for (uint64_t offset = 0; offset < messageBytes; offset += 8) {
            const UInt32 slot = (readPtr + static_cast<UInt32>(offset / 4096)) % header.msgCount;
            const UInt8 *part = init_.statusEntry(slot);
            UInt64 word = 0;
            if (!part) { badReason = 5; break; }
            __builtin_memcpy(&word, part + (offset % 4096), sizeof(word));
            checksum ^= word;
        }
        if (badReason) break;

        auto &summary = records[recordCount++];
        summary.ringIndex = readPtr;
        summary.queueSequence = q.sequence;
        summary.elementCount = q.elementCount;
        summary.checksum = static_cast<UInt32>(checksum >> 32) ^
                           static_cast<UInt32>(checksum);
        summary.headerVersion = rpc.headerVersion;
        summary.signature = rpc.signature;
        summary.length = rpc.length;
        summary.function = rpc.function;
        summary.result = rpc.result;
        summary.privateResult = rpc.privateResult;
        summary.rpcSequence = rpc.sequence;

        if (summary.checksum || q.sequence != statusSequence_ ||
            rpc.headerVersion != kRpcHeaderVersion || rpc.signature != kRpcSignature) {
            badReason = 6; break;
        }
        bool sequencerOk = false;
        SequencerResult sequencer{};
        if (rpc.function == kRunCpuSequencer) {
            const UInt32 assembledBytes = static_cast<UInt32>(messageBytes);
            UInt8 *assembled = static_cast<UInt8 *>(IOMalloc(assembledBytes));
            if (assembled) {
                for (UInt32 offset = 0; offset < assembledBytes; ++offset) {
                    const UInt32 slot = (readPtr + offset / 4096) % header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    assembled[offset] = part ? part[offset % 4096] : 0;
                }
                IODeviceMemory *bar0 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
                IOMemoryMap *seqMap = bar0 ? bar0->map() : nullptr;
                if (seqMap) {
                    Bar0Io seqIo{seqMap};
                    UInt32 chipId0 = 0;
                    sequencerOk = seqIo.read(kBoot0, &chipId0) &&
                        runCpuSequencer(seqIo, assembled, assembledBytes, chipId0,
                                        init_.libosArgsBus(), &sequencer);
                    seqMap->release();
                }
                IOFree(assembled, assembledBytes);
            }
            setProperty("NVGspControl-sequencer-ok", sequencerOk);
            setProperty("NVGspControl-sequencer-commands", sequencer.commands, 32);
            setProperty("NVGspControl-sequencer-writes", sequencer.writes, 32);
            setProperty("NVGspControl-sequencer-polls", sequencer.polls, 32);
            setProperty("NVGspControl-sequencer-failed-index", sequencer.failedIndex, 32);
            setProperty("NVGspControl-sequencer-failed-opcode", sequencer.failedOpcode, 32);
            setProperty("NVGspControl-sequencer-failed-register", sequencer.failedRegister, 32);
            setProperty("NVGspControl-sequencer-last-value", sequencer.lastValue, 32);
            setProperty("NVGspControl-sequencer-core-reset", sequencer.coreReset != 0);
            setProperty("NVGspControl-sequencer-core-start", sequencer.coreStart != 0);
            setProperty("NVGspControl-sequencer-core-halt", sequencer.coreHalt != 0);
            setProperty("NVGspControl-sequencer-core-resume", sequencer.coreResume != 0);
        }
        if (rpc.function == kLibosPrint) ++libosPrints;
        if (rpc.function == kOsErrorLog) {
            ++osErrors;
            // 0.138.3: keep the last Xid text (rpc_os_error_log_v17_00:
            // exceptType, runlistId, chid, errString[0x100] at payload 80).
            if (messageBytes > 80 + 12) {
                char text[160]{};
                const uint64_t avail = messageBytes - 80 - 12;
                const UInt32 n = static_cast<UInt32>(avail < sizeof(text) - 1 ? avail : sizeof(text) - 1);
                __builtin_memcpy(text, entry + 80 + 12, n);
                UInt32 hdr[3]{};
                __builtin_memcpy(hdr, entry + 80, sizeof(hdr));
                setProperty("NVGspControl-os-error-last", text);
                setProperty("NVGspControl-os-error-last-type", hdr[0], 32);
                // 0.164.0: and the last 8, oldest first
                char *slot = osErrLog_[osErrCount_ % 8];
                __builtin_memcpy(slot, text, sizeof(osErrLog_[0]) - 1);
                slot[sizeof(osErrLog_[0]) - 1] = 0;
                ++osErrCount_;
                if (OSArray *log = OSArray::withCapacity(8)) {
                    const UInt32 n = osErrCount_ < 8 ? osErrCount_ : 8;
                    for (UInt32 k = osErrCount_ - n; k < osErrCount_; ++k)
                        if (OSString *str = OSString::withCString(osErrLog_[k % 8])) { log->setObject(str); str->release(); }
                    setProperty("NVGspControl-os-error-log", log);
                    log->release();
                }
            }
        }
        if (rpc.function == kLockdownNotice) ++lockdownNotices;
        // 0.41.0: RC_TRIGGERED (0x1004) / MMU_FAULT_QUEUED (0x1005) are
        // consumed and decoded instead of blocking the queue (0.39/0.40:
        // a GR RC event parked the queue so the free reply never landed).
        // rpc_rc_triggered_v17_02 payload at 80: engine, chid, gfid,
        // exceptLevel, exceptType, scope, partId, mmuFaultLo/Hi/Type.
        constexpr UInt32 kRcTriggered = 0x1004, kMmuFaultQueued = 0x1005;
        if (rpc.function == kRcTriggered) {
            UInt32 rc[10]{};
            if (messageBytes >= 80 + sizeof(rc))
                __builtin_memcpy(rc, entry + 80, sizeof(rc));
            ++rcEvents_;
            setProperty("NVGspControl-rc-count", rcEvents_, 32);
            setProperty("NVGspControl-rc-engine", rc[0], 32);
            setProperty("NVGspControl-rc-chid", rc[1], 32);
            setProperty("NVGspControl-rc-except-level", rc[3], 32);
            setProperty("NVGspControl-rc-except-type", rc[4], 32);
            setProperty("NVGspControl-rc-mmu-fault-lo", rc[7], 32);
            setProperty("NVGspControl-rc-mmu-fault-hi", rc[8], 32);
            setProperty("NVGspControl-rc-mmu-fault-type", rc[9], 32);
            setProperty("NVGspControl-rc-at-phase", postInitPhase_, 32);
            {   // 0.165.0: when, and whose arena the GPU was in (attribution)
                UInt64 now = 0, ms = 0;
                clock_get_uptime(&now);
                absolutetime_to_nanoseconds(now, &ms);
                setProperty("NVGspControl-rc-uptime-ms", ms / 1000000, 64);
                setProperty("NVGspControl-rc-arena-pid", arenaActive_ ? static_cast<UInt32>(arenaActive_->pid) : 0U, 32);
            }
            {   // 0.170.0: every GPU event in one history that survives resets
                char e[160];
                snprintf(e, sizeof(e), "RC chid %u eng %u type %u va 0x%x%08x arena pid %d phase %u", rc[1], rc[0], rc[4],
                         rc[8], rc[7], arenaActive_ ? arenaActive_->pid : -1, postInitPhase_);
                noteGpuEventLocked(e);
            }
            if (rc[7] || rc[8]) explainFaultLocked((UInt64(rc[8]) << 32) | rc[7]);   // 0.158.0
            // 0.59.0: an RC on our channel kills it; stop accepting submits.
            if (rc[1] == channelCid_ && grPersistent_) {
                grPersistent_ = false;
                setProperty("NVGspControl-gr-persistent", false);
                scheduleAutoResetLocked();                     // 0.115.0
            } else if (rc[1] >= 9 && rc[1] < 9 + kMaxClientChannels &&
                       cchan_[rc[1] - 9].state) {
                // 0.178.11: a client's own GR channel (was taken for the CE
                // channel: "CE ring hung"). Its stamps can no longer land and
                // every other channel orders behind them: reset everything.
                const ClientChannel &cc = cchan_[rc[1] - 9];
                ArenaCtx *a = cc.owner ? arenaForLocked(cc.owner, false) : nullptr;
                char e[96];
                snprintf(e, sizeof(e), "client channel %u RC type %u pid %d", rc[1], rc[4], a ? a->pid : -1);
                noteGpuEventLocked(e);
                setProperty("NVGspControl-cchan-rc-chid", rc[1], 32);
                {   // 0.178.15: who was switching: FECS current/new context (instance
                    // block >> 12) against every channel's instance, GR status
                    UInt32 d[kGrDiagCount] = {};
                    readGrDiag(pci_, d);
                    setProperty("NVGspControl-cchan-rc-grdiag", d, sizeof(d));
                    UInt64 inst[kMaxClientChannels + 1] = {};
                    for (UInt32 i = 0; i < kMaxClientChannels; ++i) inst[i] = cchan_[i].state ? cchan_[i].inst : 0;
                    inst[kMaxClientChannels] = instanceBackingOffset_;
                    setProperty("NVGspControl-cchan-rc-insts", inst, sizeof(inst));
                    UInt32 sq[10] = {stampQHead_, stampQTail_};
                    if (stampQHead_ != stampQTail_) {
                        const PendingStamp &q = stampQ_[stampQHead_ % kStampQ];
                        sq[2] = q.value; sq[3] = q.seq; sq[4] = q.slot; sq[5] = q.serial;
                        if (q.slot < kMaxClientChannels) {
                            const ClientChannel &c = cchan_[q.slot];
                            sq[6] = c.state; sq[7] = c.serial; sq[8] = c.seq;
                            Ring r{};
                            if (clientRingLocked(&cchan_[q.slot], &r)) readRingSem(r, &sq[9]);
                        }
                    }
                    setProperty("NVGspControl-cchan-rc-stampq", sq, sizeof(sq));
                }
                scheduleAutoResetLocked();
            } else if (rc[1] != channelCid_) {
                // 0.164.0: the video channels have their own dead state; any
                // other channel of ours is the CE one (a CE fault used to
                // leave it "alive", every CE submit then waited it out)
                bool video = false;
                for (UInt32 i = 0; i < nvgsp::kVideoEngineCount; ++i)
                    if (nvgsp::videoEngine(i)->chid == rc[1]) {
                        video = true;
                        if (video_[i].seq && !video_[i].dead) videoMarkDeadLocked(i);
                    }
                if (!video && cePersistent_ && !ceStall_.hung) ringHungLocked(&ceStall_);
            }
        }
        // 0.89.0: POST_EVENT (NV_VGPU_MSG_EVENT_POST_EVENT 0x1003,
        // rpc_post_event_v17_00 @80): hotplug = notifyIndex 1, eventData
        // {plugDisplayMask, unplugDisplayMask} (nouveau r535_disp_hpd).
        if (rpc.function == 0x1003 && messageBytes >= 117) {
            UInt32 hEvent = 0, notifyIndex = 0, dataSize = 0, plug = 0, unplug = 0;
            __builtin_memcpy(&hEvent, entry + 84, 4);
            __builtin_memcpy(&notifyIndex, entry + 88, 4);
            __builtin_memcpy(&dataSize, entry + 104, 4);
            ++postEvents_;
            setProperty("NVGspControl-post-events", postEvents_, 32);
            setProperty("NVGspControl-post-event-last", (UInt64(hEvent) << 32) | notifyIndex, 64);
            if ((notifyIndex & 0xffff) == 1 && dataSize >= 8) {
                __builtin_memcpy(&plug, entry + 109, 4);
                __builtin_memcpy(&unplug, entry + 113, 4);
                ++hotplugs_;
                hotplugPlug_ |= plug;
                hotplugUnplug_ |= unplug;
                hotplugDeliver_ = true;
                setProperty("NVGspControl-hotplug-count", hotplugs_, 32);
                setProperty("NVGspControl-hotplug-last", (UInt64(plug) << 32) | unplug, 64);
            }
        }
        if (rpc.function == kMmuFaultQueued) {
            ++mmuFaultEvents_;
            setProperty("NVGspControl-mmu-fault-events", mmuFaultEvents_, 32);
            // 0.158.0: raw payload of the last one (layout not in the open
            // headers; kept for decoding next to the RC that follows)
            if (messageBytes > 80)
                setProperty("NVGspControl-mmu-fault-queued-last",
                            const_cast<void *>(static_cast<const void *>(entry + 80)),
                            static_cast<unsigned>(messageBytes - 80 < 64 ? messageBytes - 80 : 64));
        }
        // 0.52.0: every other GSP event (0x1000..0x1fff, e.g. 0x100F
        // PERF_BRIDGELESS_INFO_UPDATE which parked the 0.51.0 free) is
        // consumed and recorded; only RPC replies can be "unexpected".
        const bool otherEvent = rpc.function >= 0x1000 &&
            rpc.function < 0x2000 && rpc.function != kInitDone &&
            rpc.function != kRunCpuSequencer && rpc.function != kPostNocat;
        if (otherEvent && rpc.function != kLibosPrint &&
            rpc.function != kOsErrorLog && rpc.function != kLockdownNotice &&
            rpc.function != kRcTriggered && rpc.function != kMmuFaultQueued) {
            ++otherEvents_;
            setProperty("NVGspControl-other-events", otherEvents_, 32);
            setProperty("NVGspControl-other-event-last", rpc.function, 32);
        }
        const bool diagnosticEvent = otherEvent;
        const bool expectedPostInit =
            (rpc.function == 1 && postInitPhase_ == 1) ||
            (rpc.function == 64 && postInitPhase_ == 2) ||
            (rpc.function == 65 && postInitPhase_ == 3) ||
            (rpc.function == 76 && (postInitPhase_ == 5 || postInitPhase_ == 7)) ||
            (rpc.function == 76 && (postInitPhase_ == 13 || postInitPhase_ == 15)) ||
            (rpc.function == 76 && (postInitPhase_ == 34 || postInitPhase_ == 35 ||
                                    postInitPhase_ == 36 || postInitPhase_ == 37 ||
                                    postInitPhase_ == 38 || postInitPhase_ == 49)) ||
            (rpc.function == 76 && (postInitPhase_ == 52 || postInitPhase_ == 53)) ||
            (rpc.function == 76 && (postInitPhase_ == 54 || postInitPhase_ == 55)) ||
            (rpc.function == 76 && postInitPhase_ == 56) ||
            (rpc.function == 76 && postInitPhase_ == 57) ||
            (rpc.function == 76 && postInitPhase_ >= 58 &&
             postInitPhase_ <= 63) ||
            (rpc.function == 76 && postInitPhase_ == 69) ||
            (rpc.function == 76 && postInitPhase_ == 70) ||
            (rpc.function == 76 && postInitPhase_ == 71) ||
            (rpc.function == 76 && postInitPhase_ == 72) ||
            (rpc.function == 76 && (postInitPhase_ == 77 ||
                                    postInitPhase_ == 78 ||
                                    postInitPhase_ == 79 ||
                                    postInitPhase_ == 80 ||
                                    postInitPhase_ == 81 ||
                                    postInitPhase_ == 82 ||
                                    postInitPhase_ == 83 ||
                                    postInitPhase_ == 84 ||
                                    postInitPhase_ == 85 ||
                                    postInitPhase_ == 86 ||
                                    postInitPhase_ == 87)) ||
            (rpc.function == 76 && postInitPhase_ == 88) ||
            (rpc.function == 76 && postInitPhase_ == 89) ||
            (rpc.function == 76 && postInitPhase_ == 90) ||
            (rpc.function == 76 && postInitPhase_ == 91) ||
            (rpc.function == 76 && postInitPhase_ == 92) ||
            (rpc.function == 76 && postInitPhase_ == 93) ||
            (rpc.function == 76 && postInitPhase_ == 94) ||
            (rpc.function == 76 && postInitPhase_ == 95) ||
            (rpc.function == 76 && postInitPhase_ == 96) ||
            (rpc.function == 76 && postInitPhase_ == 97) ||
            (rpc.function == 76 && postInitPhase_ == 98) ||
            (rpc.function == 76 && postInitPhase_ == 99) ||
            (rpc.function == 76 && postInitPhase_ == 100) ||
            (rpc.function == 76 && postInitPhase_ == 101) ||
            (rpc.function == 76 && postInitPhase_ == 102) ||
            (rpc.function == 76 && postInitPhase_ == 103) ||
            (rpc.function == 76 && postInitPhase_ == 104) ||
            (rpc.function == 76 && postInitPhase_ == 105) ||
            (rpc.function == 76 && postInitPhase_ == 106) ||
            (rpc.function == 76 && postInitPhase_ == 107) ||
            (rpc.function == 76 && postInitPhase_ == 108) ||
            (rpc.function == 76 && postInitPhase_ == 109) ||
            (rpc.function == 76 && postInitPhase_ == 110) ||
            (rpc.function == 76 && postInitPhase_ == 111) ||
            (rpc.function == 76 && postInitPhase_ == 115) ||
            (rpc.function == 76 && postInitPhase_ == 116) ||
            (rpc.function == 76 && postInitPhase_ == 117) ||
            (rpc.function == 76 && postInitPhase_ == 118) ||
            (rpc.function == 76 && postInitPhase_ == 121) ||
            (rpc.function == 103 && postInitPhase_ == 122) ||
            (rpc.function == 76 && postInitPhase_ == 123) ||
            (rpc.function == 103 && postInitPhase_ == 124) ||
            (rpc.function == 103 && postInitPhase_ == 126) ||
            (rpc.function == 103 && postInitPhase_ == 200) ||
            (rpc.function == 76 && postInitPhase_ == 201) ||
            (rpc.function == 103 && postInitPhase_ == 202) ||
            (rpc.function == 103 && postInitPhase_ == 203) ||
            (rpc.function == 76 && postInitPhase_ == 207) ||
            (rpc.function == 103 && postInitPhase_ == 208) ||
            (rpc.function == 103 && postInitPhase_ == 209) ||
            (rpc.function == 103 && postInitPhase_ == 210) ||
            (rpc.function == 103 && postInitPhase_ == 212) ||
            (rpc.function == 76 && postInitPhase_ >= 213 &&
             postInitPhase_ <= 215) ||
            (rpc.function == 103 && postInitPhase_ == 216) ||
            (rpc.function == 103 && postInitPhase_ >= 218 &&
             postInitPhase_ <= 220) ||
            (rpc.function == 76 && postInitPhase_ >= 221 && postInitPhase_ <= 224) ||
            (rpc.function == 76 && postInitPhase_ >= 204 &&
             postInitPhase_ <= 206) ||
            (rpc.function == 97 && postInitPhase_ == 73) ||
            (rpc.function == 97 && postInitPhase_ == 74) ||
            (rpc.function == 103 && postInitPhase_ == 75) ||
            (rpc.function == 103 && postInitPhase_ == 76) ||
            (rpc.function == 103 && postInitPhase_ == 9) ||
            (rpc.function == 10 && postInitPhase_ == 11) ||
            (rpc.function == 103 && (postInitPhase_ == 17 || postInitPhase_ == 19)) ||
            (rpc.function == 103 && (postInitPhase_ == 20 || postInitPhase_ == 23 ||
                                     postInitPhase_ == 26 || postInitPhase_ == 29)) ||
            (rpc.function == 103 && (postInitPhase_ == 42 || postInitPhase_ == 43)) ||
            (rpc.function == 103 && (postInitPhase_ == 50 || postInitPhase_ == 51)) ||
            (rpc.function == 103 && postInitPhase_ == 46) ||
            (rpc.function == 103 && postInitPhase_ == 47) ||
            (rpc.function == 103 && postInitPhase_ == 48) ||
            (rpc.function == 103 && postInitPhase_ >= 64 &&
             postInitPhase_ <= 69) ||
            (rpc.function == 14 && postInitPhase_ == 30) ||
            (rpc.function == 15 && postInitPhase_ == 31) ||
            (rpc.function == 10 && postInitPhase_ == 32) ||
            (rpc.function == 76 && postInitPhase_ == 33 && pingOutstanding_) ||
            (postInitPhase_ == 33 && userRpcOutstanding_ &&
             rpc.function == userRpcFunction_) ||
            (postInitPhase_ == 33 && rpc.function == 71);   // 0.100.0 continuation
        // 0.80.0: generic user RPC reply (raw status-queue entry copy).
        // 0.178.9: and the request's sequence when the reply carries one; a
        // reply to some other RPC with the same function used to complete it
        // (short reply, client channel open failed at stage 12)
        const bool seqOk = !rpc.sequence || rpc.sequence == userRpcSeq_;
        if (postInitPhase_ == 33 && userRpcOutstanding_ && rpc.function == userRpcFunction_ && !seqOk) {
            ++userRpcSeqMismatch_;
            setProperty("NVGspControl-user-rpc-seq-mismatch", userRpcSeqMismatch_, 32);
        }
        if (postInitPhase_ == 33 && userRpcOutstanding_ && rpc.sequence)
            setProperty("NVGspControl-user-rpc-seq-echo", true);
        if (postInitPhase_ == 33 && userRpcOutstanding_ &&
            rpc.function == userRpcFunction_ && seqOk) {
            const UInt32 n = messageBytes < sizeof(userRpcReply_)
                ? static_cast<UInt32>(messageBytes) : sizeof(userRpcReply_);
            __builtin_memcpy(userRpcReply_, entry, n);
            userRpcReplyBytes_ = n;
            userRpcResult_ = rpc.result;
            userRpcOutstanding_ = false;
        }
        // 0.71.0: ping reply (P-state read) at phase 33.
        if (rpc.function == 76 && postInitPhase_ == 33 && pingOutstanding_) {
            UInt64 now = 0;
            clock_get_uptime(&now);
            pingDoneAt_ = now;
            pingViaIntr_ = inIntr_;
            pingOutstanding_ = false;
        }
        if (rpc.function != kPostNocat && rpc.function != kInitDone &&
            !(rpc.function == kRunCpuSequencer && sequencerOk) &&
            !diagnosticEvent && !expectedPostInit) {
            blockedFunction = rpc.function;
            blockedBytes = static_cast<UInt32>(messageBytes);
            blockedData = static_cast<UInt8 *>(IOMalloc(blockedBytes));
            if (blockedData) {
                for (UInt32 offset = 0; offset < blockedBytes; ++offset) {
                    const UInt32 slot = (readPtr + offset / 4096) % header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    blockedData[offset] = part ? part[offset % 4096] : 0;
                }
            }
            break;
        }
        statusSequence_++;
        if (!profFirst_) profFirst_ = mach_absolute_time();
        ++profRecords_;
        readPtr = (readPtr + q.elementCount) % header.msgCount;
        consumed += q.elementCount;
        idleWaits = 0;
        if (!init_.publishStatusReadPtr(readPtr)) { badReason = 7; break; }
        if (rpc.function == kInitDone) {
            initDone = true;
            initResult = rpc.result;
            initPrivateResult = rpc.privateResult;
            initDone_ = true;
            markBoot("init-done");
            if (!bar1Finished_) bar1EarlyPending_ = true;   // 0.146.6
            initResult_ = rpc.result;
            initPrivateResult_ = rpc.privateResult;
        }
        if (rpc.function == 1 && postInitPhase_ == 1) {
            guestInfoResponse = rpc.result == 0 && rpc.privateResult == 0;
            setProperty("NVGspControl-guest-info-result", rpc.result, 32);
            setProperty("NVGspControl-guest-info-private-result", rpc.privateResult, 32);
            if (rpc.length >= sizeof(rpc) + 8) {
                UInt32 negotiatedMajor = 0, negotiatedMinor = 0;
                __builtin_memcpy(&negotiatedMajor,
                    entry + sizeof(q) + sizeof(rpc), 4);
                __builtin_memcpy(&negotiatedMinor,
                    entry + sizeof(q) + sizeof(rpc) + 4, 4);
                setProperty("NVGspControl-guest-info-major", negotiatedMajor, 32);
                setProperty("NVGspControl-guest-info-minor", negotiatedMinor, 32);
            }
        }
        if (rpc.function == 64 && postInitPhase_ == 2) {
            // Some bare-metal GSP-RM images omit the vGPU-oriented extension
            // handler. NV_ERR_INVALID_FUNCTION is an explicit compatibility
            // response; the successful base handshake remains authoritative.
            guestInfoExtResponse = (rpc.result == 0 && rpc.privateResult == 0) ||
                rpc.result == 0x2a;
            setProperty("NVGspControl-guest-info-ext-result", rpc.result, 32);
            setProperty("NVGspControl-guest-info-ext-private-result",
                        rpc.privateResult, 32);
        }
        if (rpc.function == 65 && postInitPhase_ == 3) {
            staticInfoResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                rpc.length >= sizeof(rpc) + kStaticInfoBytes;
            if (staticInfoResponse) {
                const UInt32 payloadOffset = sizeof(q) + sizeof(rpc);
                for (UInt32 offset = 0; offset < kStaticInfoBytes; ++offset) {
                    const UInt32 absolute = payloadOffset + offset;
                    const UInt32 slot = (summary.ringIndex + absolute / 4096) % header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    staticInfo[offset] = part ? part[absolute % 4096] : 0;
                }
            }
        }
        if (rpc.function == 76 && postInitPhase_ == 5) {
            const UInt32 assembledBytes = static_cast<UInt32>(messageBytes);
            UInt32 controlClient = 0, controlObject = 0, controlCommand = 0;
            UInt32 controlFlags = 0;
            bool controlHeaderPresent = false, paramsInBounds = false;
            setProperty("NVGspControl-device-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-device-rpc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-device-rpc-length", rpc.length, 32);
            setProperty("NVGspControl-device-record-bytes", assembledBytes, 32);
            UInt8 *assembled = static_cast<UInt8 *>(IOMalloc(assembledBytes));
            if (assembled) {
                for (UInt32 offset = 0; offset < assembledBytes; ++offset) {
                    const UInt32 slot = (summary.ringIndex + offset / 4096) % header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    assembled[offset] = part ? part[offset % 4096] : 0;
                }
                const UInt32 headBytes = assembledBytes < 232
                    ? assembledBytes - (assembledBytes >= 80 ? 80 : assembledBytes)
                    : 152;
                if (headBytes)
                    setProperty("NVGspControl-device-payload-head",
                                assembled + 80, headBytes);
                if (assembledBytes >= 104) {
                    controlHeaderPresent = true;
                    __builtin_memcpy(&controlClient, assembled + 80, 4);
                    __builtin_memcpy(&controlObject, assembled + 84, 4);
                    __builtin_memcpy(&controlCommand, assembled + 88, 4);
                    __builtin_memcpy(&deviceInfoStatus, assembled + 92, 4);
                    __builtin_memcpy(&deviceInfoBytes, assembled + 96, 4);
                    __builtin_memcpy(&controlFlags, assembled + 100, 4);
                    paramsInBounds = deviceInfoBytes <= assembledBytes - 104;
                    if (paramsInBounds && deviceInfoBytes) {
                        deviceInfo = static_cast<UInt8 *>(IOMalloc(deviceInfoBytes));
                        if (deviceInfo)
                            __builtin_memcpy(deviceInfo, assembled + 104,
                                             deviceInfoBytes);
                    }
                }
                IOFree(assembled, assembledBytes);
            }
            setProperty("NVGspControl-device-control-header-present",
                        controlHeaderPresent);
            setProperty("NVGspControl-device-control-client", controlClient, 32);
            setProperty("NVGspControl-device-control-object", controlObject, 32);
            setProperty("NVGspControl-device-control-command", controlCommand, 32);
            setProperty("NVGspControl-device-control-status", deviceInfoStatus, 32);
            setProperty("NVGspControl-device-control-params-bytes",
                        deviceInfoBytes, 32);
            setProperty("NVGspControl-device-control-flags", controlFlags, 32);
            setProperty("NVGspControl-device-control-params-in-bounds",
                        paramsInBounds);
            setProperty("NVGspControl-device-control-params-allocated",
                        deviceInfo != nullptr);
            deviceInfoResponse = rpc.result == 0 && deviceInfoStatus == 0 &&
                deviceInfo && deviceInfoBytes >= 4;
        }
        if (rpc.function == 76 && postInitPhase_ == 7) {
            const UInt32 assembledBytes = static_cast<UInt32>(messageBytes);
            UInt8 *assembled = static_cast<UInt8 *>(IOMalloc(assembledBytes));
            if (assembled) {
                for (UInt32 offset = 0; offset < assembledBytes; ++offset) {
                    const UInt32 slot = (summary.ringIndex + offset / 4096) % header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    assembled[offset] = part ? part[offset % 4096] : 0;
                }
                UInt32 paramsBytes = 0, flags = 0;
                if (assembledBytes >= 104) {
                    __builtin_memcpy(&classListStatus, assembled + 92, 4);
                    __builtin_memcpy(&paramsBytes, assembled + 96, 4);
                    __builtin_memcpy(&flags, assembled + 100, 4);
                    if (paramsBytes == sizeof(classList) &&
                        paramsBytes <= assembledBytes - 104)
                        __builtin_memcpy(classList, assembled + 104,
                                         sizeof(classList));
                }
                setProperty("NVGspControl-class-list-rpc-result", rpc.result, 32);
                setProperty("NVGspControl-class-list-rpc-private-result",
                            rpc.privateResult, 32);
                setProperty("NVGspControl-class-list-status", classListStatus, 32);
                setProperty("NVGspControl-class-list-params-bytes", paramsBytes, 32);
                setProperty("NVGspControl-class-list-flags", flags, 32);
                classListResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                    classListStatus == 0 && paramsBytes == sizeof(classList);
                IOFree(assembled, assembledBytes);
            }
        }
        if (rpc.function == 103 && postInitPhase_ == 9) {
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 112) {
                UInt8 allocHead[32]{};
                for (UInt32 offset = 0; offset < sizeof(allocHead); ++offset) {
                    const UInt32 absolute = 80 + offset;
                    const UInt32 slot = (summary.ringIndex + absolute / 4096) % header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    allocHead[offset] = part ? part[absolute % 4096] : 0;
                }
                __builtin_memcpy(&allocStatus, allocHead + 16, 4);
                __builtin_memcpy(&paramsBytes, allocHead + 20, 4);
                __builtin_memcpy(&flags, allocHead + 24, 4);
                setProperty("NVGspControl-client-alloc-head", allocHead,
                            sizeof(allocHead));
            }
            setProperty("NVGspControl-client-alloc-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-client-alloc-rpc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-client-alloc-status", allocStatus, 32);
            setProperty("NVGspControl-client-alloc-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-client-alloc-flags", flags, 32);
            clientAllocResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0;
        }
        if (rpc.function == 10 && postInitPhase_ == 11) {
            UInt32 freeStatus = ~0U;
            if (messageBytes >= 96) {
                UInt8 statusBytes[4]{};
                for (UInt32 offset = 0; offset < sizeof(statusBytes); ++offset) {
                    const UInt32 absolute = 92 + offset;
                    const UInt32 slot = (summary.ringIndex + absolute / 4096) % header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    statusBytes[offset] = part ? part[absolute % 4096] : 0;
                }
                __builtin_memcpy(&freeStatus, statusBytes, 4);
            }
            setProperty("NVGspControl-client-free-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-client-free-rpc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-client-free-status", freeStatus, 32);
            clientFreeResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                freeStatus == 0;
        }
        if (rpc.function == 76 && postInitPhase_ == 13) {
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 108) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
                if (paramsBytes == sizeof(deviceIds))
                    __builtin_memcpy(&deviceIds, entry + 104, 4);
            }
            setProperty("NVGspControl-device-ids-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-device-ids-rpc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-device-ids-status", controlStatus, 32);
            setProperty("NVGspControl-device-ids-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-device-ids-flags", flags, 32);
            deviceIdsResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0 && paramsBytes == sizeof(deviceIds);
        }
        if (rpc.function == 76 && postInitPhase_ == 15) {
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104 + sizeof(attachedIds)) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
                if (paramsBytes == sizeof(attachedIds))
                    __builtin_memcpy(attachedIds, entry + 104,
                                     sizeof(attachedIds));
            }
            setProperty("NVGspControl-attached-ids-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-attached-ids-rpc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-attached-ids-status", controlStatus, 32);
            setProperty("NVGspControl-attached-ids-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-attached-ids-flags", flags, 32);
            attachedIdsResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0 && paramsBytes == sizeof(attachedIds);
        }
        if (rpc.function == 103 && (postInitPhase_ == 17 || postInitPhase_ == 19)) {
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
            }
            const bool ok = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0;
            if (postInitPhase_ == 17) {
                setProperty("NVGspControl-owned-client-rpc-result", rpc.result, 32);
                setProperty("NVGspControl-owned-client-private-result",
                            rpc.privateResult, 32);
                setProperty("NVGspControl-owned-client-status", allocStatus, 32);
                setProperty("NVGspControl-owned-client-params-bytes", paramsBytes, 32);
                setProperty("NVGspControl-owned-client-flags", flags, 32);
                ownedClientResponse = ok;
            } else {
                setProperty("NVGspControl-device-alloc-rpc-result", rpc.result, 32);
                setProperty("NVGspControl-device-alloc-private-result",
                            rpc.privateResult, 32);
                setProperty("NVGspControl-device-alloc-status", allocStatus, 32);
                setProperty("NVGspControl-device-alloc-params-bytes", paramsBytes, 32);
                setProperty("NVGspControl-device-alloc-flags", flags, 32);
                deviceAllocResponse = ok;
            }
        }
        if (rpc.function == 103 && postInitPhase_ == 20) {
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
            }
            setProperty("NVGspControl-subdevice-alloc-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-subdevice-alloc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-subdevice-alloc-status", allocStatus, 32);
            setProperty("NVGspControl-subdevice-alloc-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-subdevice-alloc-flags", flags, 32);
            subdeviceAllocResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0;
        }
        if (rpc.function == 103 && postInitPhase_ == 23) {
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[48]{};
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
                if (paramsBytes == sizeof(params) && messageBytes >= 112 + sizeof(params))
                    __builtin_memcpy(params, entry + 112, sizeof(params));
            }
            setProperty("NVGspControl-vaspace-alloc-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-vaspace-alloc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-vaspace-alloc-status", allocStatus, 32);
            setProperty("NVGspControl-vaspace-alloc-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-vaspace-alloc-flags", flags, 32);
            if (paramsBytes == sizeof(params)) {
                UInt64 vaSize = 0, vaBase = 0;
                UInt32 bigPageSize = 0;
                __builtin_memcpy(&vaSize, params + 8, 8);
                __builtin_memcpy(&bigPageSize, params + 32, 4);
                __builtin_memcpy(&vaBase, params + 40, 8);
                setProperty("NVGspControl-vaspace-alloc-returned-params",
                            params, sizeof(params));
                setProperty("NVGspControl-vaspace-size", vaSize, 64);
                setProperty("NVGspControl-vaspace-big-page-size", bigPageSize, 32);
                setProperty("NVGspControl-vaspace-base", vaBase, 64);
            }
            vaspaceAllocResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0 && paramsBytes == sizeof(params);
        }
        if (rpc.function == 103 && postInitPhase_ == 26) {
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[128]{};
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
                if (paramsBytes == sizeof(params) && messageBytes >= 112 + sizeof(params))
                    __builtin_memcpy(params, entry + 112, sizeof(params));
            }
            setProperty("NVGspControl-local-memory-alloc-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-local-memory-alloc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-local-memory-alloc-status", allocStatus, 32);
            setProperty("NVGspControl-local-memory-alloc-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-local-memory-alloc-flags", flags, 32);
            if (paramsBytes == sizeof(params)) {
                UInt32 attr = 0;
                UInt64 size = 0, offset = 0, limit = 0;
                __builtin_memcpy(&attr, params + 24, 4);
                __builtin_memcpy(&size, params + 64, 8);
                __builtin_memcpy(&offset, params + 80, 8);
                __builtin_memcpy(&limit, params + 88, 8);
                localMemoryOffset_ = offset;
                setProperty("NVGspControl-local-memory-returned-params",
                            params, sizeof(params));
                setProperty("NVGspControl-local-memory-attr", attr, 32);
                setProperty("NVGspControl-local-memory-size", size, 64);
                setProperty("NVGspControl-local-memory-offset", offset, 64);
                setProperty("NVGspControl-local-memory-limit", limit, 64);
            }
            localMemoryAllocResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0 && paramsBytes == sizeof(params);
        }
        if (rpc.function == 103 &&
            (postInitPhase_ == 42 || postInitPhase_ == 43)) {
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[128]{};
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
                if (paramsBytes == sizeof(params) && messageBytes >= 112 + sizeof(params))
                    __builtin_memcpy(params, entry + 112, sizeof(params));
            }
            const bool isGpfifo = (postInitPhase_ == 42);
            setProperty(isGpfifo ? "NVGspControl-gpfifo-backing-rpc-result"
                                 : "NVGspControl-userd-backing-rpc-result",
                        rpc.result, 32);
            setProperty(isGpfifo ? "NVGspControl-gpfifo-backing-private-result"
                                 : "NVGspControl-userd-backing-private-result",
                        rpc.privateResult, 32);
            setProperty(isGpfifo ? "NVGspControl-gpfifo-backing-status"
                                 : "NVGspControl-userd-backing-status",
                        allocStatus, 32);
            setProperty(isGpfifo ? "NVGspControl-gpfifo-backing-params-bytes"
                                 : "NVGspControl-userd-backing-params-bytes",
                        paramsBytes, 32);
            setProperty(isGpfifo ? "NVGspControl-gpfifo-backing-flags"
                                 : "NVGspControl-userd-backing-flags",
                        flags, 32);
            if (paramsBytes == sizeof(params)) {
                UInt64 size = 0, offset = 0, limit = 0;
                __builtin_memcpy(&size, params + 64, 8);
                __builtin_memcpy(&offset, params + 80, 8);
                __builtin_memcpy(&limit, params + 88, 8);
                if (isGpfifo) {
                    gpfifoBackingOffset_ = offset;
                    gpfifoBackingSize_ = size;
                } else {
                    userdBackingOffset_ = offset;
                    userdBackingSize_ = size;
                }
                setProperty(isGpfifo ? "NVGspControl-gpfifo-backing-returned-params"
                                     : "NVGspControl-userd-backing-returned-params",
                            params, sizeof(params));
                setProperty(isGpfifo ? "NVGspControl-gpfifo-backing-size"
                                     : "NVGspControl-userd-backing-size",
                            size, 64);
                setProperty(isGpfifo ? "NVGspControl-gpfifo-backing-offset"
                                     : "NVGspControl-userd-backing-offset",
                            offset, 64);
                setProperty(isGpfifo ? "NVGspControl-gpfifo-backing-limit"
                                     : "NVGspControl-userd-backing-limit",
                            limit, 64);
            }
            const bool ok = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0 && paramsBytes == sizeof(params);
            if (isGpfifo)
                gpfifoBackingAllocResponse = ok && gpfifoBackingSize_ == 4096;
            else
                userdBackingAllocResponse = ok && userdBackingSize_ == 4096;
        }
        if (rpc.function == 76 && postInitPhase_ == 49) {
            methodSizeAnswered = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt32 bytes = 0;
            if (messageBytes >= 108) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
                if (paramsBytes == 4)
                    __builtin_memcpy(&bytes, entry + 104, 4);
            }
            methodBufferBytes_ = bytes;
            setProperty("NVGspControl-method-buffer-size-rpc-result",
                        rpc.result, 32);
            setProperty("NVGspControl-method-buffer-size-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-method-buffer-size-status",
                        controlStatus, 32);
            setProperty("NVGspControl-method-buffer-size-flags", flags, 32);
            setProperty("NVGspControl-method-buffer-bytes", bytes, 32);
            methodSizeResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0 && paramsBytes == 4 && bytes > 0 &&
                bytes <= (16U * 1024U * 1024U);
        }
        if (rpc.function == 103 &&
            (postInitPhase_ == 50 || postInitPhase_ == 51)) {
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[128]{};
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
                if (paramsBytes == sizeof(params) &&
                    messageBytes >= 112 + sizeof(params))
                    __builtin_memcpy(params, entry + 112, sizeof(params));
            }
            const bool isInstance = postInitPhase_ == 50;
            if (isInstance)
                instanceBackingAnswered = true;
            else
                methodBackingAnswered = true;
            UInt64 size = 0, offset = 0, limit = 0;
            if (paramsBytes == sizeof(params)) {
                __builtin_memcpy(&size, params + 64, 8);
                __builtin_memcpy(&offset, params + 80, 8);
                __builtin_memcpy(&limit, params + 88, 8);
                if (isInstance) {
                    instanceBackingSize_ = size;
                    instanceBackingOffset_ = offset;
                } else {
                    methodBackingSize_ = size;
                    methodBackingOffset_ = offset;
                }
            }
            const char *prefix = isInstance ? "instance" : "method";
            setProperty(isInstance ? "NVGspControl-instance-backing-rpc-result"
                                   : "NVGspControl-method-backing-rpc-result",
                        rpc.result, 32);
            setProperty(isInstance ? "NVGspControl-instance-backing-private-result"
                                   : "NVGspControl-method-backing-private-result",
                        rpc.privateResult, 32);
            setProperty(isInstance ? "NVGspControl-instance-backing-status"
                                   : "NVGspControl-method-backing-status",
                        allocStatus, 32);
            setProperty(isInstance ? "NVGspControl-instance-backing-flags"
                                   : "NVGspControl-method-backing-flags",
                        flags, 32);
            setProperty(isInstance ? "NVGspControl-instance-backing-size"
                                   : "NVGspControl-method-backing-size",
                        size, 64);
            setProperty(isInstance ? "NVGspControl-instance-backing-offset"
                                   : "NVGspControl-method-backing-offset",
                        offset, 64);
            setProperty(isInstance ? "NVGspControl-instance-backing-limit"
                                   : "NVGspControl-method-backing-limit",
                        limit, 64);
            (void)prefix;
            const bool ok = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0 && paramsBytes == sizeof(params) &&
                (offset & 0xfffULL) == 0;
            if (isInstance)
                instanceBackingAllocResponse = ok && size == 4096;
            else
                methodBackingAllocResponse = ok &&
                    size >= methodBufferBytes_;
        }
        if (rpc.function == 103 && postInitPhase_ == 46) {
            channelAllocReturned = true;
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[368]{};
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
                if (paramsBytes == sizeof(params) && messageBytes >= 112 + sizeof(params))
                    __builtin_memcpy(params, entry + 112, sizeof(params));
            }
            setProperty("NVGspControl-channel-alloc-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-channel-alloc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-channel-alloc-status", allocStatus, 32);
            setProperty("NVGspControl-channel-alloc-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-channel-alloc-flags", flags, 32);
            if (paramsBytes == sizeof(params)) {
                UInt32 cid = 0, engine = 0;
                UInt64 instBase = 0, instSize = 0, userdBase = 0, ramfcBase = 0;
                UInt64 mthdBase = 0;
                __builtin_memcpy(&engine, params + 128, 4);
                __builtin_memcpy(&cid, params + 132, 4);
                __builtin_memcpy(&instBase, params + 144, 8);
                __builtin_memcpy(&instSize, params + 152, 8);
                __builtin_memcpy(&userdBase, params + 168, 8);
                __builtin_memcpy(&ramfcBase, params + 192, 8);
                __builtin_memcpy(&mthdBase, params + 216, 8);
                UInt32 tsg = 0;
                __builtin_memcpy(&tsg, params + 240, 4);
                channelTsgHandle_ = tsg;
                setProperty("NVGspControl-channel-tsg-handle", tsg, 32);
                channelCid_ = cid;
                setProperty("NVGspControl-channel-returned-params",
                            params, sizeof(params));
                setProperty("NVGspControl-channel-engine", engine, 32);
                setProperty("NVGspControl-channel-cid", cid, 32);
                setProperty("NVGspControl-channel-instance-base", instBase, 64);
                setProperty("NVGspControl-channel-instance-size", instSize, 64);
                setProperty("NVGspControl-channel-userd-base", userdBase, 64);
                setProperty("NVGspControl-channel-ramfc-base", ramfcBase, 64);
                setProperty("NVGspControl-channel-mthdbuf-base", mthdBase, 64);
            }
            channelAllocResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0 && paramsBytes == sizeof(params);
        }
        // 0.7.1: BIND (52) + GPFIFO_SCHEDULE (53) decoders. Lenient on
        // echo length (server echo shape unproven); strict on statuses.
        if (rpc.function == 76 &&
            (postInitPhase_ == 52 || postInitPhase_ == 53)) {
            const bool isBind = postInitPhase_ == 52;
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
            }
            setProperty(isBind ? "NVGspControl-channel-bind-rpc-result"
                               : "NVGspControl-channel-schedule-rpc-result",
                        rpc.result, 32);
            setProperty(isBind ? "NVGspControl-channel-bind-private-result"
                               : "NVGspControl-channel-schedule-private-result",
                        rpc.privateResult, 32);
            setProperty(isBind ? "NVGspControl-channel-bind-status"
                               : "NVGspControl-channel-schedule-status",
                        controlStatus, 32);
            setProperty(isBind ? "NVGspControl-channel-bind-params-bytes"
                               : "NVGspControl-channel-schedule-params-bytes",
                        paramsBytes, 32);
            setProperty(isBind ? "NVGspControl-channel-bind-flags"
                               : "NVGspControl-channel-schedule-flags",
                        flags, 32);
            const bool ok = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0;
            if (isBind) {
                bindReturned = true;
                bindResponse = ok;
            } else {
                scheduleReturned = true;
                scheduleResponse = ok;
            }
        }
        // 0.7.2: TSG-level BIND (54) + GPFIFO_SCHEDULE (55) decoders.
        if (rpc.function == 76 &&
            (postInitPhase_ == 54 || postInitPhase_ == 55)) {
            const bool isTsgBind = postInitPhase_ == 54;
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
            }
            setProperty(isTsgBind ? "NVGspControl-tsg-bind-rpc-result"
                                  : "NVGspControl-tsg-schedule-rpc-result",
                        rpc.result, 32);
            setProperty(isTsgBind ? "NVGspControl-tsg-bind-private-result"
                                  : "NVGspControl-tsg-schedule-private-result",
                        rpc.privateResult, 32);
            setProperty(isTsgBind ? "NVGspControl-tsg-bind-status"
                                  : "NVGspControl-tsg-schedule-status",
                        controlStatus, 32);
            setProperty(isTsgBind ? "NVGspControl-tsg-bind-params-bytes"
                                  : "NVGspControl-tsg-schedule-params-bytes",
                        paramsBytes, 32);
            setProperty(isTsgBind ? "NVGspControl-tsg-bind-flags"
                                  : "NVGspControl-tsg-schedule-flags",
                        flags, 32);
            const bool ok = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0;
            if (isTsgBind) {
                tsgBindReturned = true;
                tsgBindResponse = ok;
            } else {
                tsgScheduleReturned = true;
                tsgScheduleResponse = ok;
            }
        }
        // 0.7.3: TSG GET_INFO (56) decoder. Output-only 4-byte tsgID.
        if (rpc.function == 76 && postInitPhase_ == 56) {
            tsgInfoReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt32 hwId = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
                if (paramsBytes == 4 && messageBytes >= 108)
                    __builtin_memcpy(&hwId, entry + 104, 4);
            }
            tsgHwId_ = hwId;
            setProperty("NVGspControl-tsg-info-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-tsg-info-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-tsg-info-status", controlStatus, 32);
            setProperty("NVGspControl-tsg-info-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-tsg-info-flags", flags, 32);
            setProperty("NVGspControl-tsg-hw-id", hwId, 32);
            tsgInfoResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0 && paramsBytes == 4;
        }
        // 0.7.5: TSG SET_TIMESLICE (57) decoder. Same wire shape as 55.
        if (rpc.function == 76 && postInitPhase_ == 57) {
            tsgTimesliceReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
            }
            setProperty("NVGspControl-tsg-timeslice-rpc-result",
                        rpc.result, 32);
            setProperty("NVGspControl-tsg-timeslice-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-tsg-timeslice-status",
                        controlStatus, 32);
            setProperty("NVGspControl-tsg-timeslice-params-bytes",
                        paramsBytes, 32);
            setProperty("NVGspControl-tsg-timeslice-flags", flags, 32);
            tsgTimesliceResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0;
        }
        // 0.8.0: FIFO intel + token + flush decoders (58-63). Every read is
        // bounds-checked against messageBytes; short echoes still yield
        // their status codes.
        if (rpc.function == 76 &&
            postInitPhase_ >= 58 && postInitPhase_ <= 63) {
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
            }
            const bool ok = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0;
            if (postInitPhase_ == 58) {
                fifoInfoReturned = true;
                fifoInfoResponse = ok;
                UInt32 groups = 0, chram = 0;
                if (messageBytes >= 104 + 20) {
                    __builtin_memcpy(&groups, entry + 104 + 8, 4);
                    __builtin_memcpy(&chram, entry + 104 + 16, 4);
                }
                setProperty("NVGspControl-fifo-info-status",
                            controlStatus, 32);
                setProperty("NVGspControl-fifo-info-groups-in-use",
                            groups, 32);
                setProperty("NVGspControl-fifo-info-per-runlist-chram",
                            chram, 32);
            } else if (postInitPhase_ == 59) {
                userdLocReturned = true;
                userdLocResponse = ok;
                UInt32 aperture = 0, attribute = 0;
                if (messageBytes >= 104 + 8) {
                    __builtin_memcpy(&aperture, entry + 104, 4);
                    __builtin_memcpy(&attribute, entry + 104 + 4, 4);
                }
                setProperty("NVGspControl-userd-loc-status",
                            controlStatus, 32);
                setProperty("NVGspControl-userd-loc-aperture",
                            aperture, 32);
                setProperty("NVGspControl-userd-loc-attribute",
                            attribute, 32);
            } else if (postInitPhase_ == 60) {
                partnerReturned = true;
                partnerResponse = ok;
                UInt32 numPartners = 0, partner0 = 0;
                if (messageBytes >= 104 + 20) {
                    __builtin_memcpy(&numPartners, entry + 104 + 12, 4);
                    __builtin_memcpy(&partner0, entry + 104 + 16, 4);
                }
                setProperty("NVGspControl-partner-status",
                            controlStatus, 32);
                setProperty("NVGspControl-partner-count",
                            numPartners, 32);
                setProperty("NVGspControl-partner-first",
                            partner0, 32);
            } else if (postInitPhase_ == 61) {
                priBaseReturned = true;
                priBaseResponse = ok;
                UInt32 priBase0 = 0, runlistId0 = 0;
                if (messageBytes >= 104 + 676) {
                    __builtin_memcpy(&priBase0, entry + 104 + 336, 4);
                    __builtin_memcpy(&runlistId0, entry + 104 + 672, 4);
                }
                setProperty("NVGspControl-pribase-status",
                            controlStatus, 32);
                setProperty("NVGspControl-pribase-gr-base",
                            priBase0, 32);
                setProperty("NVGspControl-pribase-gr-runlist",
                            runlistId0, 32);
            } else if (postInitPhase_ == 62) {
                tokenReturned = true;
                tokenResponse = ok;
                UInt32 token = 0;
                if (messageBytes >= 104 + 4)
                    __builtin_memcpy(&token, entry + 104, 4);
                setProperty("NVGspControl-submit-token-status",
                            controlStatus, 32);
                setProperty("NVGspControl-submit-token", token, 32);
            } else {
                flushReturned = true;
                flushResponse = ok;
                setProperty("NVGspControl-dma-flush-status",
                            controlStatus, 32);
            }
            // Per-phase echo length (rpc/private fold into ok; both have
            // been zero on every control in every cycle to date).
            if (postInitPhase_ == 58)
                setProperty("NVGspControl-fifo-info-params-bytes",
                            paramsBytes, 32);
            else if (postInitPhase_ == 59)
                setProperty("NVGspControl-userd-loc-params-bytes",
                            paramsBytes, 32);
            else if (postInitPhase_ == 60)
                setProperty("NVGspControl-partner-params-bytes",
                            paramsBytes, 32);
            else if (postInitPhase_ == 61)
                setProperty("NVGspControl-pribase-params-bytes",
                            paramsBytes, 32);
            else if (postInitPhase_ == 62)
                setProperty("NVGspControl-submit-token-params-bytes",
                            paramsBytes, 32);
            else
                setProperty("NVGspControl-dma-flush-params-bytes",
                            paramsBytes, 32);
        }
        // 0.8.1: display alloc decoders (64-67). Standard function-103
        // shape; each records its own status triple.
        if (rpc.function == 103 &&
            postInitPhase_ >= 64 && postInitPhase_ <= 68) {
            UInt32 allocStatus = ~0U, paramsBytes = 0;
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
            }
            const bool ok = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0;
            if (postInitPhase_ == 64) {
                dispReturned = true;
                dispResponse = ok;
                setProperty("NVGspControl-disp-parent-status",
                            allocStatus, 32);
                setProperty("NVGspControl-disp-parent-params-bytes",
                            paramsBytes, 32);
            } else if (postInitPhase_ == 65) {
                pbBackingReturned = true;
                UInt64 size = 0, offset = 0;
                if (paramsBytes == 128 && messageBytes >= 112 + 128) {
                    __builtin_memcpy(&size, entry + 112 + 64, 8);
                    __builtin_memcpy(&offset, entry + 112 + 80, 8);
                }
                pbBackingResponse = ok && paramsBytes == 128 && size != 0;
                if (pbBackingResponse)
                    dispPbBackingOffset_ = offset;
                setProperty("NVGspControl-pb-backing-status",
                            allocStatus, 32);
                setProperty("NVGspControl-pb-backing-offset", offset, 64);
            } else if (postInitPhase_ == 66) {
                pbCtxdmaReturned = true;
                pbCtxdmaResponse = ok && paramsBytes == 32;
                setProperty("NVGspControl-pb-ctxdma-status",
                            allocStatus, 32);
                setProperty("NVGspControl-pb-ctxdma-params-bytes",
                            paramsBytes, 32);
            } else if (postInitPhase_ == 67) {
                notifyBackingReturned = true;
                UInt64 size = 0, offset = 0;
                if (paramsBytes == 128 && messageBytes >= 112 + 128) {
                    __builtin_memcpy(&size, entry + 112 + 64, 8);
                    __builtin_memcpy(&offset, entry + 112 + 80, 8);
                }
                notifyBackingResponse = ok && paramsBytes == 128 &&
                    size != 0;
                setProperty("NVGspControl-notify-backing-status",
                            allocStatus, 32);
                setProperty("NVGspControl-notify-backing-offset",
                            offset, 64);
            } else if (postInitPhase_ == 68) {
                notifyCtxdmaReturned = true;
                notifyCtxdmaResponse = ok && paramsBytes == 32;
                setProperty("NVGspControl-notify-ctxdma-status",
                            allocStatus, 32);
                setProperty("NVGspControl-notify-ctxdma-params-bytes",
                            paramsBytes, 32);
            }
        }
        // 0.8.5/0.8.6: CPU-RM's actual split display path. The C77D object is
        // host-owned because its constructor calls osMapGPU; physical RM gets
        // only this internal subdevice control. Direct C77D allocation on GSP
        // therefore ended at NV_ERR_GENERIC after programming the physical
        // parameters, when GSP could not perform the host mapping.
        if (rpc.function == 76 && postInitPhase_ == 69) {
            dispPbProgramReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
            }
            dispPbProgramResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == sizeof(nvgsp::NvDispChannelPushbufferParams);
            setProperty("NVGspControl-disp-pb-program-rpc-result",
                        rpc.result, 32);
            setProperty("NVGspControl-disp-pb-program-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-disp-pb-program-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-pb-program-params-bytes",
                        paramsBytes, 32);
            setProperty("NVGspControl-disp-pb-program-flags", flags, 32);
        }
        if (rpc.function == 76 && postInitPhase_ == 70) {
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            dispKickFlushReturned = true;
            dispKickFlushResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 4;
            setProperty("NVGspControl-disp-kick-flush-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-kick-flush-params-bytes",
                        paramsBytes, 32);
        }
        // 0.8.9: handler-validation probes. The control status is the
        // data (0 vs refusal), so transport-OK always advances; only a
        // missing/transport-broken reply stalls into the watchdog.
        if (rpc.function == 76 && postInitPhase_ == 71) {
            dispHandlerProbe1Returned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
            }
            dispHandlerProbe1Response = rpc.result == 0 &&
                rpc.privateResult == 0 &&
                paramsBytes == sizeof(nvgsp::NvDispChannelPushbufferParams);
            setProperty("NVGspControl-disp-handler-probe1-rpc-result",
                        rpc.result, 32);
            setProperty("NVGspControl-disp-handler-probe1-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-disp-handler-probe1-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-handler-probe1-params-bytes",
                        paramsBytes, 32);
            setProperty("NVGspControl-disp-handler-probe1-flags", flags, 32);
        }
        if (rpc.function == 76 && postInitPhase_ == 72) {
            dispHandlerProbe2Returned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
            }
            dispHandlerProbe2Response = rpc.result == 0 &&
                rpc.privateResult == 0 &&
                paramsBytes == sizeof(nvgsp::NvDispChannelPushbufferParams);
            setProperty("NVGspControl-disp-handler-probe2-rpc-result",
                        rpc.result, 32);
            setProperty("NVGspControl-disp-handler-probe2-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-disp-handler-probe2-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-handler-probe2-params-bytes",
                        paramsBytes, 32);
            setProperty("NVGspControl-disp-handler-probe2-flags", flags, 32);
        }
        // 0.9.0: function-97 schedule replies. Server side is closed, so
        // the reply layout is unknown — capture rpc results plus up to 6
        // raw echo words. Any answered reply advances (data, not danger).
        if (rpc.function == 97 && postInitPhase_ == 73) {
            sched97chanReturned = true;
            UInt32 words[6];
            for (UInt32 i = 0; i < 6; ++i) words[i] = ~0U;
            for (UInt32 i = 0; i < 6; ++i) {
                const uint64_t need = 92ULL + (i + 1) * 4;
                if (messageBytes >= need)
                    __builtin_memcpy(&words[i], entry + 92 + i * 4, 4);
            }
            sched97chanResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && messageBytes >= 96;
            setProperty("NVGspControl-sched97chan-rpc-result",
                        rpc.result, 32);
            setProperty("NVGspControl-sched97chan-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-sched97chan-w0", words[0], 32);
            setProperty("NVGspControl-sched97chan-w1", words[1], 32);
            setProperty("NVGspControl-sched97chan-w2", words[2], 32);
            setProperty("NVGspControl-sched97chan-w3", words[3], 32);
            setProperty("NVGspControl-sched97chan-w4", words[4], 32);
            setProperty("NVGspControl-sched97chan-w5", words[5], 32);
        }
        if (rpc.function == 97 && postInitPhase_ == 74) {
            sched97tsgReturned = true;
            UInt32 words[6];
            for (UInt32 i = 0; i < 6; ++i) words[i] = ~0U;
            for (UInt32 i = 0; i < 6; ++i) {
                const uint64_t need = 92ULL + (i + 1) * 4;
                if (messageBytes >= need)
                    __builtin_memcpy(&words[i], entry + 92 + i * 4, 4);
            }
            sched97tsgResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && messageBytes >= 96;
            setProperty("NVGspControl-sched97tsg-rpc-result",
                        rpc.result, 32);
            setProperty("NVGspControl-sched97tsg-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-sched97tsg-w0", words[0], 32);
            setProperty("NVGspControl-sched97tsg-w1", words[1], 32);
            setProperty("NVGspControl-sched97tsg-w2", words[2], 32);
            setProperty("NVGspControl-sched97tsg-w3", words[3], 32);
            setProperty("NVGspControl-sched97tsg-w4", words[4], 32);
            setProperty("NVGspControl-sched97tsg-w5", words[5], 32);
        }
        // 0.9.1: C372 display-SW alloc (function 103, device parent,
        // NULL params — nvkms-exact). Standard 103 reply shape.
        if (rpc.function == 103 && postInitPhase_ == 75) {
            dispC372Returned = true;
            UInt32 allocStatus = ~0U, paramsBytes = 0;
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
            }
            dispC372Response = rpc.result == 0 &&
                rpc.privateResult == 0 && allocStatus == 0;
            setProperty("NVGspControl-disp-c372-status",
                        allocStatus, 32);
            setProperty("NVGspControl-disp-c372-params-bytes",
                        paramsBytes, 32);
        }
        // 0.9.2: display-common (0x73) alloc. Same 103 shape, NULL
        // params (nvkms-exact: displayCommonHandle under device).
        if (rpc.function == 103 && postInitPhase_ == 76) {
            dispCommonReturned = true;
            UInt32 allocStatus = ~0U, paramsBytes = 0;
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
            }
            dispCommonResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && allocStatus == 0;
            setProperty("NVGspControl-disp-common-status",
                        allocStatus, 32);
            setProperty("NVGspControl-disp-common-params-bytes",
                        paramsBytes, 32);
        }
        // 0.9.7: SYSTEM_GET_SUPPORTED (0x730107) reply. Echo:
        // subdev@104, displayMask@108, displayMaskDDC@112.
        if (rpc.function == 76 && postInitPhase_ == 77) {
            dispSysSupportedReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 mask = 0, maskDdc = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 116) {
                __builtin_memcpy(&mask, entry + 108, 4);
                __builtin_memcpy(&maskDdc, entry + 112, 4);
            }
            dispSysSupportedResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 12;
            setProperty("NVGspControl-disp-sys-supported-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-sys-supported-mask",
                        mask, 32);
            setProperty("NVGspControl-disp-sys-supported-mask-ddc",
                        maskDdc, 32);
        }
        // 0.9.7: SYSTEM_GET_NUM_HEADS (0x730102) reply. Echo:
        // subdev@104, flags@108, numHeads@112.
        if (rpc.function == 76 && postInitPhase_ == 78) {
            dispSysNumHeadsReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 numHeads = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 116)
                __builtin_memcpy(&numHeads, entry + 112, 4);
            dispSysNumHeadsResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 12;
            setProperty("NVGspControl-disp-sys-num-heads-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-sys-num-heads",
                        numHeads, 32);
        }
        // 0.9.7: SYSTEM_GET_ACTIVE (0x73010c) head0 reply. Echo:
        // subdev@104, head@108, flags@112, displayId@116.
        if (rpc.function == 76 && postInitPhase_ == 79) {
            dispSysActiveReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 displayId = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 120)
                __builtin_memcpy(&displayId, entry + 116, 4);
            dispSysActiveResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 16;
            setProperty("NVGspControl-disp-sys-active-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-sys-active-display-id",
                        displayId, 32);
        }
        // 0.9.8: SYSTEM_GET_ACTIVE (0x73010c) head1 reply. Same echo
        // layout as head0 (displayId@116).
        if (rpc.function == 76 && postInitPhase_ == 80) {
            dispSysActive1Returned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 displayId = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 120)
                __builtin_memcpy(&displayId, entry + 116, 4);
            dispSysActive1Response = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 16;
            setProperty("NVGspControl-disp-sys-active1-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-sys-active1-display-id",
                        displayId, 32);
        }
        // 0.9.8: SYSTEM_GET_ACTIVE (0x73010c) head2 reply.
        if (rpc.function == 76 && postInitPhase_ == 81) {
            dispSysActive2Returned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 displayId = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 120)
                __builtin_memcpy(&displayId, entry + 116, 4);
            dispSysActive2Response = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 16;
            setProperty("NVGspControl-disp-sys-active2-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-sys-active2-display-id",
                        displayId, 32);
        }
        // 0.9.8: SYSTEM_GET_ACTIVE (0x73010c) head3 reply.
        if (rpc.function == 76 && postInitPhase_ == 82) {
            dispSysActive3Returned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 displayId = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 120)
                __builtin_memcpy(&displayId, entry + 116, 4);
            dispSysActive3Response = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 16;
            setProperty("NVGspControl-disp-sys-active3-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-sys-active3-display-id",
                        displayId, 32);
        }
        // 0.9.8: SYSTEM_GET_CONNECT_STATE (0x730108) reply. Echo:
        // subdev@104, flags@108, displayMask@112, retryTimeMs@116.
        if (rpc.function == 76 && postInitPhase_ == 83) {
            dispConnectReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 connMask = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 116)
                __builtin_memcpy(&connMask, entry + 112, 4);
            dispConnectResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 16;
            setProperty("NVGspControl-disp-connect-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-connect-mask",
                        connMask, 32);
        }
        // 0.9.8: SYSTEM_GET_BOOT_DISPLAYS (0x73011e) reply. Echo:
        // subdev@104, bootDisplayMask@108.
        if (rpc.function == 76 && postInitPhase_ == 84) {
            dispBootDisplaysReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 bootMask = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 112)
                __builtin_memcpy(&bootMask, entry + 108, 4);
            dispBootDisplaysResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 8;
            setProperty("NVGspControl-disp-boot-displays-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-boot-displays-mask",
                        bootMask, 32);
        }
        // 0.9.9: SYSTEM_GET_SCANLINE (0x730104) head0 reply. Echo:
        // subdev@104, head@108, scanline@112, eye@120. 0xffffffff =
        // no valid mode on the head.
        if (rpc.function == 76 && postInitPhase_ == 85) {
            dispScanlineReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 scanline = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 116)
                __builtin_memcpy(&scanline, entry + 112, 4);
            dispScanlineResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 20;
            setProperty("NVGspControl-disp-scanline-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-scanline-head0",
                        scanline, 32);
        }
        // 0.9.9: SYSTEM_GET_VBLANK_COUNTER (0x730105) head0 reply.
        // Echo: subdev@104, head@108, hint@112, counter@116.
        if (rpc.function == 76 && postInitPhase_ == 86) {
            dispVblankReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 counter = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 120)
                __builtin_memcpy(&counter, entry + 116, 4);
            dispVblankResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 16;
            setProperty("NVGspControl-disp-vblank-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-vblank-head0",
                        counter, 32);
        }
        // 0.9.9: SYSTEM_GET_HEAD_ROUTING_MAP (0x73010b) reply. Echo:
        // subdev@104, mask@108, oldMask@112, oldMap@116, map@120.
        if (rpc.function == 76 && postInitPhase_ == 87) {
            dispRoutingReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 routeMap = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 124)
                __builtin_memcpy(&routeMap, entry + 120, 4);
            dispRoutingResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 20;
            setProperty("NVGspControl-disp-routing-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-routing-map",
                        routeMap, 32);
        }
        // 0.10.0: C372 GET_ACTIVE_VIEWPORT_POINT_IN (0xc3720104)
        // window 0 reply. Echo: base.subdev@104, windowIndex@108,
        // point.x@112, point.y@116.
        if (rpc.function == 76 && postInitPhase_ == 88) {
            dispViewportReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 pointX = 0, pointY = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 120) {
                __builtin_memcpy(&pointX, entry + 112, 4);
                __builtin_memcpy(&pointY, entry + 116, 4);
            }
            dispViewportResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 16;
            setProperty("NVGspControl-disp-viewport-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-viewport-x",
                        pointX, 32);
            setProperty("NVGspControl-disp-viewport-y",
                        pointY, 32);
        }
        // 0.11.0: DFP_GET_INFO (0x731140) display 0x200 reply. Echo:
        // subdev@104, displayId@108, flags@112, UHBR@116.
        if (rpc.function == 76 && postInitPhase_ == 89) {
            dispDfpReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 dfpFlags = 0, uhbr = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 120) {
                __builtin_memcpy(&dfpFlags, entry + 112, 4);
                __builtin_memcpy(&uhbr, entry + 116, 4);
            }
            dispDfpResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 16;
            setProperty("NVGspControl-disp-dfp-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-dfp-flags",
                        dfpFlags, 32);
            setProperty("NVGspControl-disp-dfp-uhbr",
                        uhbr, 32);
        }
        // 0.12.0: SPECIFIC_GET_EDID_V2 (0x730245) display 0x200 reply.
        // Echo: subdev@104, displayId@108, bufferSize@112, flags@116,
        // edid[2048]@120. Hash the returned bytes (FNV-1a) + capture
        // the first 32 bytes (4x u64) for the manufacturer/model check.
        if (rpc.function == 76 && postInitPhase_ == 90) {
            dispEdidReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 edidSize = 0;
            UInt64 edidHash = 1469598103934665603ULL;
            UInt64 edidW[4]{};
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 120)
                __builtin_memcpy(&edidSize, entry + 112, 4);
            if (edidSize > 0 && edidSize <= 2048 &&
                messageBytes >= 120 + edidSize) {
                for (UInt32 off = 0; off < edidSize; ++off) {
                    UInt8 byte = 0;
                    const UInt32 slot =
                        (readPtr + (120 + off) / 4096) % header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    if (part)
                        __builtin_memcpy(
                            &byte, part + ((120 + off) % 4096), 1);
                    edidHash ^= byte;
                    edidHash *= 1099511628211ULL;
                }
                const UInt32 words =
                    edidSize >= 32 ? 4 : edidSize / 8;
                for (UInt32 w = 0; w < words; ++w) {
                    UInt64 word = 0;
                    for (UInt32 b = 0; b < 8; ++b) {
                        UInt8 byte = 0;
                        const UInt32 off = w * 8 + b;
                        const UInt32 slot =
                            (readPtr + (120 + off) / 4096) %
                            header.msgCount;
                        const UInt8 *part = init_.statusEntry(slot);
                        if (part)
                            __builtin_memcpy(
                                &byte, part + ((120 + off) % 4096), 1);
                        word |= static_cast<UInt64>(byte) << (b * 8);
                    }
                    edidW[w] = word;
                }
            }
            dispEdidResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 2064 && edidSize > 0 &&
                edidSize <= 2048;
            setProperty("NVGspControl-disp-edid-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-edid-size",
                        edidSize, 32);
            setProperty("NVGspControl-disp-edid-hash",
                        edidHash, 64);
            setProperty("NVGspControl-disp-edid-w0", edidW[0], 64);
            setProperty("NVGspControl-disp-edid-w1", edidW[1], 64);
            setProperty("NVGspControl-disp-edid-w2", edidW[2], 64);
            setProperty("NVGspControl-disp-edid-w3", edidW[3], 64);
        }
        // 0.14.0: SPECIFIC_GET_PCLK_LIMIT (0x73028a) display 0x200
        // reply. Echo: subdev@104, displayId@108, pclk@112,
        // orPclk@116, vbPclk@120.
        if (rpc.function == 76 && postInitPhase_ == 91) {
            dispPclkReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 pclk = 0, orPclk = 0, vbPclk = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 124) {
                __builtin_memcpy(&pclk, entry + 112, 4);
                __builtin_memcpy(&orPclk, entry + 116, 4);
                __builtin_memcpy(&vbPclk, entry + 120, 4);
            }
            dispPclkResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 20;
            setProperty("NVGspControl-disp-pclk-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-pclk-limit",
                        pclk, 32);
            setProperty("NVGspControl-disp-pclk-or-limit",
                        orPclk, 32);
            setProperty("NVGspControl-disp-pclk-vb-limit",
                        vbPclk, 32);
        }
        // 0.14.0: SPECIFIC_OR_GET_INFO (0x73028b) display 0x200
        // index 0 reply. Echo: type@112, protocol@116, dcbIndex@140,
        // vbiosAddress@144, litByVbios@152, dispDynamic@153.
        if (rpc.function == 76 && postInitPhase_ == 92) {
            dispOrReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 orType = 0, orProto = 0, dcbIndex = 0;
            UInt8 litByVbios = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 120) {
                __builtin_memcpy(&orType, entry + 116, 4);
                __builtin_memcpy(&orProto, entry + 120, 4);
            }
            if (messageBytes >= 144)
                __builtin_memcpy(&dcbIndex, entry + 140, 4);
            if (messageBytes >= 156)
                __builtin_memcpy(&litByVbios, entry + 152, 1);
            dispOrResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 52;
            setProperty("NVGspControl-disp-or-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-or-type",
                        orType, 32);
            setProperty("NVGspControl-disp-or-protocol",
                        orProto, 32);
            setProperty("NVGspControl-disp-or-dcb-index",
                        dcbIndex, 32);
            setProperty("NVGspControl-disp-or-lit-by-vbios",
                        litByVbios, 8);
        }
        // 0.15.0: SYSTEM_GET_CAPS_V2 (0x730101) reply. Echo: 2-byte
        // caps table @104.
        if (rpc.function == 76 && postInitPhase_ == 93) {
            dispCapsReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt16 caps = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 106)
                __builtin_memcpy(&caps, entry + 104, 2);
            dispCapsResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 2;
            setProperty("NVGspControl-disp-caps-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-caps",
                        caps, 16);
        }
        // 0.15.0: SYSTEM_GET_VBLANK_ENABLE (0x730106) head0 reply.
        // Echo: subdev@104, head@108, enabled@112 (byte).
        if (rpc.function == 76 && postInitPhase_ == 94) {
            dispVbEnReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt8 enabled = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 113)
                __builtin_memcpy(&enabled, entry + 112, 1);
            dispVbEnResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 12;
            setProperty("NVGspControl-disp-vblank-en-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-vblank-en-head0",
                        enabled, 8);
        }
        // 0.16.0: C372 IS_MODE_POSSIBLE (0xc3720101) numHeads=0 reply.
        // Echo: status@92, paramsBytes@96 (2048), bIsPossible@2008.
        if (rpc.function == 76 && postInitPhase_ == 95) {
            dispImpReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt8 possible = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 2009)
                __builtin_memcpy(&possible, entry + 2008, 1);
            dispImpResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 2048;
            setProperty("NVGspControl-disp-imp-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-imp-possible",
                        possible, 8);
        }
        // 0.17.0: IS_MODE_POSSIBLE head0 4K60 reply. Same echo
        // (status@92, paramsBytes@96 = 2048, bIsPossible@2008).
        if (rpc.function == 76 && postInitPhase_ == 96) {
            dispModeReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt8 possible = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 2009)
                __builtin_memcpy(&possible, entry + 2008, 1);
            dispModeResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 2048;
            setProperty("NVGspControl-disp-mode-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-mode-possible",
                        possible, 8);
        }
        // 0.18.0: SPECIFIC_GET_CONNECTOR_DATA (0x730250) 0x200 reply.
        // Echo: flags@112, DDCPartners@116, count@120, data0
        // {index@124, type@128, location@132}, platform@172.
        if (rpc.function == 76 && postInitPhase_ == 97) {
            dispConnReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 connFlags = 0, connCount = 0;
            UInt32 connIndex = 0, connType = 0, connLoc = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 124) {
                __builtin_memcpy(&connFlags, entry + 112, 4);
                __builtin_memcpy(&connCount, entry + 120, 4);
            }
            if (messageBytes >= 136) {
                __builtin_memcpy(&connIndex, entry + 124, 4);
                __builtin_memcpy(&connType, entry + 128, 4);
                __builtin_memcpy(&connLoc, entry + 132, 4);
            }
            dispConnResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 72;
            setProperty("NVGspControl-disp-conn-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-conn-flags",
                        connFlags, 32);
            setProperty("NVGspControl-disp-conn-count",
                        connCount, 32);
            setProperty("NVGspControl-disp-conn-index0",
                        connIndex, 32);
            setProperty("NVGspControl-disp-conn-type0",
                        connType, 32);
            setProperty("NVGspControl-disp-conn-loc0",
                        connLoc, 32);
        }
        // 0.19.0: MC_GET_ARCH_INFO (0x20801701) reply. Echo:
        // arch@104, impl@108, rev@112, subRev@116 (byte).
        if (rpc.function == 76 && postInitPhase_ == 98) {
            dispArchReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 arch = 0, impl = 0, rev = 0;
            UInt8 subRev = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 116) {
                __builtin_memcpy(&arch, entry + 104, 4);
                __builtin_memcpy(&impl, entry + 108, 4);
                __builtin_memcpy(&rev, entry + 112, 4);
            }
            if (messageBytes >= 117)
                __builtin_memcpy(&subRev, entry + 116, 1);
            dispArchResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 13;
            setProperty("NVGspControl-disp-arch-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-arch",
                        arch, 32);
            setProperty("NVGspControl-disp-arch-impl",
                        impl, 32);
            setProperty("NVGspControl-disp-arch-rev",
                        rev, 32);
            setProperty("NVGspControl-disp-arch-subrev",
                        subRev, 8);
        }
        // 0.19.0: GPU_GET_NAME_STRING ASCII (0x20800110) reply.
        // Echo: flags@104, string[64]@108. Capture first 32 bytes.
        if (rpc.function == 76 && postInitPhase_ == 99) {
            dispNameReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt64 nameW[4]{};
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 140) {
                for (UInt32 w = 0; w < 4; ++w) {
                    UInt64 word = 0;
                    for (UInt32 b = 0; b < 8; ++b) {
                        UInt8 byte = 0;
                        const UInt32 off = w * 8 + b;
                        const UInt32 slot =
                            (readPtr + (108 + off) / 4096) %
                            header.msgCount;
                        const UInt8 *part = init_.statusEntry(slot);
                        if (part)
                            __builtin_memcpy(
                                &byte, part + ((108 + off) % 4096), 1);
                        word |= static_cast<UInt64>(byte) << (b * 8);
                    }
                    nameW[w] = word;
                }
            }
            dispNameResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 68;
            setProperty("NVGspControl-disp-name-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-name-w0", nameW[0], 64);
            setProperty("NVGspControl-disp-name-w1", nameW[1], 64);
            setProperty("NVGspControl-disp-name-w2", nameW[2], 64);
            setProperty("NVGspControl-disp-name-w3", nameW[3], 64);
        }
        // 0.20.0: FIFO_GET_PHYSICAL_CHANNEL_COUNT (0x20801108)
        // reply. Echo: count@104, inUse@108.
        if (rpc.function == 76 && postInitPhase_ == 100) {
            dispFifoReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 fifoCount = 0, fifoInUse = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 112) {
                __builtin_memcpy(&fifoCount, entry + 104, 4);
                __builtin_memcpy(&fifoInUse, entry + 108, 4);
            }
            dispFifoResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 8;
            setProperty("NVGspControl-disp-fifo-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-fifo-count",
                        fifoCount, 32);
            setProperty("NVGspControl-disp-fifo-in-use",
                        fifoInUse, 32);
        }
        // 0.21.0: FIFO_GET_ALLOCATED_CHANNELS (0x20801119) runlist 0
        // reply. Echo: runlistId@104, bitmask[128]@108..619.
        // 0.25.0: + words 0-7 + FNV-1a mask hash (diff pre/post bind).
        // Publish popcount + words 0-1 (channels 0-63, incl. cid 3).
        if (rpc.function == 76 && postInitPhase_ == 101) {
            dispRunlistReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 popCount = 0, word0 = 0, word1 = 0;
            UInt32 words[8]{};
            UInt64 maskHash = 1469598103934665603ULL;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 108 + 512) {
                for (UInt32 w = 0; w < 128; ++w) {
                    UInt32 word = 0;
                    const UInt32 off = w * 4;
                    const UInt32 slot =
                        (readPtr + (108 + off) / 4096) %
                        header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    if (part)
                        __builtin_memcpy(
                            &word, part + ((108 + off) % 4096), 4);
                    if (w == 0)
                        word0 = word;
                    else if (w == 1)
                        word1 = word;
                    if (w < 8)
                        words[w] = word;
                    for (UInt32 b = 0; b < 4; ++b) {
                        maskHash ^= static_cast<UInt64>(
                            (word >> (b * 8)) & 0xffU);
                        maskHash *= 1099511628211ULL;
                    }
                    while (word) {
                        popCount += word & 1U;
                        word >>= 1;
                    }
                }
            }
            dispRunlistResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 516;
            setProperty("NVGspControl-disp-runlist-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-runlist-popcount",
                        popCount, 32);
            setProperty("NVGspControl-disp-runlist-w0",
                        word0, 32);
            setProperty("NVGspControl-disp-runlist-w1",
                        word1, 32);
            setProperty("NVGspControl-disp-runlist-w2",
                        words[2], 32);
            setProperty("NVGspControl-disp-runlist-w3",
                        words[3], 32);
            setProperty("NVGspControl-disp-runlist-w4",
                        words[4], 32);
            setProperty("NVGspControl-disp-runlist-w5",
                        words[5], 32);
            setProperty("NVGspControl-disp-runlist-w6",
                        words[6], 32);
            setProperty("NVGspControl-disp-runlist-w7",
                        words[7], 32);
            setProperty("NVGspControl-disp-runlist-mask-hash",
                        maskHash, 64);
        }
        // 0.22.0: GPFIFO BIND to GR (0xa06f0104, engineType=1) reply.
        // Echo: status@92, paramsBytes@96 (4).
        if (rpc.function == 76 && postInitPhase_ == 102) {
            dispGrBindReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            dispGrBindResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 4;
            setProperty("NVGspControl-disp-grbind-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-grbind-params-bytes",
                        paramsBytes, 32);
        }
        // 0.23.0: FIFO_GET_ALLOCATED_CHANNELS re-query (runlist 0)
        // after the GR bind. Same echo (popcount + w0/w1).
        // 0.25.0: + words 2-7 + mask hash (same as phase 101).
        if (rpc.function == 76 && postInitPhase_ == 103) {
            dispReRunReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 popCount = 0, word0 = 0, word1 = 0;
            UInt32 words[8]{};
            UInt64 maskHash = 1469598103934665603ULL;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 108 + 512) {
                for (UInt32 w = 0; w < 128; ++w) {
                    UInt32 word = 0;
                    const UInt32 off = w * 4;
                    const UInt32 slot =
                        (readPtr + (108 + off) / 4096) %
                        header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    if (part)
                        __builtin_memcpy(
                            &word, part + ((108 + off) % 4096), 4);
                    if (w == 0)
                        word0 = word;
                    else if (w == 1)
                        word1 = word;
                    if (w < 8)
                        words[w] = word;
                    for (UInt32 b = 0; b < 4; ++b) {
                        maskHash ^= static_cast<UInt64>(
                            (word >> (b * 8)) & 0xffU);
                        maskHash *= 1099511628211ULL;
                    }
                    while (word) {
                        popCount += word & 1U;
                        word >>= 1;
                    }
                }
            }
            dispReRunResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 516;
            setProperty("NVGspControl-disp-rerun-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-rerun-popcount",
                        popCount, 32);
            setProperty("NVGspControl-disp-rerun-w0",
                        word0, 32);
            setProperty("NVGspControl-disp-rerun-w1",
                        word1, 32);
            setProperty("NVGspControl-disp-rerun-w2",
                        words[2], 32);
            setProperty("NVGspControl-disp-rerun-w3",
                        words[3], 32);
            setProperty("NVGspControl-disp-rerun-w4",
                        words[4], 32);
            setProperty("NVGspControl-disp-rerun-w5",
                        words[5], 32);
            setProperty("NVGspControl-disp-rerun-w6",
                        words[6], 32);
            setProperty("NVGspControl-disp-rerun-w7",
                        words[7], 32);
            setProperty("NVGspControl-disp-rerun-mask-hash",
                        maskHash, 64);
        }
        // 0.24.0: runlist samples 104 (rl0), 105 (rl1), 106 (rl2).
        // Same 516B echo (popcount + w0/w1).
        // 0.25.0: + words 2-7 + mask hash (same as phase 101).
        if ((rpc.function == 76 && postInitPhase_ == 104) ||
            (rpc.function == 76 && postInitPhase_ == 105) ||
            (rpc.function == 76 && postInitPhase_ == 106)) {
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 popCount = 0, word0 = 0, word1 = 0;
            UInt32 words[8]{};
            UInt64 maskHash = 1469598103934665603ULL;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 108 + 512) {
                for (UInt32 w = 0; w < 128; ++w) {
                    UInt32 word = 0;
                    const UInt32 off = w * 4;
                    const UInt32 slot =
                        (readPtr + (108 + off) / 4096) %
                        header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    if (part)
                        __builtin_memcpy(
                            &word, part + ((108 + off) % 4096), 4);
                    if (w == 0)
                        word0 = word;
                    else if (w == 1)
                        word1 = word;
                    if (w < 8)
                        words[w] = word;
                    for (UInt32 b = 0; b < 4; ++b) {
                        maskHash ^= static_cast<UInt64>(
                            (word >> (b * 8)) & 0xffU);
                        maskHash *= 1099511628211ULL;
                    }
                    while (word) {
                        popCount += word & 1U;
                        word >>= 1;
                    }
                }
            }
            const bool ok = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 516;
            if (postInitPhase_ == 104) {
                dispRl0cReturned = true;
                dispRl0cResponse = ok;
                setProperty("NVGspControl-disp-rl0c-status",
                            controlStatus, 32);
                setProperty("NVGspControl-disp-rl0c-popcount",
                            popCount, 32);
                setProperty("NVGspControl-disp-rl0c-w0", word0, 32);
                setProperty("NVGspControl-disp-rl0c-w1", word1, 32);
                setProperty("NVGspControl-disp-rl0c-w2", words[2], 32);
                setProperty("NVGspControl-disp-rl0c-w3", words[3], 32);
                setProperty("NVGspControl-disp-rl0c-w4", words[4], 32);
                setProperty("NVGspControl-disp-rl0c-w5", words[5], 32);
                setProperty("NVGspControl-disp-rl0c-w6", words[6], 32);
                setProperty("NVGspControl-disp-rl0c-w7", words[7], 32);
                setProperty("NVGspControl-disp-rl0c-mask-hash",
                            maskHash, 64);
            } else if (postInitPhase_ == 105) {
                dispRl1Returned = true;
                dispRl1Response = ok;
                setProperty("NVGspControl-disp-rl1-status",
                            controlStatus, 32);
                setProperty("NVGspControl-disp-rl1-popcount",
                            popCount, 32);
                setProperty("NVGspControl-disp-rl1-w0", word0, 32);
                setProperty("NVGspControl-disp-rl1-w1", word1, 32);
                setProperty("NVGspControl-disp-rl1-w2", words[2], 32);
                setProperty("NVGspControl-disp-rl1-w3", words[3], 32);
                setProperty("NVGspControl-disp-rl1-w4", words[4], 32);
                setProperty("NVGspControl-disp-rl1-w5", words[5], 32);
                setProperty("NVGspControl-disp-rl1-w6", words[6], 32);
                setProperty("NVGspControl-disp-rl1-w7", words[7], 32);
                setProperty("NVGspControl-disp-rl1-mask-hash",
                            maskHash, 64);
            } else {
                dispRl2Returned = true;
                dispRl2Response = ok;
                setProperty("NVGspControl-disp-rl2-status",
                            controlStatus, 32);
                setProperty("NVGspControl-disp-rl2-popcount",
                            popCount, 32);
                setProperty("NVGspControl-disp-rl2-w0", word0, 32);
                setProperty("NVGspControl-disp-rl2-w1", word1, 32);
                setProperty("NVGspControl-disp-rl2-w2", words[2], 32);
                setProperty("NVGspControl-disp-rl2-w3", words[3], 32);
                setProperty("NVGspControl-disp-rl2-w4", words[4], 32);
                setProperty("NVGspControl-disp-rl2-w5", words[5], 32);
                setProperty("NVGspControl-disp-rl2-w6", words[6], 32);
                setProperty("NVGspControl-disp-rl2-w7", words[7], 32);
                setProperty("NVGspControl-disp-rl2-mask-hash",
                            maskHash, 64);
            }
        }
        // 0.26.0: post-bind GPFIFO_SCHEDULE (0xa06f0103) reply.
        // Echo: status@92, paramsBytes@96 (2).
        if (rpc.function == 76 && postInitPhase_ == 107) {
            dispSchedReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            dispSchedResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 2;
            setProperty("NVGspControl-disp-sched-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-sched-params-bytes",
                        paramsBytes, 32);
        }
        // 0.26.0: rl0 re-query after the schedule attempt. Same
        // 516B echo (popcount + w0-w1 + hash only — words trimmed
        // to keep the telemetry small).
        if (rpc.function == 76 && postInitPhase_ == 108) {
            dispPostReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 popCount = 0, word0 = 0, word1 = 0;
            UInt64 maskHash = 1469598103934665603ULL;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 108 + 512) {
                for (UInt32 w = 0; w < 128; ++w) {
                    UInt32 word = 0;
                    const UInt32 off = w * 4;
                    const UInt32 slot =
                        (readPtr + (108 + off) / 4096) %
                        header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    if (part)
                        __builtin_memcpy(
                            &word, part + ((108 + off) % 4096), 4);
                    if (w == 0)
                        word0 = word;
                    else if (w == 1)
                        word1 = word;
                    for (UInt32 b = 0; b < 4; ++b) {
                        maskHash ^= static_cast<UInt64>(
                            (word >> (b * 8)) & 0xffU);
                        maskHash *= 1099511628211ULL;
                    }
                    while (word) {
                        popCount += word & 1U;
                        word >>= 1;
                    }
                }
            }
            dispPostResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 516;
            setProperty("NVGspControl-disp-post-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-post-popcount",
                        popCount, 32);
            setProperty("NVGspControl-disp-post-w0", word0, 32);
            setProperty("NVGspControl-disp-post-w1", word1, 32);
            setProperty("NVGspControl-disp-post-mask-hash",
                        maskHash, 64);
        }
        // 0.27.0: post-schedule rl1 (109) / rl2 (110) samples.
        // Same 516B echo; popcount + w0/w1 + hash.
        // 0.28.0: + words 2-7 (locate post-schedule new bits).
        if ((rpc.function == 76 && postInitPhase_ == 109) ||
            (rpc.function == 76 && postInitPhase_ == 110)) {
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 popCount = 0, word0 = 0, word1 = 0;
            UInt32 words[8]{};
            UInt64 maskHash = 1469598103934665603ULL;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 108 + 512) {
                for (UInt32 w = 0; w < 128; ++w) {
                    UInt32 word = 0;
                    const UInt32 off = w * 4;
                    const UInt32 slot =
                        (readPtr + (108 + off) / 4096) %
                        header.msgCount;
                    const UInt8 *part = init_.statusEntry(slot);
                    if (part)
                        __builtin_memcpy(
                            &word, part + ((108 + off) % 4096), 4);
                    if (w == 0)
                        word0 = word;
                    else if (w == 1)
                        word1 = word;
                    if (w < 8)
                        words[w] = word;
                    for (UInt32 b = 0; b < 4; ++b) {
                        maskHash ^= static_cast<UInt64>(
                            (word >> (b * 8)) & 0xffU);
                        maskHash *= 1099511628211ULL;
                    }
                    while (word) {
                        popCount += word & 1U;
                        word >>= 1;
                    }
                }
            }
            const bool ok = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 516;
            if (postInitPhase_ == 109) {
                dispPs1Returned = true;
                dispPs1Response = ok;
                setProperty("NVGspControl-disp-ps1-status",
                            controlStatus, 32);
                setProperty("NVGspControl-disp-ps1-popcount",
                            popCount, 32);
                setProperty("NVGspControl-disp-ps1-w0", word0, 32);
                setProperty("NVGspControl-disp-ps1-w1", word1, 32);
                setProperty("NVGspControl-disp-ps1-w2", words[2], 32);
                setProperty("NVGspControl-disp-ps1-w3", words[3], 32);
                setProperty("NVGspControl-disp-ps1-w4", words[4], 32);
                setProperty("NVGspControl-disp-ps1-w5", words[5], 32);
                setProperty("NVGspControl-disp-ps1-w6", words[6], 32);
                setProperty("NVGspControl-disp-ps1-w7", words[7], 32);
                setProperty("NVGspControl-disp-ps1-mask-hash",
                            maskHash, 64);
            } else {
                dispPs2Returned = true;
                dispPs2Response = ok;
                setProperty("NVGspControl-disp-ps2-status",
                            controlStatus, 32);
                setProperty("NVGspControl-disp-ps2-popcount",
                            popCount, 32);
                setProperty("NVGspControl-disp-ps2-w0", word0, 32);
                setProperty("NVGspControl-disp-ps2-w1", word1, 32);
                setProperty("NVGspControl-disp-ps2-w2", words[2], 32);
                setProperty("NVGspControl-disp-ps2-w3", words[3], 32);
                setProperty("NVGspControl-disp-ps2-w4", words[4], 32);
                setProperty("NVGspControl-disp-ps2-w5", words[5], 32);
                setProperty("NVGspControl-disp-ps2-w6", words[6], 32);
                setProperty("NVGspControl-disp-ps2-w7", words[7], 32);
                setProperty("NVGspControl-disp-ps2-mask-hash",
                            maskHash, 64);
            }
        }
        // 0.29.0: FIFO_GET_DEVICE_INFO_TABLE (0x20801112) reply.
        // Echo: baseIndex@104, numEntries@108, bMore@112,
        // entry0 {engineData[16]@116, pbdmaIds@180, faultIds@188,
        // numPbdmas@196, name[16]@200}.
        // 0.30.0: scan all 12 entries (name0 + pbdma0 + npbdma each)
        // to locate GR and its PBDMA ids. Entry i @ 116+i*100:
        // pbdma0 @ +64, npbdma @ +76, name @ +80.
        if (rpc.function == 76 && postInitPhase_ == 111) {
            dispDevInfoReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 numEntries = 0, numPbdmas = 0;
            UInt32 pbdmaId0 = 0, pbdmaId1 = 0;
            UInt8 more = 0;
            UInt64 nameW0 = 0, nameW1 = 0;
            UInt64 entryName[12]{};
            UInt32 entryPbdma[12]{};
            UInt32 entryCount[12]{};
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 116) {
                __builtin_memcpy(&numEntries, entry + 108, 4);
                __builtin_memcpy(&more, entry + 112, 1);
            }
            if (messageBytes >= 220) {
                __builtin_memcpy(&pbdmaId0, entry + 184, 4);
                __builtin_memcpy(&pbdmaId1, entry + 188, 4);
                __builtin_memcpy(&numPbdmas, entry + 196, 4);
                for (UInt32 b = 0; b < 8; ++b) {
                    UInt8 byte = 0;
                    __builtin_memcpy(&byte, entry + 200 + b, 1);
                    nameW0 |= static_cast<UInt64>(byte) << (b * 8);
                    __builtin_memcpy(&byte, entry + 208 + b, 1);
                    nameW1 |= static_cast<UInt64>(byte) << (b * 8);
                }
            }
            // 0.30.1: entries start at echo+120 (16B header:
            // baseIndex@104, numEntries@108, bMore@112+pad), NOT +116.
            // Entry i @ 120+i*100: pbdma0 @ +64, npbdma @ +76,
            // name @ +80. (0.30.0 loop was off by 4.)
            // 0.30.2: table is 104+3212=3316B < 4096 from message
            // start: single page, direct reads (no slot-walk).
            if (numEntries > 12)
                numEntries = 12;
            for (UInt32 e = 0; e < numEntries; ++e) {
                const UInt32 base = 120U + e * 100U;
                if (messageBytes < static_cast<uint64_t>(base) + 88)
                    break;
                UInt64 name = 0;
                UInt32 pbdma = 0, npb = 0;
                for (UInt32 b = 0; b < 8; ++b) {
                    UInt8 byte = 0;
                    __builtin_memcpy(&byte, entry + base + 80 + b, 1);
                    name |= static_cast<UInt64>(byte) << (b * 8);
                }
                __builtin_memcpy(&pbdma, entry + base + 64, 4);
                __builtin_memcpy(&npb, entry + base + 76, 4);
                entryName[e] = name;
                entryPbdma[e] = pbdma;
                entryCount[e] = npb;
            }
            dispDevInfoResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 3212;
            setProperty("NVGspControl-disp-devinfo-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-devinfo-entries",
                        numEntries, 32);
            setProperty("NVGspControl-disp-devinfo-more",
                        more, 8);
            setProperty("NVGspControl-disp-devinfo-pbdma0",
                        pbdmaId0, 32);
            setProperty("NVGspControl-disp-devinfo-pbdma1",
                        pbdmaId1, 32);
            setProperty("NVGspControl-disp-devinfo-npbdma",
                        numPbdmas, 32);
            setProperty("NVGspControl-disp-devinfo-name0",
                        nameW0, 64);
            setProperty("NVGspControl-disp-devinfo-name1",
                        nameW1, 64);
            for (UInt32 e = 0; e < 12; ++e) {
                char keyName[64]{}, keyPbdma[64]{}, keyCount[64]{};
                snprintf(keyName, sizeof(keyName),
                         "NVGspControl-disp-devinfo-e%u-name",
                         e);
                snprintf(keyPbdma, sizeof(keyPbdma),
                         "NVGspControl-disp-devinfo-e%u-pbdma",
                         e);
                snprintf(keyCount, sizeof(keyCount),
                         "NVGspControl-disp-devinfo-e%u-npbdma",
                         e);
                setProperty(keyName, entryName[e], 64);
                setProperty(keyPbdma, entryPbdma[e], 32);
                setProperty(keyCount, entryCount[e], 32);
            }
        }
        // 0.33.0: DEVICE FIFO_GET_CAPS_V2 (0x801713) reply. Echo:
        // 2-byte caps table @104.
        if (rpc.function == 76 && postInitPhase_ == 115) {
            dispFifoCapsReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt16 caps = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 106)
                __builtin_memcpy(&caps, entry + 104, 2);
            dispFifoCapsResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 2;
            setProperty("NVGspControl-disp-fifocaps-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-fifocaps",
                        caps, 16);
        }
        // 0.33.0: DEVICE GET_ENGINE_CONTEXT_PROPERTIES (0x801707)
        // GRAPHICS reply. Echo: engineId@104, alignment@108,
        // size@112.
        if (rpc.function == 76 && postInitPhase_ == 116) {
            dispCtxPropReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 align = 0, ctxSize = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 116) {
                __builtin_memcpy(&align, entry + 108, 4);
                __builtin_memcpy(&ctxSize, entry + 112, 4);
            }
            dispCtxPropResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 12;
            setProperty("NVGspControl-disp-ctxprop-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-ctxprop-align",
                        align, 32);
            setProperty("NVGspControl-disp-ctxprop-size",
                        ctxSize, 32);
        }
        // 0.34.0: GR_GET_TPC_PARTITION_MODE (0x801107) reply. Echo:
        // hTSG@104, mode@108, bEnableAllTpcs@112 (byte).
        if (rpc.function == 76 && postInitPhase_ == 117) {
            dispTpcReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            UInt32 tpcMode = 0;
            UInt8 tpcEnable = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 112)
                __builtin_memcpy(&tpcMode, entry + 108, 4);
            if (messageBytes >= 113)
                __builtin_memcpy(&tpcEnable, entry + 112, 1);
            dispTpcResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 32;
            setProperty("NVGspControl-disp-tpc-status",
                        controlStatus, 32);
            setProperty("NVGspControl-disp-tpc-mode",
                        tpcMode, 32);
            setProperty("NVGspControl-disp-tpc-enable-all",
                        tpcEnable, 8);
        }
        // 0.51.0: display probe replies.
        if (rpc.function == 103 && postInitPhase_ == 200) {
            dispInstMemReturned = true;
            UInt32 st = ~0U, pb = 0;
            UInt64 off = 0;
            if (messageBytes >= 112) {
                __builtin_memcpy(&st, entry + 96, 4);
                __builtin_memcpy(&pb, entry + 100, 4);
            }
            if (pb == 128 && messageBytes >= 112 + 128)
                __builtin_memcpy(&off, entry + 112 + 80, 8);
            dispInstMemResponse = rpc.result == 0 && st == 0 && off &&
                (off & 0xffff) == 0;
            if (dispInstMemResponse) dispInstOffset_ = off;
            setProperty("NVGspControl-dispinst-status", st, 32);
            setProperty("NVGspControl-dispinst-offset", off, 64);
        }
        if (rpc.function == 76 && postInitPhase_ == 201) {
            dispInstWriteReturned = true;
            UInt32 cs = ~0U;
            if (messageBytes >= 96) __builtin_memcpy(&cs, entry + 92, 4);
            dispInstWriteResponse = rpc.result == 0 && cs == 0;
            setProperty("NVGspControl-dispinst-write-rpc", rpc.result, 32);
            setProperty("NVGspControl-dispinst-write-status", cs, 32);
        }
        if ((rpc.function == 103 && postInitPhase_ == 203) ||
            (rpc.function == 76 && postInitPhase_ >= 204 &&
             postInitPhase_ <= 206)) {
            dispHealthReturned = true;
            UInt32 st = ~0U, v = 0;
            if (rpc.function == 103) {
                if (messageBytes >= 100) __builtin_memcpy(&st, entry + 96, 4);
                setProperty("NVGspControl-dh-common-status", st, 32);
            } else {
                if (messageBytes >= 96) __builtin_memcpy(&st, entry + 92, 4);
                if (postInitPhase_ == 204 && messageBytes >= 120)
                    __builtin_memcpy(&v, entry + 116, 4);
                if (postInitPhase_ != 204 && messageBytes >= 116)
                    __builtin_memcpy(&v, entry + 112, 4);
                setProperty(postInitPhase_ == 204 ? "NVGspControl-dh-active-display-id"
                            : postInitPhase_ == 205 ? "NVGspControl-dh-scanline-a"
                            : "NVGspControl-dh-scanline-b", v, 32);
            }
            dispHealthOk = rpc.result == 0 && st == 0;
        }
        if ((rpc.function == 103 &&
             (postInitPhase_ == 212 || postInitPhase_ == 216)) ||
            (rpc.function == 76 && postInitPhase_ >= 213 &&
             postInitPhase_ <= 215)) {
            ceReturned = true;
            UInt32 st = ~0U;
            if (rpc.function == 103 && messageBytes >= 100)
                __builtin_memcpy(&st, entry + 96, 4);
            if (rpc.function == 76 && messageBytes >= 96)
                __builtin_memcpy(&st, entry + 92, 4);
            if (postInitPhase_ == 215 && messageBytes >= 108)
                __builtin_memcpy(&ceTokenReply, entry + 104, 4);
            ceOk = rpc.result == 0 && st == 0;
            char key[48]{};
            size_t pos = 0;
            appendStr(key, sizeof(key), &pos, "NVGspControl-ce-p");
            appendDec(key, sizeof(key), &pos, postInitPhase_);
            key[pos] = '\0';
            setProperty(key, (UInt64(rpc.result) << 32) | st, 64);
        }
        if (rpc.function == 103 && postInitPhase_ >= 218 &&
            postInitPhase_ <= 220) {
            ceUserdReturned = true;
            UInt32 st = ~0U, pb = 0;
            UInt64 off = 0;
            if (messageBytes >= 112) {
                __builtin_memcpy(&st, entry + 96, 4);
                __builtin_memcpy(&pb, entry + 100, 4);
            }
            if (pb == 128 && messageBytes >= 240)
                __builtin_memcpy(&off, entry + 112 + 80, 8);
            const bool okAlloc = rpc.result == 0 && st == 0 && off;
            if (okAlloc && postInitPhase_ == 218) ceUserdOffset_ = off;
            if (okAlloc && postInitPhase_ == 219) ceInstOffset_ = off;
            if (okAlloc && postInitPhase_ == 220) ceMthdOffset_ = off;
            ceBackingOk = okAlloc;
            char key[48]{};
            size_t pos = 0;
            appendStr(key, sizeof(key), &pos, "NVGspControl-ce-backing-p");
            appendDec(key, sizeof(key), &pos, postInitPhase_);
            key[pos] = '\0';
            setProperty(key, (UInt64(st) << 40) | off, 64);
        }
        // 0.77.0: DP AUXCH_CTRL reply (48B params @104): data@124,
        // size@140 (bytes done, 1-indexed), replyType@144 (0 ACK, 2 DEFER).
        if (rpc.function == 76 && postInitPhase_ == 224) {
            perfReturned = true;
            UInt32 st = ~0U, done = 0, reply = ~0U;
            if (messageBytes >= 104 + 48) {
                __builtin_memcpy(&st, entry + 92, 4);
                __builtin_memcpy(&done, entry + 140, 4);
                __builtin_memcpy(&reply, entry + 144, 4);
            }
            auxLastStatus_ = st;
            auxLastReply_ = reply;
            auxOk_ = rpc.result == 0 && st == 0 && reply == 0;
            if (auxOk_ && done > 16) done = 16;
            if (auxOk_ && auxStep_ <= 2)
                __builtin_memcpy(dpcd_ + auxStep_ * 16, entry + 124, done);
            if (auxOk_ && auxStep_ >= 4 && auxStep_ < 20) {
                __builtin_memcpy(edid_ + (auxStep_ - 4) * 16, entry + 124, done);
                edidBytes_ = (auxStep_ - 4) * 16 + done;
            }
        }
        // 0.69.0: INTERNAL_INTR_GET_KERNEL_TABLE reply (2068B params):
        // tableLen@104, table[128]{u16 engineIdx, u32 pmcMask, u32 stall,
        // u32 nonStall} (16B each)@108, subtreeMap[7]{u8,u8}@2156.
        if (rpc.function == 76 && postInitPhase_ == 223) {
            perfReturned = true;
            UInt32 st = ~0U, pb = 0, len = 0;
            if (messageBytes >= 108) {
                __builtin_memcpy(&st, entry + 92, 4);
                __builtin_memcpy(&pb, entry + 96, 4);
                __builtin_memcpy(&len, entry + 104, 4);
            }
            setProperty("NVGspControl-intr-table-rpc", rpc.result, 32);
            setProperty("NVGspControl-intr-table-status", st, 32);
            setProperty("NVGspControl-intr-table-params-bytes", pb, 32);
            setProperty("NVGspControl-intr-table-len", len, 32);
            if (rpc.result == 0 && st == 0 && pb == 2068 && len <= 128 &&
                messageBytes >= 104 + 2068) {
                setProperty("NVGspControl-intr-table", const_cast<UInt8 *>(entry + 108), len * 16);
                setProperty("NVGspControl-intr-subtrees", const_cast<UInt8 *>(entry + 2156), 14);
                for (UInt32 i = 0; i < len; ++i) {
                    UInt16 eng = 0;
                    UInt32 stall = 0, nonStall = 0;
                    __builtin_memcpy(&eng, entry + 108 + i * 16, 2);
                    __builtin_memcpy(&stall, entry + 116 + i * 16, 4);
                    __builtin_memcpy(&nonStall, entry + 120 + i * 16, 4);
                    const char *name = eng == 50 ? "gsp" : eng == 2 ? "disp"
                        : eng == 4 ? "fifo" : eng == 15 ? "ce0"
                        : eng == 61 ? "nonreplay-fault" : nullptr;
                    if (!name) continue;
                    char key[64]{};
                    size_t pos = 0;
                    appendStr(key, sizeof(key), &pos, "NVGspControl-intr-vec-");
                    appendStr(key, sizeof(key), &pos, name);
                    key[pos] = '\0';
                    // stall vector in high 32, non-stall in low 32.
                    setProperty(key, (UInt64(stall) << 32) | nonStall, 64);
                }
            }
            // Read-only snapshot of the CPU interrupt tree (VF window
            // 0xB80000): TOP[0..1], TOP_EN_SET[0..1], LEAF[0..15],
            // LEAF_EN_SET[0..15], plus PCI MSI / MSI-X control words.
            IOMemoryMap *bar0Map = sharedBar0Map(pci_);
            if (bar0Map && bar0Map->getLength() >= 0xB82000) {
                Bar0Io bar0{bar0Map};
                UInt32 tree[36]{};
                for (UInt32 i = 0; i < 2; ++i) {
                    bar0.read(0xB81600 + i * 4, &tree[i]);
                    bar0.read(0xB81608 + i * 4, &tree[2 + i]);
                }
                for (UInt32 i = 0; i < 16; ++i) {
                    bar0.read(0xB81000 + i * 4, &tree[4 + i]);
                    bar0.read(0xB81200 + i * 4, &tree[20 + i]);
                }
                setProperty("NVGspControl-intr-tree", tree, sizeof(tree));
            }
            if (bar0Map) bar0Map->release();
            UInt8 msiCap = 0, msixCap = 0;
            pci_->findPCICapability(0x05 /* MSI */, &msiCap);
            pci_->findPCICapability(0x11 /* MSI-X */, &msixCap);
            setProperty("NVGspControl-msi-cap", msiCap, 8);
            setProperty("NVGspControl-msi-ctrl",
                        msiCap ? pci_->configRead16(msiCap + 2) : 0, 16);
            setProperty("NVGspControl-msix-cap", msixCap, 8);
            setProperty("NVGspControl-msix-ctrl",
                        msixCap ? pci_->configRead16(msixCap + 2) : 0, 16);
            setProperty("NVGspControl-pci-command",
                        pci_->configRead16(kIOPCIConfigCommand), 16);
            // 0.70.0: arm the MSI path. 0.74.0: GSP stall (50), DISP stall
            // (2), GR non-stall (84) vectors from the table.
            if ((experimentFlags_ & 8) && rpc.result == 0 && st == 0 &&
                pb == 2068 && len <= 128 && messageBytes >= 104 + 2068) {
                UInt32 vecs[kVecCount] = {~0U, ~0U, ~0U, ~0U};
                for (UInt32 i = 0; i < len; ++i) {
                    UInt16 eng = 0;
                    UInt32 stall = ~0U, nonStall = ~0U;
                    __builtin_memcpy(&eng, entry + 108 + i * 16, 2);
                    __builtin_memcpy(&stall, entry + 116 + i * 16, 4);
                    __builtin_memcpy(&nonStall, entry + 120 + i * 16, 4);
                    if (eng == 50) vecs[kVecGsp] = stall;
                    if (eng == 2) vecs[kVecDisp] = stall;
                    if (eng == 84) vecs[kVecGrNs] = nonStall;
                    if (eng == 15) vecs[kVecCeNs] = nonStall;   // 0.119.0: CE0
                }
                setProperty("NVGspControl-intr-armed", armInterrupts(vecs));
            }
        }
        if (rpc.function == 76 && (postInitPhase_ == 221 || postInitPhase_ == 222)) {
            perfReturned = true;
            UInt32 st = ~0U, v = 0;
            if (messageBytes >= 96) __builtin_memcpy(&st, entry + 92, 4);
            if (messageBytes >= 108) __builtin_memcpy(&v, entry + 104, 4);
            if (postInitPhase_ == 221) {
                setProperty("NVGspControl-perf-boost-rpc", rpc.result, 32);
                setProperty("NVGspControl-perf-boost-status", st, 32);
            } else {
                setProperty("NVGspControl-perf-pstate-status", st, 32);
                setProperty("NVGspControl-perf-pstate", v, 32);
            }
        }
        if (rpc.function == 103 && postInitPhase_ == 210) {
            scratchReturned = true;
            UInt32 st = ~0U, pb = 0;
            UInt64 off = 0;
            if (messageBytes >= 112) {
                __builtin_memcpy(&st, entry + 96, 4);
                __builtin_memcpy(&pb, entry + 100, 4);
            }
            if (pb == 128 && messageBytes >= 240)
                __builtin_memcpy(&off, entry + 112 + 80, 8);
            if (rpc.result == 0 && st == 0 && off && !(off & 0x1fffff))
                scratchOffset_ = off;
            else if (scratchTry_ > 0x1000000)
                scratchRetry_ = true;  // 0.60.0: halve and retry
            setProperty("NVGspControl-scratch-status", st, 32);
            setProperty("NVGspControl-scratch-offset", off, 64);
        }
        if (rpc.function == 103 && postInitPhase_ == 209) {
            twoDReturned = true;
            if (messageBytes >= 100) __builtin_memcpy(&twoDStatus, entry + 96, 4);
        }
        if (rpc.function == 76 && postInitPhase_ == 207) {
            wndPbReturned = true;
            UInt32 cs = ~0U;
            if (messageBytes >= 96) __builtin_memcpy(&cs, entry + 92, 4);
            wndPbOk = rpc.result == 0 && cs == 0;
            setProperty("NVGspControl-wnd-pb-status", cs, 32);
        }
        if (rpc.function == 103 && postInitPhase_ == 208) {
            wndChanReturned = true;
            UInt32 st = ~0U;
            if (messageBytes >= 100) __builtin_memcpy(&st, entry + 96, 4);
            wndChanOk = rpc.result == 0 && st == 0;
            setProperty("NVGspControl-wnd-chan-rpc", rpc.result, 32);
            setProperty("NVGspControl-wnd-chan-status", st, 32);
        }
        if (rpc.function == 103 && postInitPhase_ == 202) {
            coreChanReturned = true;
            UInt32 st = ~0U;
            if (messageBytes >= 100) __builtin_memcpy(&st, entry + 96, 4);
            coreChanResponse = rpc.result == 0 && st == 0;
            setProperty("NVGspControl-corechan-rpc", rpc.result, 32);
            setProperty("NVGspControl-corechan-status", st, 32);
        }
        // 0.38.0: ctx VRAM alloc reply (offset at params+80), PROMOTE_CTX
        // reply (status@92), GR object alloc reply (status@96).
        if (rpc.function == 103 && postInitPhase_ == 122) {
            ctxMemReturned = true;
            UInt32 allocStatus = ~0U, paramsBytes = 0;
            UInt64 size = 0, offset = 0;
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
            }
            if (paramsBytes == 128 && messageBytes >= 112 + 128) {
                __builtin_memcpy(&size, entry + 112 + 64, 8);
                __builtin_memcpy(&offset, entry + 112 + 80, 8);
            }
            ctxMemResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0 && size >= 0x2A00000 && offset &&
                (offset & 0x1fffff) == 0;
            if (ctxMemResponse) ctxBackingOffset_ = offset;
            setProperty("NVGspControl-ctxmem-status", allocStatus, 32);
            setProperty("NVGspControl-ctxmem-size", size, 64);
            setProperty("NVGspControl-ctxmem-offset", offset, 64);
        }
        if (rpc.function == 76 && postInitPhase_ == 123) {
            promoteReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            promoteResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0;
            setProperty("NVGspControl-promote-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-promote-status", controlStatus, 32);
            setProperty("NVGspControl-promote-params-bytes", paramsBytes, 32);
        }
        if (rpc.function == 103 &&
            (postInitPhase_ == 124 || postInitPhase_ == 126)) {
            grObjReturned = true;
            UInt32 allocStatus = ~0U;
            if (messageBytes >= 100)
                __builtin_memcpy(&allocStatus, entry + 96, 4);
            grObjResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0;
            const bool is3d = postInitPhase_ == 124;
            setProperty(is3d ? "NVGspControl-grobj3d-rpc-result"
                             : "NVGspControl-grobjc-rpc-result",
                        rpc.result, 32);
            setProperty(is3d ? "NVGspControl-grobj3d-status"
                             : "NVGspControl-grobjc-status",
                        allocStatus, 32);
        }
        // 0.37.0: CONTEXT_BUFFERS_INFO reply. Params at 104:
        // engine[26]{size,alignment} per GR instance; publish GR0.
        if (rpc.function == 76 && postInitPhase_ == 121) {
            dispCtxBufReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            dispCtxBufResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 1664 && messageBytes >= 104 + 26 * 8;
            setProperty("NVGspControl-ctxbuf-status", controlStatus, 32);
            setProperty("NVGspControl-ctxbuf-params-bytes", paramsBytes, 32);
            if (dispCtxBufResponse) {
                for (UInt32 id = 0; id < 26; ++id) {
                    UInt32 size = 0, align = 0;
                    __builtin_memcpy(&size, entry + 104 + id * 8, 4);
                    __builtin_memcpy(&align, entry + 108 + id * 8, 4);
                    char key[64]{};
                    size_t pos = 0;
                    appendStr(key, sizeof(key), &pos,
                              "NVGspControl-ctxbuf-gr0-");
                    appendDec(key, sizeof(key), &pos, id);
                    key[pos] = '\0';
                    // size in low 32, alignment in high 32.
                    setProperty(key, UInt64(size) |
                                (UInt64(align) << 32), 64);
                }
            }
        }
        // 0.35.0: GPFIFO_GET_WORK_SUBMIT_TOKEN reply. Echo:
        // workSubmitToken@104.
        if (rpc.function == 76 && postInitPhase_ == 118) {
            dispTokenReturned = true;
            UInt32 controlStatus = ~0U, paramsBytes = 0, token = ~0U;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
            }
            if (messageBytes >= 108)
                __builtin_memcpy(&token, entry + 104, 4);
            dispTokenResponse = rpc.result == 0 &&
                rpc.privateResult == 0 && controlStatus == 0 &&
                paramsBytes == 4 && token != ~0U;
            if (dispTokenResponse) kickToken_ = token;
            setProperty("NVGspControl-kick-token-status",
                        controlStatus, 32);
            setProperty("NVGspControl-kick-token", token, 32);
        }
        if (rpc.function == 103 && postInitPhase_ == 47) {
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[128]{};
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
                if (paramsBytes == sizeof(params) && messageBytes >= 112 + sizeof(params))
                    __builtin_memcpy(params, entry + 112, sizeof(params));
            }
            setProperty("NVGspControl-err-backing-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-err-backing-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-err-backing-status", allocStatus, 32);
            setProperty("NVGspControl-err-backing-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-err-backing-flags", flags, 32);
            if (paramsBytes == sizeof(params)) {
                UInt64 size = 0, offset = 0, limit = 0;
                __builtin_memcpy(&size, params + 64, 8);
                __builtin_memcpy(&offset, params + 80, 8);
                __builtin_memcpy(&limit, params + 88, 8);
                errBackingOffset_ = offset;
                errBackingSize_ = size;
                setProperty("NVGspControl-err-backing-size", size, 64);
                setProperty("NVGspControl-err-backing-offset", offset, 64);
                setProperty("NVGspControl-err-backing-limit", limit, 64);
            }
            errBackingAllocResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0 && paramsBytes == sizeof(params) &&
                errBackingSize_ == 4096;
        }
        if (rpc.function == 103 && postInitPhase_ == 48) {
            errCtxAllocReturned = true;
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[32]{};
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
                if (paramsBytes == sizeof(params) && messageBytes >= 112 + sizeof(params))
                    __builtin_memcpy(params, entry + 112, sizeof(params));
            }
            setProperty("NVGspControl-errctx-alloc-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-errctx-alloc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-errctx-alloc-status", allocStatus, 32);
            setProperty("NVGspControl-errctx-alloc-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-errctx-alloc-flags", flags, 32);
            errCtxAllocResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0 && paramsBytes == sizeof(params);
        }
        if (rpc.function == 103 && postInitPhase_ == 29) {
            UInt32 allocStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[128]{};
            if (messageBytes >= 112) {
                __builtin_memcpy(&allocStatus, entry + 96, 4);
                __builtin_memcpy(&paramsBytes, entry + 100, 4);
                __builtin_memcpy(&flags, entry + 104, 4);
                if (paramsBytes == sizeof(params) && messageBytes >= 112 + sizeof(params))
                    __builtin_memcpy(params, entry + 112, sizeof(params));
            }
            setProperty("NVGspControl-virtual-memory-alloc-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-virtual-memory-alloc-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-virtual-memory-alloc-status", allocStatus, 32);
            setProperty("NVGspControl-virtual-memory-alloc-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-virtual-memory-alloc-flags", flags, 32);
            if (paramsBytes == sizeof(params)) {
                UInt64 size = 0, offset = 0, limit = 0;
                __builtin_memcpy(&size, params + 64, 8);
                __builtin_memcpy(&offset, params + 80, 8);
                __builtin_memcpy(&limit, params + 88, 8);
                virtualOffset_ = offset;
                setProperty("NVGspControl-virtual-memory-returned-params",
                            params, sizeof(params));
                setProperty("NVGspControl-virtual-memory-size", size, 64);
                setProperty("NVGspControl-virtual-memory-offset", offset, 64);
                setProperty("NVGspControl-virtual-memory-limit", limit, 64);
            }
            virtualMemoryAllocResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                allocStatus == 0 && paramsBytes == sizeof(params) && virtualOffset_ != 0;
        }
        if (rpc.function == 76 && postInitPhase_ == 34) {
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[192]{};
            if (messageBytes >= 104 + sizeof(params)) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
                if (paramsBytes == sizeof(params))
                    __builtin_memcpy(params, entry + 104, sizeof(params));
            }
            setProperty("NVGspControl-va-caps-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-va-caps-private-result", rpc.privateResult, 32);
            setProperty("NVGspControl-va-caps-status", controlStatus, 32);
            setProperty("NVGspControl-va-caps-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-va-caps-flags", flags, 32);
            if (paramsBytes == sizeof(params)) {
                UInt32 vaBits = 0, pdeBits = 0, formats4K = 0;
                UInt32 bigPage = 0, dual = 0, ideal = 0;
                UInt64 vaRangeLo = 0, pageMask = 0;
                __builtin_memcpy(&vaBits, params, 4);
                __builtin_memcpy(&pdeBits, params + 4, 4);
                __builtin_memcpy(&formats4K, params + 8, 4);
                __builtin_memcpy(&bigPage, params + 12, 4);
                __builtin_memcpy(&dual, params + 20, 4);
                __builtin_memcpy(&ideal, params + 24, 4);
                __builtin_memcpy(&vaRangeLo, params + 168, 8);
                __builtin_memcpy(&pageMask, params + 184, 8);
                setProperty("NVGspControl-va-caps-raw", params, sizeof(params));
                setProperty("NVGspControl-va-bit-count", vaBits, 32);
                setProperty("NVGspControl-va-pde-coverage-bits", pdeBits, 32);
                setProperty("NVGspControl-va-4k-format-count", formats4K, 32);
                setProperty("NVGspControl-va-big-page-size", bigPage, 32);
                setProperty("NVGspControl-va-dual-page-table", dual, 32);
                setProperty("NVGspControl-va-ideal-vram-page-size", ideal, 32);
                setProperty("NVGspControl-va-range-lo", vaRangeLo, 64);
                setProperty("NVGspControl-va-supported-page-mask", pageMask, 64);
            }
            vaCapsResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0 && paramsBytes == sizeof(params);
        }
        if (rpc.function == 76 && postInitPhase_ == 35) {
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            UInt8 params[208]{};
            if (messageBytes >= 104 + sizeof(params)) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
                if (paramsBytes == sizeof(params))
                    __builtin_memcpy(params, entry + 104, sizeof(params));
            }
            setProperty("NVGspControl-pde-info-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-pde-info-private-result", rpc.privateResult, 32);
            setProperty("NVGspControl-pde-info-status", controlStatus, 32);
            setProperty("NVGspControl-pde-info-params-bytes", paramsBytes, 32);
            setProperty("NVGspControl-pde-info-flags", flags, 32);
            if (paramsBytes == sizeof(params)) {
                UInt64 gpuAddr = 0, pdeVirtAddr = 0, pdbAddr = 0;
                UInt32 pdeEntrySize = 0, pdeAddrSpace = 0, pdeSize = 0;
                __builtin_memcpy(&gpuAddr, params, 8);
                __builtin_memcpy(&pdeVirtAddr, params + 8, 8);
                __builtin_memcpy(&pdeEntrySize, params + 16, 4);
                __builtin_memcpy(&pdeAddrSpace, params + 20, 4);
                __builtin_memcpy(&pdeSize, params + 24, 4);
                __builtin_memcpy(&pdbAddr, params + 192, 8);
                setProperty("NVGspControl-pde-info-raw", params, sizeof(params));
                setProperty("NVGspControl-pde-gpu-address", gpuAddr, 64);
                setProperty("NVGspControl-pde-virtual-address", pdeVirtAddr, 64);
                setProperty("NVGspControl-pde-entry-size", pdeEntrySize, 32);
                setProperty("NVGspControl-pde-address-space", pdeAddrSpace, 32);
                setProperty("NVGspControl-pde-size", pdeSize, 32);
                setProperty("NVGspControl-pdb-address", pdbAddr, 64);
                pdbAddress_ = pdbAddr;
                setProperty("NVGspControl-pte-blocks", params + 32, 160);

                // NVIDIA's host RM reaches arbitrary VRAM through the movable
                // BAR0 PRAMIN window. Read the live 4 KiB PTE table entry for
                // this VA, then restore the complete window register before
                // allowing GSP-RM to continue. This is deliberately read-only.
                UInt64 pte4KAddress = 0;
                UInt32 pte4KEntrySize = 0;
                for (UInt32 block = 0; block < 5; ++block) {
                    const UInt8 *pte = params + 32 + block * 32;
                    UInt64 address = 0;
                    UInt32 entrySize = 0, pageSize = 0, addressSpace = ~0U;
                    __builtin_memcpy(&address, pte, 8);
                    __builtin_memcpy(&entrySize, pte + 12, 4);
                    __builtin_memcpy(&pageSize, pte + 16, 4);
                    __builtin_memcpy(&addressSpace, pte + 20, 4);
                    if (pageSize == 0x200000 && entrySize == 16 &&
                        addressSpace == 0 && address)
                        pd0Address_ = address;
                    if (pageSize == 4096 && addressSpace == 0 && address &&
                        !pte4KAddress) {
                        pte4KAddress = address;
                        pte4KEntrySize = entrySize;
                    }
                }
                setProperty("NVGspControl-pramin-pte-physical", pte4KAddress, 64);
                setProperty("NVGspControl-pramin-pte-entry-size", pte4KEntrySize, 32);
                IOMemoryMap *bar0Map = sharedBar0Map(pci_);
                UInt8 pteHead[64]{};
                UInt32 windowBefore = 0, windowProgrammed = 0;
                UInt32 windowObserved = 0, windowRestored = 0;
                bool windowRead = false, windowWrite = false;
                bool dataRead = false, restoreWrite = false, restoreRead = false;
                if (bar0Map && pte4KAddress &&
                    (pte4KAddress >> 16) <= 0x00ffffffULL &&
                    bar0Map->getLength() >= 0x00700000 +
                        (pte4KAddress & 0xffff) + sizeof(pteHead)) {
                    Bar0Io bar0{bar0Map};
                    windowRead = bar0.read(0x1700, &windowBefore);
                    windowProgrammed = (windowBefore & 0xfc000000U) |
                        static_cast<UInt32>((pte4KAddress >> 16) & 0x00ffffffU);
                    windowWrite = windowRead && bar0.write(0x1700, windowProgrammed);
                    windowWrite = windowWrite &&
                        bar0.read(0x1700, &windowObserved) &&
                        ((windowObserved & 0x03ffffffU) ==
                         (windowProgrammed & 0x03ffffffU));
                    dataRead = windowWrite;
                    for (UInt32 offset = 0; dataRead && offset < sizeof(pteHead);
                         offset += 4) {
                        UInt32 word = 0;
                        dataRead = bar0.read(0x00700000 +
                            static_cast<UInt32>(pte4KAddress & 0xffff) + offset,
                            &word);
                        if (dataRead)
                            __builtin_memcpy(pteHead + offset, &word, 4);
                    }
                    restoreWrite = windowRead && bar0.write(0x1700, windowBefore);
                    restoreRead = restoreWrite &&
                        bar0.read(0x1700, &windowRestored) &&
                        windowRestored == windowBefore;
                }
                if (bar0Map) bar0Map->release();
                praminPteReadOk_ = windowRead && windowWrite && dataRead &&
                    restoreWrite && restoreRead;
                const UInt64 pteIndex = (gpuAddr >> 12) & 0x1ffULL;
                pte4KAddress_ = pte4KAddress + pteIndex * pte4KEntrySize;
                __builtin_memcpy(&originalPte_, pteHead, sizeof(originalPte_));
                setProperty("NVGspControl-pramin-pte-entry-physical",
                            pte4KAddress_, 64);
                setProperty("NVGspControl-pramin-pte-original", originalPte_, 64);
                setProperty("NVGspControl-pramin-window-before", windowBefore, 32);
                setProperty("NVGspControl-pramin-window-programmed",
                            windowProgrammed, 32);
                setProperty("NVGspControl-pramin-window-observed",
                            windowObserved, 32);
                setProperty("NVGspControl-pramin-window-restored",
                            windowRestored, 32);
                setProperty("NVGspControl-pramin-pte-head", pteHead,
                            sizeof(pteHead));
                setProperty("NVGspControl-pramin-pte-read-ok", praminPteReadOk_);
            }
            pdeInfoResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0 && paramsBytes == sizeof(params);
        }
        if (rpc.function == 76 &&
            (postInitPhase_ == 36 || postInitPhase_ == 37 || postInitPhase_ == 38)) {
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
            }
            const bool ok = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0;
            if (postInitPhase_ == 36) {
                setProperty("NVGspControl-bar2-map-rpc-result", rpc.result, 32);
                setProperty("NVGspControl-bar2-map-private-result", rpc.privateResult, 32);
                setProperty("NVGspControl-bar2-map-status", controlStatus, 32);
                setProperty("NVGspControl-bar2-map-params-bytes", paramsBytes, 32);
                setProperty("NVGspControl-bar2-map-flags", flags, 32);
                bar2MapResponse = ok && paramsBytes == 4;
                bar2MapUnsupportedResponse = controlStatus == 0x56;
                bar2MapUnsupported_ = bar2MapUnsupportedResponse;
                setProperty("NVGspControl-bar2-map-unsupported",
                            bar2MapUnsupportedResponse);
                setProperty("NVGspControl-host-bar2-mapping-required",
                            bar2MapUnsupportedResponse);
            } else if (postInitPhase_ == 37) {
                setProperty("NVGspControl-bar2-verify-rpc-result", rpc.result, 32);
                setProperty("NVGspControl-bar2-verify-private-result", rpc.privateResult, 32);
                setProperty("NVGspControl-bar2-verify-status", controlStatus, 32);
                setProperty("NVGspControl-bar2-verify-params-bytes", paramsBytes, 32);
                setProperty("NVGspControl-bar2-verify-flags", flags, 32);
                bar2VerifyResponse = ok && paramsBytes == 12;
            } else {
                setProperty("NVGspControl-bar2-unmap-rpc-result", rpc.result, 32);
                setProperty("NVGspControl-bar2-unmap-private-result", rpc.privateResult, 32);
                setProperty("NVGspControl-bar2-unmap-status", controlStatus, 32);
                setProperty("NVGspControl-bar2-unmap-params-bytes", paramsBytes, 32);
                setProperty("NVGspControl-bar2-unmap-flags", flags, 32);
                bar2UnmapResponse = ok && paramsBytes == 4;
            }
        }
        if (rpc.function == 76 &&
            (postInitPhase_ == 39 || postInitPhase_ == 40 || postInitPhase_ == 41)) {
            UInt32 controlStatus = ~0U, paramsBytes = 0, flags = 0;
            if (messageBytes >= 104) {
                __builtin_memcpy(&controlStatus, entry + 92, 4);
                __builtin_memcpy(&paramsBytes, entry + 96, 4);
                __builtin_memcpy(&flags, entry + 100, 4);
            }
            const bool ok = rpc.result == 0 && rpc.privateResult == 0 &&
                controlStatus == 0;
            if (postInitPhase_ == 39) {
                setProperty("NVGspControl-pte-map-invalidate-rpc-result",
                            rpc.result, 32);
                setProperty("NVGspControl-pte-map-invalidate-private-result",
                            rpc.privateResult, 32);
                setProperty("NVGspControl-pte-map-invalidate-status",
                            controlStatus, 32);
                setProperty("NVGspControl-pte-map-invalidate-params-bytes",
                            paramsBytes, 32);
                setProperty("NVGspControl-pte-map-invalidate-flags", flags, 32);
                pteMapInvalidateOk_ = ok && paramsBytes == 16;
                pteMapInvalidateResponse = ok;
            } else if (postInitPhase_ == 40) {
                UInt8 params[184]{};
                if (paramsBytes == sizeof(params) &&
                    messageBytes >= 104 + sizeof(params))
                    __builtin_memcpy(params, entry + 104, sizeof(params));
                setProperty("NVGspControl-pte-info-rpc-result", rpc.result, 32);
                setProperty("NVGspControl-pte-info-private-result",
                            rpc.privateResult, 32);
                setProperty("NVGspControl-pte-info-status", controlStatus, 32);
                setProperty("NVGspControl-pte-info-params-bytes", paramsBytes, 32);
                setProperty("NVGspControl-pte-info-flags", flags, 32);
                pteInfoReturned = paramsBytes == sizeof(params) &&
                    messageBytes >= 104 + sizeof(params);
                if (pteInfoReturned) {
                    UInt64 pageSize = 0, entrySize = 0;
                    UInt32 kind = 0, pteFlags = 0;
                    __builtin_memcpy(&pageSize, params + 16, 8);
                    __builtin_memcpy(&entrySize, params + 24, 8);
                    __builtin_memcpy(&kind, params + 36, 4);
                    __builtin_memcpy(&pteFlags, params + 40, 4);
                    hostPteValidated_ = ok && pageSize == 4096 &&
                        entrySize == 8 && kind == 6 && (pteFlags & 1);
                    setProperty("NVGspControl-pte-info-raw", params,
                                sizeof(params));
                    setProperty("NVGspControl-pte-info-page-size", pageSize, 64);
                    setProperty("NVGspControl-pte-info-entry-size", entrySize, 64);
                    setProperty("NVGspControl-pte-info-kind", kind, 32);
                    setProperty("NVGspControl-pte-info-pte-flags", pteFlags, 32);
                    setProperty("NVGspControl-host-pte-validated",
                                hostPteValidated_);
                }
            } else {
                setProperty("NVGspControl-pte-restore-invalidate-rpc-result",
                            rpc.result, 32);
                setProperty("NVGspControl-pte-restore-invalidate-private-result",
                            rpc.privateResult, 32);
                setProperty("NVGspControl-pte-restore-invalidate-status",
                            controlStatus, 32);
                setProperty("NVGspControl-pte-restore-invalidate-params-bytes",
                            paramsBytes, 32);
                setProperty("NVGspControl-pte-restore-invalidate-flags", flags, 32);
                pteRestoreInvalidateOk_ = ok && paramsBytes == 16;
                pteRestoreInvalidateResponse = ok;
            }
        }
        if (rpc.function == 14 && postInitPhase_ == 30) {
            UInt32 mapStatus = ~0U;
            UInt64 dmaOffset = 0;
            if (messageBytes >= 136) {
                __builtin_memcpy(&dmaOffset, entry + 120, 8);
                __builtin_memcpy(&mapStatus, entry + 128, 4);
            }
            setProperty("NVGspControl-dma-map-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-dma-map-private-result", rpc.privateResult, 32);
            setProperty("NVGspControl-dma-map-status", mapStatus, 32);
            setProperty("NVGspControl-dma-map-offset", dmaOffset, 64);
            dmaMapResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                mapStatus == 0 && dmaOffset == virtualOffset_;
            dmaMapUnsupportedResponse = rpc.result == 0x2a;
            setProperty("NVGspControl-dma-map-rpc-unsupported",
                        dmaMapUnsupportedResponse);
            setProperty("NVGspControl-split-vas-host-mapping-required",
                        dmaMapUnsupportedResponse);
        }
        if (rpc.function == 15 && postInitPhase_ == 31) {
            UInt32 unmapStatus = ~0U;
            if (messageBytes >= 116)
                __builtin_memcpy(&unmapStatus, entry + 112, 4);
            setProperty("NVGspControl-dma-unmap-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-dma-unmap-private-result", rpc.privateResult, 32);
            setProperty("NVGspControl-dma-unmap-status", unmapStatus, 32);
            dmaUnmapResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                unmapStatus == 0;
            dmaMapCompleted_ = dmaUnmapResponse;
        }
        if (rpc.function == 10 && postInitPhase_ == 32) {
            UInt32 freeStatus = ~0U;
            if (messageBytes >= 96)
                __builtin_memcpy(&freeStatus, entry + 92, 4);
            setProperty("NVGspControl-owned-tree-free-rpc-result", rpc.result, 32);
            setProperty("NVGspControl-owned-tree-free-private-result",
                        rpc.privateResult, 32);
            setProperty("NVGspControl-owned-tree-free-status", freeStatus, 32);
            ownedTreeFreeResponse = rpc.result == 0 && rpc.privateResult == 0 &&
                freeStatus == 0;
        }
    }

    if (initDone_ && initResult_ == 0 && !badReason) {
        // 0.7.0-0.8.1: unproven controls (CE size 49, BIND 52, SCHEDULE 53,
        // TSG BIND 54, TSG SCHEDULE 55, TSG GET_INFO 56, TSG TIMESLICE 57,
        // FIFO intel 58-61, token 62, flush 63, display allocs 64-67) must
        // never park like phase 39. If GSP-RM never answers, restore the
        // PTE when installed and free the tree. Only PROVEN function-103
        // phases skip the watchdog; 64-67 are new classes/objects.
        const bool phase58Answered = fifoInfoReturned || userdLocReturned ||
            partnerReturned || priBaseReturned || tokenReturned ||
            flushReturned;
        const bool phase64Answered = dispReturned || pbBackingReturned ||
            pbCtxdmaReturned || notifyBackingReturned ||
            notifyCtxdmaReturned || dispPbProgramReturned ||
            dispKickFlushReturned || dispHandlerProbe1Returned ||
            dispHandlerProbe2Returned || sched97chanReturned ||
            sched97tsgReturned || dispC372Returned ||
            dispCommonReturned || dispSysSupportedReturned ||
            dispSysNumHeadsReturned || dispSysActiveReturned ||
            dispSysActive1Returned || dispSysActive2Returned ||
            dispSysActive3Returned || dispConnectReturned ||
            dispBootDisplaysReturned || dispScanlineReturned ||
            dispVblankReturned || dispRoutingReturned ||
            dispViewportReturned || dispDfpReturned ||
            dispEdidReturned || dispPclkReturned ||
            dispOrReturned || dispCapsReturned ||
            dispVbEnReturned || dispImpReturned ||
            dispModeReturned || dispConnReturned ||
            dispArchReturned || dispNameReturned ||
            dispFifoReturned || dispRunlistReturned ||
            dispGrBindReturned || dispReRunReturned ||
            dispRl0cReturned || dispRl1Returned ||
            dispRl2Returned || dispSchedReturned ||
            dispPostReturned || dispPs1Returned ||
            dispPs2Returned || dispDevInfoReturned ||
            dispUserdReturned || dispGpfifoReturned ||
            dispRamfcReturned || dispFifoCapsReturned ||
            dispCtxPropReturned || dispTpcReturned ||
            dispTokenReturned || dispCtxBufReturned ||
            ctxMemReturned || promoteReturned || grObjReturned ||
            dispInstMemReturned || dispInstWriteReturned || coreChanReturned ||
            dispHealthReturned || wndPbReturned || wndChanReturned ||
            twoDReturned || scratchReturned || ceReturned || ceUserdReturned ||
            perfReturned;
        const bool awaitingUnproven =
            (postInitPhase_ == 49 && !methodSizeAnswered) ||
            (postInitPhase_ == 52 && !bindReturned) ||
            (postInitPhase_ == 53 && !scheduleReturned) ||
            (postInitPhase_ == 54 && !tsgBindReturned) ||
            (postInitPhase_ == 55 && !tsgScheduleReturned) ||
            (postInitPhase_ == 56 && !tsgInfoReturned) ||
            (postInitPhase_ == 57 && !tsgTimesliceReturned) ||
            (postInitPhase_ >= 58 && postInitPhase_ <= 63 &&
             !phase58Answered) ||
            (((postInitPhase_ >= 64 && postInitPhase_ <= 118) ||
              (postInitPhase_ >= 121 && postInitPhase_ <= 126 &&
               postInitPhase_ != 125) ||
              (postInitPhase_ >= 200 && postInitPhase_ <= 224 &&
               postInitPhase_ != 217)) &&
             !phase64Answered);
        if (awaitingUnproven) {
            // 0.91.0: time-based as well — with 0.2 s daemon polls (0.90.0)
            // 12 polls were only ~2.4 s and pb-backing tore the chain down.
            UInt64 nowAbs = 0, stalledNs = 0;
            clock_get_uptime(&nowAbs);
            if (ceSizeStallPolls_ == 0) stallStartAbs_ = nowAbs;
            absolutetime_to_nanoseconds(nowAbs - stallStartAbs_, &stalledNs);
            if (++ceSizeStallPolls_ > 12 && stalledNs > 30000000000ULL) {
                const char *reason = "ce-size-no-response-watchdog";
                if (postInitPhase_ == 52)
                    reason = "bind-no-response-watchdog";
                else if (postInitPhase_ == 53)
                    reason = "schedule-no-response-watchdog";
                else if (postInitPhase_ == 54)
                    reason = "tsg-bind-no-response-watchdog";
                else if (postInitPhase_ == 55)
                    reason = "tsg-schedule-no-response-watchdog";
                else if (postInitPhase_ == 56)
                    reason = "tsg-info-no-response-watchdog";
                else if (postInitPhase_ == 57)
                    reason = "tsg-timeslice-no-response-watchdog";
                else if (postInitPhase_ == 58)
                    reason = "fifo-info-no-response-watchdog";
                else if (postInitPhase_ == 59)
                    reason = "userd-loc-no-response-watchdog";
                else if (postInitPhase_ == 60)
                    reason = "partner-no-response-watchdog";
                else if (postInitPhase_ == 61)
                    reason = "pribase-no-response-watchdog";
                else if (postInitPhase_ == 62)
                    reason = "token-no-response-watchdog";
                else if (postInitPhase_ == 63)
                    reason = "flush-no-response-watchdog";
                else if (postInitPhase_ == 64)
                    reason = "disp-no-response-watchdog";
                else if (postInitPhase_ == 65)
                    reason = "pb-backing-no-response-watchdog";
                else if (postInitPhase_ == 66)
                    reason = "pb-ctxdma-no-response-watchdog";
                else if (postInitPhase_ == 67)
                    reason = "notify-backing-no-response-watchdog";
                else if (postInitPhase_ == 68)
                    reason = "notify-ctxdma-no-response-watchdog";
                else if (postInitPhase_ == 69)
                    reason = "disp-pb-program-no-response-watchdog";
                else if (postInitPhase_ == 70)
                    reason = "disp-kick-flush-no-response-watchdog";
                else if (postInitPhase_ == 71)
                    reason = "disp-handler-probe1-no-response-watchdog";
                else if (postInitPhase_ == 72)
                    reason = "disp-handler-probe2-no-response-watchdog";
                else if (postInitPhase_ == 73)
                    reason = "sched97chan-no-response-watchdog";
                else if (postInitPhase_ == 74)
                    reason = "sched97tsg-no-response-watchdog";
                else if (postInitPhase_ == 75)
                    reason = "disp-c372-no-response-watchdog";
                else if (postInitPhase_ == 76)
                    reason = "disp-common-no-response-watchdog";
                else if (postInitPhase_ == 77)
                    reason = "disp-sys-supported-no-response-watchdog";
                else if (postInitPhase_ == 78)
                    reason = "disp-sys-num-heads-no-response-watchdog";
                else if (postInitPhase_ == 79)
                    reason = "disp-sys-active-no-response-watchdog";
                else if (postInitPhase_ == 80)
                    reason = "disp-sys-active1-no-response-watchdog";
                else if (postInitPhase_ == 81)
                    reason = "disp-sys-active2-no-response-watchdog";
                else if (postInitPhase_ == 82)
                    reason = "disp-sys-active3-no-response-watchdog";
                else if (postInitPhase_ == 83)
                    reason = "disp-connect-no-response-watchdog";
                else if (postInitPhase_ == 84)
                    reason = "disp-boot-displays-no-response-watchdog";
                else if (postInitPhase_ == 85)
                    reason = "disp-scanline-no-response-watchdog";
                else if (postInitPhase_ == 86)
                    reason = "disp-vblank-no-response-watchdog";
                else if (postInitPhase_ == 87)
                    reason = "disp-routing-no-response-watchdog";
                else if (postInitPhase_ == 88)
                    reason = "disp-viewport-no-response-watchdog";
                else if (postInitPhase_ == 89)
                    reason = "disp-dfp-no-response-watchdog";
                else if (postInitPhase_ == 90)
                    reason = "disp-edid-no-response-watchdog";
                else if (postInitPhase_ == 91)
                    reason = "disp-pclk-no-response-watchdog";
                else if (postInitPhase_ == 92)
                    reason = "disp-or-no-response-watchdog";
                else if (postInitPhase_ == 93)
                    reason = "disp-caps-no-response-watchdog";
                else if (postInitPhase_ == 94)
                    reason = "disp-vblank-en-no-response-watchdog";
                else if (postInitPhase_ == 95)
                    reason = "disp-imp-no-response-watchdog";
                else if (postInitPhase_ == 96)
                    reason = "disp-mode-no-response-watchdog";
                else if (postInitPhase_ == 97)
                    reason = "disp-conn-no-response-watchdog";
                else if (postInitPhase_ == 98)
                    reason = "disp-arch-no-response-watchdog";
                else if (postInitPhase_ == 99)
                    reason = "disp-name-no-response-watchdog";
                else if (postInitPhase_ == 100)
                    reason = "disp-fifo-no-response-watchdog";
                else if (postInitPhase_ == 101)
                    reason = "disp-runlist-no-response-watchdog";
                else if (postInitPhase_ == 102)
                    reason = "disp-grbind-no-response-watchdog";
                else if (postInitPhase_ == 103)
                    reason = "disp-rerun-no-response-watchdog";
                else if (postInitPhase_ == 104)
                    reason = "disp-rl0c-no-response-watchdog";
                else if (postInitPhase_ == 105)
                    reason = "disp-rl1-no-response-watchdog";
                else if (postInitPhase_ == 106)
                    reason = "disp-rl2-no-response-watchdog";
                else if (postInitPhase_ == 107)
                    reason = "disp-sched-no-response-watchdog";
                else if (postInitPhase_ == 108)
                    reason = "disp-post-no-response-watchdog";
                else if (postInitPhase_ == 109)
                    reason = "disp-ps1-no-response-watchdog";
                else if (postInitPhase_ == 110)
                    reason = "disp-ps2-no-response-watchdog";
                else if (postInitPhase_ == 111)
                    reason = "disp-devinfo-no-response-watchdog";
                else if (postInitPhase_ == 112)
                    reason = "disp-userd-no-response-watchdog";
                else if (postInitPhase_ == 113)
                    reason = "disp-gpfifo-no-response-watchdog";
                else if (postInitPhase_ == 114)
                    reason = "disp-ramfc-no-response-watchdog";
                else if (postInitPhase_ == 115)
                    reason = "disp-fifocaps-no-response-watchdog";
                else if (postInitPhase_ == 116)
                    reason = "disp-ctxprop-no-response-watchdog";
                else if (postInitPhase_ == 117)
                    reason = "disp-tpc-no-response-watchdog";
                else if (postInitPhase_ == 118)
                    reason = "disp-token-no-response-watchdog";
                else if (postInitPhase_ == 121)
                    reason = "ctxbuf-info-no-response-watchdog";
                else if (postInitPhase_ == 122)
                    reason = "ctx-mem-no-response-watchdog";
                else if (postInitPhase_ == 123)
                    reason = "promote-ctx-no-response-watchdog";
                else if (postInitPhase_ == 124)
                    reason = "gr-3d-object-no-response-watchdog";
                else if (postInitPhase_ == 126)
                    reason = "gr-compute-object-no-response-watchdog";
                else if (postInitPhase_ == 200)
                    reason = "disp-instmem-alloc-no-response-watchdog";
                else if (postInitPhase_ == 201)
                    reason = "disp-instmem-write-no-response-watchdog";
                else if (postInitPhase_ == 202)
                    reason = "disp-core-alloc-no-response-watchdog";
                else if (postInitPhase_ >= 203 && postInitPhase_ <= 206)
                    reason = "disp-health-no-response-watchdog";
                else if (postInitPhase_ == 207 || postInitPhase_ == 208)
                    reason = "disp-window-no-response-watchdog";
                setProperty("NVGspControl-channel-skipped-reason", reason);
                // 0.39.0: our ctx mappings go before the client is freed.
                // 0.40.0: huge ctx PTEs are left for RM's VAS free.
                ctxHugeInstalled_ = 0;
                finishBar1();
                if (ctxPtesInstalled_) {
                    setProperty("NVGspControl-ctx-ptes-cleared",
                                praminWritePteRun(pci_, pte4KAddress_ + 8,
                                                  ~0ULL, ctxPtesInstalled_, 0));
                    ctxPtesInstalled_ = 0;
                }
                if (gpfifoPteInstalled_) {
                    UInt64 original = originalPte_;
                    PraminPteResult restore{};
                    gpfifoPteRestored_ = praminPteAccess(
                        pci_, pte4KAddress_, &original, true, false,
                        &restore);
                    setProperty("NVGspControl-gpfifo-pte-restored",
                                gpfifoPteRestored_);
                }
                constexpr UInt32 kClientHandle = 0xc0d00001;
                UInt8 freeParams[16]{};
                __builtin_memcpy(freeParams, &kClientHandle, 4);
                __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                    postInitPhase_ = 32;
                else
                    badReason = 45;
            }
        } else if (postInitPhase_ != 49 && postInitPhase_ != 52 &&
                   postInitPhase_ != 53 && postInitPhase_ != 54 &&
                   postInitPhase_ != 55 && postInitPhase_ != 56 &&
                   postInitPhase_ != 57 &&
                   (postInitPhase_ < 58 || postInitPhase_ > 63) &&
                   (postInitPhase_ < 64 || postInitPhase_ > 118) &&
                   (postInitPhase_ < 121 || postInitPhase_ > 126 ||
                    postInitPhase_ == 125) &&
                   (postInitPhase_ < 200 || postInitPhase_ > 224 ||
                    postInitPhase_ == 217)) {
            ceSizeStallPolls_ = 0;
        }
        // 0.38.0: common teardown for the ctx path — remove our ctx
        // PTEs (VALID first), restore the GPFIFO PTE, free the client.
        // 0.60.0: scratch VRAM alloc (phase 210), size scratchTry_.
        auto scratchAlloc = [&]() {
            // 0.58.0: 256 MiB scratch (2 MiB pages, contiguous, 2 MiB
            // aligned) for benchmarks, before any PTE is installed.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00070;
            constexpr UInt32 kMemoryClass = 0x40;
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6, kFlags = 0x100;
            // 0.59.0: 128 MiB, 4 KiB-page contiguous with 2 MiB alignment
            // (256 MiB HUGE pages -> 0x51 NO_MEMORY in 0.58.0).
            constexpr UInt32 kAttr = 0x10800000, kAttr2 = 0;
            if (!scratchTry_) scratchTry_ = 0x8000000;
            const UInt64 kBytes = scratchTry_;
            constexpr UInt64 kAlign = 0x200000;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 40, &kFlags, 4);
            __builtin_memcpy(alloc + 56, &kAttr, 4);
            __builtin_memcpy(alloc + 60, &kAttr2, 4);
            __builtin_memcpy(alloc + 96, &kBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlign, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 210;
            else
                badReason = 125;
        };
        // 0.66.0: CE backing allocs via RPC (GSP heap): 218 USERD 4 KiB
        // (0xc0d10043), 219 instance 4 KiB (0xc0d10044), 220 method
        // buffer (0xc0d10045).
        auto ceBackingAlloc = [&](UInt32 phase) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            const UInt32 handle = 0xc0d10043 + (phase - 218);
            constexpr UInt32 kMemoryClass = 0x40;
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &handle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6, kAttr = 0x10800000;
            const UInt64 bytes = phase == 220
                ? UInt64((methodBufferBytes_ ? methodBufferBytes_ : 20480) + 0xfff) & ~0xfffULL
                : 4096;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 56, &kAttr, 4);
            __builtin_memcpy(alloc + 96, &bytes, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = phase;
            else
                badReason = 126;
        };
        // 0.54.0: display health sweep start (display common 0x73 alloc,
        // phase 203); falls back to teardown.
        auto dispHealthStart = [&]() {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kCommonHandle = 0xc0d00073;
            constexpr UInt32 kCommonClass = 0x73;
            UInt8 alloc[32]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kCommonHandle, 4);
            __builtin_memcpy(alloc + 12, &kCommonClass, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 203;
            else
                badReason = 123;
        };
        auto ctxTeardown = [&]() {
            if (bar1Early_) { bar1Early_ = false; bar1Finished_ = false; }   // redo at the end
            finishBar1();
            // 0.57.0: with our window owning the screen the whole client
            // (display + GR channel, golden ctx, mappings) stays alive for
            // submitGr; GPFIFO PTE stays installed.
            if (wndOwnsScreen_ && ceMapped_ && !ceStarted_) {
                // 0.65.0: CE bring-up starts with an RPC USERD object (the
                // GR channel got hUserdMemory 0xc0d00043; hUserdMemory 0
                // made the CE channel alloc fail NO_MEMORY in 0.63/0.64).
                ceStarted_ = true;
                grPersistent_ = gr3dOk_ && ctxHugeInstalled_ != 0;
                setProperty("NVGspControl-gr-persistent", grPersistent_);
                syncRingSeqLocked(true, false);          // 0.164.0
                publishSemVa();                          // 0.128.0
                constexpr UInt32 kClientHandle = 0xc0d00001;
                constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
                constexpr UInt32 kMemoryHandle = 0xc0d10043;
                constexpr UInt32 kMemoryClass = 0x40;
                constexpr UInt32 kMemoryParamsBytes = 128;
                UInt8 alloc[32 + kMemoryParamsBytes]{};
                __builtin_memcpy(alloc, &kClientHandle, 4);
                __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
                __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
                __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
                __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
                __builtin_memcpy(alloc + 32, &kClientHandle, 4);
                constexpr UInt32 kDmaType = 6, kAttr = 0x00800000;
                constexpr UInt64 kBytes = 4096;
                __builtin_memcpy(alloc + 36, &kDmaType, 4);
                __builtin_memcpy(alloc + 56, &kAttr, 4);
                __builtin_memcpy(alloc + 96, &kBytes, 8);
                if (init_.enqueueRpc(103, alloc, sizeof(alloc))) {
                    postInitPhase_ = 218;
                    return;
                }
            }
            if (wndOwnsScreen_) {
                grPersistent_ = gr3dOk_ && ctxHugeInstalled_ != 0;
                setProperty("NVGspControl-persistent-client", true);
                setProperty("NVGspControl-gr-persistent", grPersistent_);
                syncRingSeqLocked(true, false);          // 0.164.0
                publishSemVa();                          // 0.128.0
                postInitPhase_ = 33;
                markBoot("phase33");
                return;
            }
            fbHugeInstalled_ = 0;
            // 0.40.0: huge ctx PTEs stay until RM frees the VAS: clearing
            // them under a live GR context faulted the free-time ctx save
            // (0.39.0 free hang). PD0 goes away with the client.
            ctxHugeInstalled_ = 0;
            if (ctxPtesInstalled_) {
                const bool cleared = praminWritePteRun(
                    pci_, pte4KAddress_ + 8, ~0ULL, ctxPtesInstalled_, 0);
                setProperty("NVGspControl-ctx-ptes-cleared", cleared);
                ctxPtesInstalled_ = 0;
            }
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        };
        if (postInitPhase_ == 0) {
            // 0.97.0: no phantom advance without staging (post-S3 quiesce
            // drops it; the daemon re-boots). Enqueueing RPC 1 into
            // released queues faked a chain to phase 33 with a dead GSP.
            if (!staged_) {
                setProperty("NVGspControl-resume-wait", true);
            } else {
            UInt8 guest[792]{};
            UInt32 value = 0x29; __builtin_memcpy(guest, &value, 4);
            value = 0x0c; __builtin_memcpy(guest + 4, &value, 4);
            value = 256;
            __builtin_memcpy(guest + 8, &value, 4);
            __builtin_memcpy(guest + 12, &value, 4);
            __builtin_memcpy(guest + 16, &value, 4);
            value = 35817632; __builtin_memcpy(guest + 20, &value, 4);
            strlcpy(reinterpret_cast<char *>(guest + 24), "570.144", 256);
            strlcpy(reinterpret_cast<char *>(guest + 280),
                    "rel/gpu_drv/r570/r570_00-407", 256);
            strlcpy(reinterpret_cast<char *>(guest + 536),
                    "Official r570_00 rel/gpu_drv/r570/r570_00-407", 256);
            if (init_.enqueueRpc(1, guest, sizeof(guest))) postInitPhase_ = 1;
            else badReason = 8;
            }
        } else if (guestInfoResponse) {
            UInt8 ext[268]{};
            strlcpy(reinterpret_cast<char *>(ext), "r570_00", 256);
            UInt32 domain = 0;
            UInt16 bus = pci_->getBusNumber(), device = pci_->getDeviceNumber();
            __builtin_memcpy(ext + 256, &domain, 4);
            __builtin_memcpy(ext + 260, &bus, 2);
            __builtin_memcpy(ext + 262, &device, 2);
            if (init_.enqueueRpc(64, ext, sizeof(ext))) postInitPhase_ = 2;
            else badReason = 9;
        } else if (guestInfoExtResponse) {
            UInt8 *request = static_cast<UInt8 *>(IOMalloc(kStaticInfoBytes));
            if (!request) {
                badReason = 10;
            } else {
                bzero(request, kStaticInfoBytes);
                if (init_.enqueueRpc(65, request, kStaticInfoBytes))
                    postInitPhase_ = 3;
                else
                    badReason = 10;
                IOFree(request, kStaticInfoBytes);
            }
        } else if (staticInfoResponse) {
            postInitPhase_ = 4;
            setProperty("NVGspControl-static-info", staticInfo, kStaticInfoBytes);
            UInt64 fbLength = 0, fbpMask = 0;
            UInt32 fbBusWidth = 0, fbRamType = 0, l2Bytes = 0;
            __builtin_memcpy(&fbLength, staticInfo + 1224, 8);
            __builtin_memcpy(&fbBusWidth, staticInfo + 1240, 4);
            __builtin_memcpy(&fbRamType, staticInfo + 1244, 4);
            __builtin_memcpy(&fbpMask, staticInfo + 1248, 8);
            __builtin_memcpy(&l2Bytes, staticInfo + 1256, 4);
            setProperty("NVGspControl-static-fb-bytes", fbLength, 64);
            setProperty("NVGspControl-static-fb-bus-width", fbBusWidth, 32);
            setProperty("NVGspControl-static-fb-ram-type", fbRamType, 32);
            setProperty("NVGspControl-static-fbp-mask", fbpMask, 64);
            setProperty("NVGspControl-static-l2-bytes", l2Bytes, 32);
            char name[65]{};
            __builtin_memcpy(name, staticInfo + 1260, 64);
            setProperty("NVGspControl-static-gpu-name", name);
            // 0.61.0: FB map from GspStaticConfigInfo (r570 layout:
            // fbRegionInfoParams@344 = numFBRegions + 16 x 48 B regions
            // @352 {base, limit, reserved, perf, compr, iso, protected},
            // fb_length@1224, fb_bus_width@1240, fb_ram_type@1244).
            {
                UInt64 fbLength = 0;
                UInt32 nRegions = 0, busWidth = 0, ramType = 0;
                __builtin_memcpy(&fbLength, staticInfo + 1224, 8);
                __builtin_memcpy(&busWidth, staticInfo + 1240, 4);
                __builtin_memcpy(&ramType, staticInfo + 1244, 4);
                __builtin_memcpy(&nRegions, staticInfo + 344, 4);
                setProperty("NVGspControl-fb-length", fbLength, 64);
                setProperty("NVGspControl-fb-bus-width", busWidth, 32);
                setProperty("NVGspControl-fb-ram-type", ramType, 32);
                setProperty("NVGspControl-fb-regions", nRegions, 32);
                if (nRegions > 16) nRegions = 16;
                setProperty("NVGspControl-fb-region-raw", staticInfo + 352,
                            nRegions * 48);
                // 0.62.0: the largest unreserved region is the client
                // (CPU-RM/PMA) heap — ours, since this kext is CPU-RM.
                for (UInt32 i = 0; i < nRegions; ++i) {
                    UInt64 base = 0, limit = 0, reserved = ~0ULL;
                    __builtin_memcpy(&base, staticInfo + 352 + i * 48, 8);
                    __builtin_memcpy(&limit, staticInfo + 360 + i * 48, 8);
                    __builtin_memcpy(&reserved, staticInfo + 368 + i * 48, 8);
                    if (reserved == 0 && limit > base &&
                        limit - base > fbFreeLimit_ - fbFreeBase_) {
                        fbFreeBase_ = base;
                        fbFreeLimit_ = limit;
                    }
                }
                setProperty("NVGspControl-fb-free-base", fbFreeBase_, 64);
                setProperty("NVGspControl-fb-free-limit", fbFreeLimit_, 64);
            }
            __builtin_memcpy(&internalClient_, staticInfo + 1600, 4);
            __builtin_memcpy(&internalDevice_, staticInfo + 1604, 4);
            __builtin_memcpy(&internalSubdevice_, staticInfo + 1608, 4);
            // 0.82.0: published for live INTERNAL_* controls via --rpc.
            setProperty("NVGspControl-internal-client", internalClient_, 32);
            setProperty("NVGspControl-internal-device", internalDevice_, 32);
            setProperty("NVGspControl-internal-subdevice", internalSubdevice_, 32);
        } else if (postInitPhase_ == 4) {
            constexpr UInt32 kParamsBytes = 4 + 512 * 48;
            UInt8 *control = static_cast<UInt8 *>(IOMalloc(24 + kParamsBytes));
            if (!control) {
                badReason = 11;
            } else {
                bzero(control, 24 + kParamsBytes);
                const UInt32 command = 0x20800a40;
                // This internal 0x20800a40 control is not present in the
                // 570.144 FINN serializer table. NVIDIA's
                // serverSerializeCtrlDown() therefore leaves the native
                // v28.04 parameter block unchanged and sends flags=0.
                const UInt32 serialized = 0;
                __builtin_memcpy(control, &internalClient_, 4);
                __builtin_memcpy(control + 4, &internalSubdevice_, 4);
                __builtin_memcpy(control + 8, &command, 4);
                __builtin_memcpy(control + 16, &kParamsBytes, 4);
                __builtin_memcpy(control + 20, &serialized, 4);
                if (init_.enqueueRpc(76, control, 24 + kParamsBytes))
                    postInitPhase_ = 5;
                else
                    badReason = 12;
                IOFree(control, 24 + kParamsBytes);
            }
        } else if (deviceInfoResponse) {
            UInt32 entries = 0;
            __builtin_memcpy(&entries, deviceInfo, 4);
            setProperty("NVGspControl-device-info", deviceInfo, deviceInfoBytes);
            setProperty("NVGspControl-device-info-entries", entries, 32);
            setProperty("NVGspControl-device-info-status", deviceInfoStatus, 32);
            postInitPhase_ = 6;
        } else if (postInitPhase_ == 6) {
            constexpr UInt32 kClassListBytes = sizeof(classList);
            UInt8 control[24 + kClassListBytes]{};
            const UInt32 command = 0x00800292;
            __builtin_memcpy(control, &internalClient_, 4);
            __builtin_memcpy(control + 4, &internalDevice_, 4);
            __builtin_memcpy(control + 8, &command, 4);
            __builtin_memcpy(control + 16, &kClassListBytes, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 7;
            else
                badReason = 13;
        } else if (classListResponse) {
            UInt32 count = 0;
            __builtin_memcpy(&count, classList, 4);
            if (count <= 100) {
                setProperty("NVGspControl-class-list", classList + 4,
                            count * sizeof(UInt32));
                setProperty("NVGspControl-class-list-count", count, 32);
                postInitPhase_ = 8;
            } else {
                badReason = 14;
            }
        } else if (postInitPhase_ == 8) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kRootClientClass = 0x41;
            constexpr UInt32 kRootParamsBytes = 120;
            UInt8 alloc[32 + kRootParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);       // hClient
            __builtin_memcpy(alloc + 8, &kClientHandle, 4);  // hObject
            __builtin_memcpy(alloc + 12, &kRootClientClass, 4);
            __builtin_memcpy(alloc + 20, &kRootParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            strlcpy(reinterpret_cast<char *>(alloc + 40),
                    "NVGspControl", 100);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 9;
            else
                badReason = 15;
        } else if (clientAllocResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 11;
            else
                badReason = 16;
        } else if (clientFreeResponse) {
            postInitPhase_ = 12;
        } else if (postInitPhase_ == 12) {
            UInt8 control[28]{};
            constexpr UInt32 command = 0x00000204;
            constexpr UInt32 paramsBytes = sizeof(deviceIds);
            __builtin_memcpy(control, &internalClient_, 4);
            __builtin_memcpy(control + 4, &internalClient_, 4);
            __builtin_memcpy(control + 8, &command, 4);
            __builtin_memcpy(control + 16, &paramsBytes, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 13;
            else
                badReason = 17;
        } else if (deviceIdsResponse) {
            setProperty("NVGspControl-device-ids", deviceIds, 32);
            postInitPhase_ = 14;
        } else if (postInitPhase_ == 14) {
            UInt8 control[24 + sizeof(attachedIds)]{};
            constexpr UInt32 command = 0x00000201;
            constexpr UInt32 paramsBytes = sizeof(attachedIds);
            __builtin_memcpy(control, &internalClient_, 4);
            __builtin_memcpy(control + 4, &internalClient_, 4);
            __builtin_memcpy(control + 8, &command, 4);
            __builtin_memcpy(control + 16, &paramsBytes, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 15;
            else
                badReason = 18;
        } else if (attachedIdsResponse) {
            UInt32 count = 0;
            while (count < 32 && attachedIds[count] != ~0U) ++count;
            setProperty("NVGspControl-attached-gpu-ids", attachedIds,
                        count * sizeof(UInt32));
            setProperty("NVGspControl-attached-gpu-count", count, 32);
            postInitPhase_ = 16;
        } else if (postInitPhase_ == 16) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kRootClientClass = 0x41;
            constexpr UInt32 kRootParamsBytes = 120;
            UInt8 alloc[32 + kRootParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 8, &kClientHandle, 4);
            __builtin_memcpy(alloc + 12, &kRootClientClass, 4);
            __builtin_memcpy(alloc + 20, &kRootParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            strlcpy(reinterpret_cast<char *>(alloc + 40),
                    "NVGspControl", 100);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 17;
            else
                badReason = 19;
        } else if (ownedClientResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kDeviceClass = 0x80;
            constexpr UInt32 kDeviceParamsBytes = 56;
            UInt8 alloc[32 + kDeviceParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kClientHandle, 4);
            __builtin_memcpy(alloc + 8, &kDeviceHandle, 4);
            __builtin_memcpy(alloc + 12, &kDeviceClass, 4);
            __builtin_memcpy(alloc + 20, &kDeviceParamsBytes, 4);
            // deviceId=0 selects bit 0 from the validated device mask.
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 19;
            else
                badReason = 20;
        } else if (deviceAllocResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kSubdeviceClass = 0x2080;
            constexpr UInt32 kSubdeviceParamsBytes = 4;
            UInt8 alloc[32 + kSubdeviceParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 12, &kSubdeviceClass, 4);
            __builtin_memcpy(alloc + 20, &kSubdeviceParamsBytes, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 20;
            else
                badReason = 21;
        } else if (subdeviceAllocResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            constexpr UInt32 kVaspaceClass = 0x90f1;
            constexpr UInt32 kVaspaceParamsBytes = 48;
            UInt8 alloc[32 + kVaspaceParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kVaspaceHandle, 4);
            __builtin_memcpy(alloc + 12, &kVaspaceClass, 4);
            __builtin_memcpy(alloc + 20, &kVaspaceParamsBytes, 4);
            // index=GPU_NEW and all remaining fields zero ask RM to select
            // its native VA range and big-page size for this AD103 device.
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 23;
            else
                badReason = 22;
        } else if (vaspaceAllocResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00041;
            constexpr UInt32 kMemoryClass = 0x40; // NV01_MEMORY_LOCAL_USER
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            // NV_MEMORY_ALLOCATION_PARAMS: owner, DMA type, 4 KiB page,
            // 4096-byte size. Location defaults to local video memory.
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6;
            constexpr UInt32 kPage4KAttr = 0x00800000;
            constexpr UInt64 kMemoryBytes = 4096;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kMemoryBytes, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 26;
            else
                badReason = 23;
        } else if (localMemoryAllocResponse) {
            // 0.5.9: identical backing flow to 0.5.8 (probe 0x41, GPFIFO 0x42
            // 4 KiB, USERD 0x43) plus the missing status-queue gate entries for
            // phases 42/43. Root cause of the 0.5.6-0.5.8 stalls: GSP-RM HAD
            // answered every second alloc with all-zero success (blocked-data
            // proves 8192@0x3f1dbb000, 4096@0x3f1dbc000 twice), but
            // expectedPostInit parked function-103 at phases 42/43 as blocked
            // before the decoder ran. Dead BAR2/map/unmap blocks below still
            // name 0x40; unreachable in this build, reconciled later.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00042;
            constexpr UInt32 kMemoryClass = 0x40;  // NV01_MEMORY_LOCAL_USER
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6;
            constexpr UInt32 kAllocFlags = 0x00010100;
            constexpr UInt32 kPage4KAttr = 0x00800000;
            constexpr UInt64 kMemoryBytes = 4096;
            constexpr UInt64 kAlignment = 4096;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 40, &kAllocFlags, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kMemoryBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlignment, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 42;
            else
                badReason = 24;
        } else if (gpfifoBackingAllocResponse) {
            // USERD backing (4 KiB) as a third NV01_MEMORY_LOCAL_USER object.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00043;
            constexpr UInt32 kMemoryClass = 0x40;  // NV01_MEMORY_LOCAL_USER
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6;
            constexpr UInt32 kAllocFlags = 0x00010100;
            constexpr UInt32 kPage4KAttr = 0x00800000;
            constexpr UInt64 kMemoryBytes = 4096;
            constexpr UInt64 kAlignment = 4096;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 40, &kAllocFlags, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kMemoryBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlignment, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 43;
            else
                badReason = 35;
        } else if (userdBackingAllocResponse) {
            channelBackingComplete_ = true;
            setProperty("NVGspControl-channel-backing-complete",
                        channelBackingComplete_);
            setProperty("NVGspControl-channel-backing-probe-offset",
                        localMemoryOffset_, 64);
            setProperty("NVGspControl-channel-backing-gpfifo-offset",
                        gpfifoBackingOffset_, 64);
            setProperty("NVGspControl-channel-backing-gpfifo-size",
                        gpfifoBackingSize_, 64);
            setProperty("NVGspControl-channel-backing-userd-offset",
                        userdBackingOffset_, 64);
            setProperty("NVGspControl-channel-backing-userd-size",
                        userdBackingSize_, 64);
            // CPU-RM asks physical RM for the exact CE fault-method-buffer
            // size before constructing the mirrored TSG/channel descriptors.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kCommand = 0x20802a08;
            constexpr UInt32 kParamsBytes = 4;
            UInt8 control[24 + kParamsBytes]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 49;
            else
                badReason = 41;
        } else if (methodSizeResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00044;
            constexpr UInt32 kMemoryClass = 0x40;
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6;
            constexpr UInt32 kAllocFlags = 0x00010100;
            constexpr UInt32 kPage4KAttr = 0x00800000;
            constexpr UInt64 kMemoryBytes = 4096;
            constexpr UInt64 kAlignment = 4096;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 40, &kAllocFlags, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kMemoryBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlignment, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 50;
            else
                badReason = 42;
        } else if ((methodSizeAnswered && !methodSizeResponse) ||
                   (instanceBackingAnswered && !instanceBackingAllocResponse) ||
                   (methodBackingAnswered && !methodBackingAllocResponse)) {
            // 0.7.0: split-backing refused (CE size unsupported or backing
            // alloc rejected). Degrade to the proven 0.6.9 shape: zero the
            // split descriptors and continue at the GPU-VA window instead
            // of parking with the tree held. Success polls never reach
            // here (their ok-flag is true), so attribution stays clean.
            instanceBackingOffset_ = instanceBackingSize_ = 0;
            methodBackingOffset_ = methodBackingSize_ = 0;
            setProperty("NVGspControl-channel-split-backing-skipped", true);
            setProperty("NVGspControl-channel-split-backing-complete", false);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVirtualHandle = 0xc0d050a0;
            constexpr UInt32 kVirtualClass = 0x50a0;
            constexpr UInt32 kParamsBytes = 128;
            UInt8 alloc[32 + kParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kVirtualHandle, 4);
            __builtin_memcpy(alloc + 12, &kVirtualClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kVirtualFlags = 0x08080100;
            constexpr UInt32 kPage4KAttr = 0x00800000;
            constexpr UInt64 kBytes = 4096;
            constexpr UInt64 kAlignment = 4096;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            __builtin_memcpy(alloc + 40, &kVirtualFlags, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlignment, 8);
            __builtin_memcpy(alloc + 140, &kVaspaceHandle, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 29;
            else
                badReason = 36;
        } else if (instanceBackingAllocResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00045;
            constexpr UInt32 kMemoryClass = 0x40;
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6;
            constexpr UInt32 kAllocFlags = 0x00010100;
            // 4 KiB + CPU cached. CPU-RM normally uses cached SYSMEM for the
            // fault method buffer; FBMEM cached is the supported fallback
            // available through this GSP-local allocation path.
            constexpr UInt32 kPage4KAttr = 0x20800000;
            const UInt64 kMemoryBytes = methodBufferBytes_;
            constexpr UInt64 kAlignment = 4096;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 40, &kAllocFlags, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kMemoryBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlignment, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 51;
            else
                badReason = 43;
        } else if (methodBackingAllocResponse) {
            setProperty("NVGspControl-channel-split-backing-complete", true);
            // NV01_CONTEXT_DMA is not exposed by the bare-metal GSP server
            // (0.6.7 returned NV_ERR_NOT_SUPPORTED). hObjectError=0 is valid
            // in kernel_channel.c, so proceed directly to the GPU-VA window.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVirtualHandle = 0xc0d050a0;
            constexpr UInt32 kVirtualClass = 0x50a0;
            constexpr UInt32 kParamsBytes = 128;
            UInt8 alloc[32 + kParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kVirtualHandle, 4);
            __builtin_memcpy(alloc + 12, &kVirtualClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kVirtualFlags = 0x08080100;
            constexpr UInt32 kPage4KAttr = 0x00800000;
            constexpr UInt64 kBytes = 4096;
            constexpr UInt64 kAlignment = 4096;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            __builtin_memcpy(alloc + 40, &kVirtualFlags, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlignment, 8);
            __builtin_memcpy(alloc + 140, &kVaspaceHandle, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 29;
            else
                badReason = 36;
        } else if (errBackingAllocResponse) {
            // 0.6.4: bind the error backing memory as NV01_CONTEXT_DMA
            // (upstream errorCtxDma shape: KERNEL mapping + HASH_TABLE DISABLE,
            // offset 0, limit 4095). ContextDma requires the same parent as
            // hMemory, and backing object 0x44 is under the subdevice.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kCtxDmaHandle = 0xc0d00045;
            constexpr UInt32 kCtxDmaClass = nvgsp::kContextDma;
            nvgsp::NvCtxDmaAllocParams ctx{};
            ctx.hMemory = 0xc0d00044;
            ctx.flags = nvgsp::kCtxDmaFlagsKernelNoHash;
            ctx.limit = 4095;
            constexpr UInt32 kParamsBytes = sizeof(ctx);
            UInt8 alloc[32 + kParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kCtxDmaHandle, 4);
            __builtin_memcpy(alloc + 12, &kCtxDmaClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &ctx, kParamsBytes);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 48;
            else
                badReason = 39;
        } else if (errCtxAllocReturned && !errCtxAllocResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 40;
        } else if (errCtxAllocResponse) {
            // GPU-VA window for the channel GPFIFO (proven NV50 alloc).
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVirtualHandle = 0xc0d050a0;
            constexpr UInt32 kVirtualClass = 0x50a0;  // NV50_MEMORY_VIRTUAL
            constexpr UInt32 kParamsBytes = 128;
            UInt8 alloc[32 + kParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kVirtualHandle, 4);
            __builtin_memcpy(alloc + 12, &kVirtualClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kVirtualFlags = 0x08080100;
            constexpr UInt32 kPage4KAttr = 0x00800000;
            constexpr UInt64 kBytes = 4096;
            constexpr UInt64 kAlignment = 4096;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            __builtin_memcpy(alloc + 40, &kVirtualFlags, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlignment, 8);
            __builtin_memcpy(alloc + 140, &kVaspaceHandle, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 29;
            else
                badReason = 40;
        } else if (virtualMemoryAllocResponse) {
            // 0.6.2: resolve the live PTE location for the virtual GPFIFO
            // window before mapping (GET_PDE_INFO, proven in 0.4.6).
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            constexpr UInt32 kParamsBytes = 208;
            UInt8 control[24 + kParamsBytes]{};
            constexpr UInt32 kCommand = 0x00801809;
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kDeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            __builtin_memcpy(control + 24, &virtualOffset_, 8);
            __builtin_memcpy(control + 24 + 200, &kVaspaceHandle, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 35;
            else
                badReason = 37;
        } else if (channelAllocReturned) {
            channelAllocOk_ = channelAllocResponse;
            setProperty("NVGspControl-channel-alloc-ok", channelAllocOk_);
            setProperty("NVGspControl-channel-handle-cid", channelCid_, 32);
            if (channelAllocResponse) {
                // 0.7.1: the channel lives — bind it to GRAPHICS exactly
                // like Linux BindAndScheduleChannel (NVA06F_CTRL_CMD_BIND
                // 0xa06f0104 + NvU32 engineType).
                constexpr UInt32 kClientHandle = 0xc0d00001;
                constexpr UInt32 kChannelHandle = 0xc0d0c56f;
                constexpr UInt32 kCommand = 0xa06f0104;
                constexpr UInt32 kParamsBytes = 4;
                UInt8 control[24 + kParamsBytes]{};
                __builtin_memcpy(control, &kClientHandle, 4);
                __builtin_memcpy(control + 4, &kChannelHandle, 4);
                __builtin_memcpy(control + 8, &kCommand, 4);
                __builtin_memcpy(control + 16, &kParamsBytes, 4);
                constexpr UInt32 kEngine = nvgsp::kEngineTypeCopy0;  // 0.7.7
                __builtin_memcpy(control + 24, &kEngine, 4);
                if (init_.enqueueRpc(76, control, sizeof(control)))
                    postInitPhase_ = 52;
                else
                    badReason = 47;
            } else {
                // Explicit RM refusal: restore the fresh GPFIFO PTE before
                // recursively freeing the client tree, so the probe leaves
                // no trace.
                UInt64 original = originalPte_;
                PraminPteResult restore{};
                gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                    pci_, pte4KAddress_, &original, true, false, &restore);
                setProperty("NVGspControl-gpfifo-pte-restore-readback",
                            restore.observedPte, 64);
                setProperty("NVGspControl-gpfifo-pte-restore-window-restored",
                            restore.windowRestored, 32);
                setProperty("NVGspControl-gpfifo-pte-restored",
                            gpfifoPteRestored_);
                constexpr UInt32 kClientHandle = 0xc0d00001;
                UInt8 freeParams[16]{};
                __builtin_memcpy(freeParams, &kClientHandle, 4);
                __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                    postInitPhase_ = 32;
                else
                    badReason = 38;
            }
        } else if (bindReturned) {
            setProperty("NVGspControl-channel-bind-ok", bindResponse);
            if (bindResponse) {
                // 0.7.2: channel-level SCHEDULE is a proven 0x1f (0.7.1),
                // so skip it and bind the RM-assigned TSG instead
                // (NVA06C_CTRL_CMD_BIND 0xa06c0102 + NvU32 engineType).
                // A zero TSG handle means the echo never decoded: never
                // guess, tear down with a reason instead.
                if (channelTsgHandle_ == 0) {
                    setProperty("NVGspControl-channel-skipped-reason",
                                "tsg-handle-unknown");
                    UInt64 original = originalPte_;
                    PraminPteResult restore{};
                    gpfifoPteRestored_ = gpfifoPteInstalled_ &&
                        praminPteAccess(pci_, pte4KAddress_, &original,
                                        true, false, &restore);
                    setProperty("NVGspControl-gpfifo-pte-restored",
                                gpfifoPteRestored_);
                    constexpr UInt32 kClientHandle = 0xc0d00001;
                    UInt8 freeParams[16]{};
                    __builtin_memcpy(freeParams, &kClientHandle, 4);
                    __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                    if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                        postInitPhase_ = 32;
                    else
                        badReason = 38;
                } else {
                    constexpr UInt32 kClientHandle = 0xc0d00001;
                    constexpr UInt32 kCommand = 0xa06c0102;
                    constexpr UInt32 kParamsBytes = 4;
                    UInt8 control[24 + kParamsBytes]{};
                    __builtin_memcpy(control, &kClientHandle, 4);
                    __builtin_memcpy(control + 4, &channelTsgHandle_, 4);
                    __builtin_memcpy(control + 8, &kCommand, 4);
                    __builtin_memcpy(control + 16, &kParamsBytes, 4);
                    constexpr UInt32 kEngine = nvgsp::kEngineTypeCopy0;  // 0.7.7
                    __builtin_memcpy(control + 24, &kEngine, 4);
                    if (init_.enqueueRpc(76, control, sizeof(control)))
                        postInitPhase_ = 54;
                    else
                        badReason = 49;
                }
            } else {
                setProperty("NVGspControl-channel-skipped-reason",
                            "bind-refused");
                UInt64 original = originalPte_;
                PraminPteResult restore{};
                gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                    pci_, pte4KAddress_, &original, true, false, &restore);
                setProperty("NVGspControl-gpfifo-pte-restored",
                            gpfifoPteRestored_);
                constexpr UInt32 kClientHandle = 0xc0d00001;
                UInt8 freeParams[16]{};
                __builtin_memcpy(freeParams, &kClientHandle, 4);
                __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                    postInitPhase_ = 32;
                else
                    badReason = 38;
            }
        } else if (scheduleReturned) {
            setProperty("NVGspControl-channel-schedule-ok", scheduleResponse);
            // 0.7.1 stops at a scheduled channel: no method is submitted
            // yet. Teardown is identical on success and refusal.
            setProperty("NVGspControl-channel-live",
                        scheduleResponse);
            if (!scheduleResponse)
                setProperty("NVGspControl-channel-skipped-reason",
                            "schedule-refused");
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restore-readback",
                        restore.observedPte, 64);
            setProperty("NVGspControl-gpfifo-pte-restore-window-restored",
                        restore.windowRestored, 32);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        } else if (tsgBindReturned) {
            // 0.7.3: TSG bound — first query its hardware state
            // (NVA06C_CTRL_CMD_GET_INFO 0xa06c0106, output-only tsgID)
            // before attempting the schedule again.
            setProperty("NVGspControl-tsg-bind-ok", tsgBindResponse);
            if (tsgBindResponse) {
                constexpr UInt32 kClientHandle = 0xc0d00001;
                constexpr UInt32 kCommand = 0xa06c0106;
                constexpr UInt32 kParamsBytes = 4;
                UInt8 control[24 + kParamsBytes]{};
                __builtin_memcpy(control, &kClientHandle, 4);
                __builtin_memcpy(control + 4, &channelTsgHandle_, 4);
                __builtin_memcpy(control + 8, &kCommand, 4);
                __builtin_memcpy(control + 16, &kParamsBytes, 4);
                if (init_.enqueueRpc(76, control, sizeof(control)))
                    postInitPhase_ = 56;
                else
                    badReason = 51;
            } else {
                setProperty("NVGspControl-channel-skipped-reason",
                            "tsg-bind-refused");
                UInt64 original = originalPte_;
                PraminPteResult restore{};
                gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                    pci_, pte4KAddress_, &original, true, false, &restore);
                setProperty("NVGspControl-gpfifo-pte-restored",
                            gpfifoPteRestored_);
                constexpr UInt32 kClientHandle = 0xc0d00001;
                UInt8 freeParams[16]{};
                __builtin_memcpy(freeParams, &kClientHandle, 4);
                __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                    postInitPhase_ = 32;
                else
                    badReason = 38;
            }
        } else if (tsgInfoReturned) {
            // 0.8.0: TSG state captured — NO schedule call (6/6 refused).
            // Start the FIFO intel sweep: P1a FIFO_GET_INFO (0x20801109)
            // with {size=2, idx 6/7, engineType=GR}.
            setProperty("NVGspControl-tsg-info-ok", tsgInfoResponse);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kCommand = 0x20801109;
            constexpr UInt32 kParamsBytes = 2056;
            UInt8 control[24 + kParamsBytes]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            constexpr UInt32 kTblSize = 2;
            constexpr UInt32 kIdxGroups = 6;
            constexpr UInt32 kIdxChram = 7;
            constexpr UInt32 kEngine = nvgsp::kEngineTypeGraphics;
            __builtin_memcpy(control + 24, &kTblSize, 4);
            __builtin_memcpy(control + 28, &kIdxGroups, 4);
            __builtin_memcpy(control + 36, &kIdxChram, 4);
            __builtin_memcpy(control + 24 + 2052, &kEngine, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 58;
            else
                badReason = 53;
        } else if (tsgTimesliceReturned) {
            // 0.7.5: timeslice answered — teardown. The schedule attempt
            // (55) is skipped: four 0x1f reproductions are enough, and a
            // fifth adds no information to this matrix.
            setProperty("NVGspControl-tsg-timeslice-ok",
                        tsgTimesliceResponse);
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restore-readback",
                        restore.observedPte, 64);
            setProperty("NVGspControl-gpfifo-pte-restore-window-restored",
                        restore.windowRestored, 32);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        } else if (fifoInfoReturned) {
            // 0.8.0 P1b: FIFO_GET_USERD_LOCATION (0x2080110D), 8 B out.
            // Answered phases always advance (refusal is data, the sweep
            // phases are independent); only a watchdog stall tears down.
            setProperty("NVGspControl-fifo-info-ok", fifoInfoResponse);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kCommand = 0x2080110d;
            constexpr UInt32 kParamsBytes = 8;
            UInt8 control[24 + kParamsBytes]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 59;
            else
                badReason = 54;
        } else if (userdLocReturned) {
            // 0.8.0 P1c: GPU_GET_ENGINE_PARTNERLIST (0x20800147), 144 B.
            setProperty("NVGspControl-userd-loc-ok", userdLocResponse);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kCommand = 0x20800147;
            constexpr UInt32 kParamsBytes = 144;
            UInt8 control[24 + kParamsBytes]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            constexpr UInt32 kEngine = nvgsp::kEngineTypeGraphics;
            constexpr UInt32 kClass = nvgsp::kAmpereChannelGpfifoA;
            __builtin_memcpy(control + 24, &kEngine, 4);
            __builtin_memcpy(control + 28, &kClass, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 60;
            else
                badReason = 55;
        } else if (partnerReturned) {
            // 0.8.0 P1d: GPU_GET_ENGINE_RUNLIST_PRI_BASE (0x20800179).
            // 3 x 84 x u32: engineList[0]=GR, rest NULL, outputs zero.
            setProperty("NVGspControl-partner-ok", partnerResponse);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kCommand = 0x20800179;
            constexpr UInt32 kParamsBytes = 1008;
            constexpr UInt32 kEngines = 84;
            UInt8 control[24 + kParamsBytes]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            constexpr UInt32 kEngine = nvgsp::kEngineTypeGraphics;
            constexpr UInt32 kNull = 0xffffffff;
            __builtin_memcpy(control + 24, &kEngine, 4);
            for (UInt32 i = 1; i < kEngines; ++i) {
                UInt32 off = 24 + i * 4;
                __builtin_memcpy(control + off, &kNull, 4);
            }
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 61;
            else
                badReason = 56;
        } else if (priBaseReturned) {
            // 0.8.0 P2: GPFIFO_GET_WORK_SUBMIT_TOKEN (0xc36f0108) on the
            // bound channel. Record only — no doorbell write (no usermode
            // mapping exists).
            setProperty("NVGspControl-pribase-ok", priBaseResponse);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kChannelHandle = 0xc0d0c56f;
            constexpr UInt32 kCommand = 0xc36f0108;
            constexpr UInt32 kParamsBytes = 4;
            UInt8 control[24 + kParamsBytes]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kChannelHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 62;
            else
                badReason = 57;
        } else if (tokenReturned) {
            // 0.8.0 P3: DMA_FLUSH (0x801805) on the device, FB flush.
            // Display-path benign probe; no channel alloc here.
            setProperty("NVGspControl-submit-token-ok", tokenResponse);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kCommand = 0x801805;
            constexpr UInt32 kParamsBytes = 4;
            UInt8 control[24 + kParamsBytes]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kDeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            constexpr UInt32 kFbFlush = 0x4;
            __builtin_memcpy(control + 24, &kFbFlush, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 63;
            else
                badReason = 58;
        } else if (flushReturned && (experimentFlags_ & 2)) {
            // 0.51.0: display probe — 64 KiB display instance memory
            // (nouveau r535_disp_oneinit RAMIN), 64 KiB aligned VRAM.
            setProperty("NVGspControl-dma-flush-ok", flushResponse);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00060;
            constexpr UInt32 kMemoryClass = 0x40;
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6, kFlags = 0x100;
            constexpr UInt32 kAttr = 0x10800000;  // 4K pages, contiguous
            constexpr UInt64 kBytes = 0x10000, kAlign = 0x10000;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 40, &kFlags, 4);
            __builtin_memcpy(alloc + 56, &kAttr, 4);
            __builtin_memcpy(alloc + 96, &kBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlign, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 200;
            else
                badReason = 120;
        } else if (dispInstMemReturned && dispInstMemResponse) {
            // 0.51.0: hand the RAMIN to physical RM.
            // 0.54.0: zero it first (RAMHT + ctxdma objects live here).
            setProperty("NVGspControl-dispinst-zeroed",
                        praminZeroRange(pci_, dispInstOffset_, 0x10000));
            UInt8 control[24 + 24]{};
            constexpr UInt32 kCommand = 0x20800a49;
            constexpr UInt32 kParamsBytes = 24;
            __builtin_memcpy(control, &internalClient_, 4);
            __builtin_memcpy(control + 4, &internalSubdevice_, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            const UInt64 size = 0x10000;
            const UInt32 space = 2 /* ADDR_FBMEM */, cache = 2 /* WC */;
            __builtin_memcpy(control + 24, &dispInstOffset_, 8);
            __builtin_memcpy(control + 32, &size, 8);
            __builtin_memcpy(control + 40, &space, 4);
            __builtin_memcpy(control + 44, &cache, 4);
            if (internalClient_ && internalSubdevice_ &&
                init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 201;
            else
                badReason = 121;
        } else if ((dispInstWriteReturned && dispInstWriteResponse) ||
                   (flushReturned && !(experimentFlags_ & 2))) {
            // 0.8.1: intel sweep complete — graft the display path onto the
            // live tree. NVC770_DISPLAY (0xC770) under the device, NULL
            // params (nvkms-exact: nvRmApiAlloc NULL).
            setProperty("NVGspControl-dma-flush-ok", flushResponse);
            setProperty("NVGspControl-intel-sweep-complete", true);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kDisplayHandle = 0xc0d0c770;
            constexpr UInt32 kDisplayClass = nvgsp::kDispDisplay;
            constexpr UInt32 kParamsBytes = 0;
            UInt8 alloc[32]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kDisplayHandle, 4);
            __builtin_memcpy(alloc + 12, &kDisplayClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 64;
            else
                badReason = 59;
        } else if (dispReturned && !dispResponse) {
            // Display parent refused (perm gate 0x1B? class? arg?) —
            // record + teardown; the c77d chain needs this handle.
            setProperty("NVGspControl-disp-parent-ok", false);
            setProperty("NVGspControl-display-skipped-reason",
                        "disp-parent-refused");
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        } else if (dispResponse) {
            // 0.8.4: PB backing — VRAM revert (0.8.2 bytes: bare-metal
            // GSP-RM refused 0x3E sysmem 0x22, so VRAM is the only viable
            // backing). Class 0x40 under SUBDEVICE, owner+dmaType6+4K
            // attr, 4096 bytes — the proven 0.8.2 shape.
            setProperty("NVGspControl-disp-parent-ok", true);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00048;
            constexpr UInt32 kMemoryClass = 0x40;
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6;
            constexpr UInt32 kPage4KAttr = 0x00800000;
            constexpr UInt64 kBytes = 4096;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kBytes, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 65;
            else
                badReason = 60;
        } else if (pbBackingReturned && !pbBackingResponse) {
            setProperty("NVGspControl-pb-backing-ok", false);
            setProperty("NVGspControl-display-skipped-reason",
                        "pb-backing-refused");
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        } else if (pbBackingResponse) {
            // 0.8.4: PB ctxdma — SUBDEVICE parent (0.8.2 bytes; matches
            // the VRAM backing's parent per context_dma.c:151-154).
            setProperty("NVGspControl-pb-backing-ok", true);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kCtxDmaHandle = 0xc0d00049;
            constexpr UInt32 kCtxDmaClass = nvgsp::kContextDma;
            nvgsp::NvCtxDmaAllocParams ctx{};
            ctx.hMemory = 0xc0d00048;
            ctx.flags = nvgsp::kCtxDmaFlagsRwNoHash;
            ctx.limit = 4095;
            constexpr UInt32 kParamsBytes = sizeof(ctx);
            UInt8 alloc[32 + kParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kCtxDmaHandle, 4);
            __builtin_memcpy(alloc + 12, &kCtxDmaClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &ctx, kParamsBytes);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 66;
            else
                badReason = 61;
        } else if (pbCtxdmaReturned && !pbCtxdmaResponse) {
            setProperty("NVGspControl-pb-ctxdma-ok", false);
            setProperty("NVGspControl-display-skipped-reason",
                        "pb-ctxdma-refused");
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        } else if (pbCtxdmaResponse) {
            // 0.8.4: per-subdevice notifier backing — the skipped
            // RmAllocEvoChannel step-1 (nvkms-rm.c:2666). Same VRAM 0x40
            // shape as the PB backing, fresh handle 0xc0d0004a.
            setProperty("NVGspControl-pb-ctxdma-ok", true);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d0004a;
            constexpr UInt32 kMemoryClass = 0x40;
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6;
            constexpr UInt32 kPage4KAttr = 0x00800000;
            constexpr UInt64 kBytes = 4096;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 56, &kPage4KAttr, 4);
            __builtin_memcpy(alloc + 96, &kBytes, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 67;
            else
                badReason = 63;
        } else if (notifyBackingReturned && !notifyBackingResponse) {
            setProperty("NVGspControl-notify-backing-ok", false);
            setProperty("NVGspControl-display-skipped-reason",
                        "notify-backing-refused");
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        } else if (notifyBackingResponse) {
            // 0.8.4: notifier ctxdma — subdevice parent (matches the
            // notifier backing's parent per context_dma.c:151-154),
            // limit 4095, handle 0xc0d0004b. hObjectNotify stays 0 in
            // the channel params (RM ignores it; DISPLAY-ABI §3).
            setProperty("NVGspControl-notify-backing-ok", true);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kCtxDmaHandle = 0xc0d0004b;
            constexpr UInt32 kCtxDmaClass = nvgsp::kContextDma;
            nvgsp::NvCtxDmaAllocParams ctx{};
            ctx.hMemory = 0xc0d0004a;
            ctx.flags = nvgsp::kCtxDmaFlagsRwNoHash;
            ctx.limit = 4095;
            constexpr UInt32 kParamsBytes = sizeof(ctx);
            UInt8 alloc[32 + kParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kCtxDmaHandle, 4);
            __builtin_memcpy(alloc + 12, &kCtxDmaClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &ctx, kParamsBytes);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 68;
            else
                badReason = 64;
        } else if (notifyCtxdmaReturned && !notifyCtxdmaResponse) {
            setProperty("NVGspControl-notify-ctxdma-ok", false);
            setProperty("NVGspControl-display-skipped-reason",
                        "notify-ctxdma-refused");
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        } else if (notifyCtxdmaResponse) {
            // 0.8.5: mirror CPU-RM's split display path. C77D allocation is
            // host-side and ends in osMapGPU; physical RM receives only this
            // internal control on the INTERNAL subdevice. The PB is local
            // VRAM, so Ada's DISPv0502 path selects PHYS_NVM.
            setProperty("NVGspControl-notify-ctxdma-ok", true);
            nvgsp::NvDispChannelPushbufferParams params{};
            params.addressSpace = nvgsp::kAddressSpaceFbmem;
            params.physicalAddr = dispPbBackingOffset_;
            params.limit = 4095;
            params.hclass = nvgsp::kDispCoreChannelDma;
            params.valid = 1;
            params.pbTargetAperture = nvgsp::kPbTargetPhysNvm;
            params.channelPBSize = nvgsp::kChannelPbSize4K;
            constexpr UInt32 kParamsBytes = sizeof(params);
            UInt8 control[24 + kParamsBytes]{};
            __builtin_memcpy(control, &internalClient_, 4);
            __builtin_memcpy(control + 4, &internalSubdevice_, 4);
            constexpr UInt32 kCommand =
                nvgsp::kDispSetChannelPushbuffer;
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            __builtin_memcpy(control + 24, &params, kParamsBytes);
            if (dispPbBackingOffset_ != 0 && internalClient_ != 0 &&
                internalSubdevice_ != 0 &&
                init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 69;
            else
                badReason = 62;
        } else if (dispKickFlushReturned) {
            // 0.8.7: the two-dword UPDATE-with-zero-data packet is visible
            // in FB. Publish PUT=8 exactly as nvkms-dma.c does, then observe
            // whether the display core channel consumes it (GET=8).
            // 0.8.8: read-only discriminator for the 0.8.7 not-consumed
            // result. PUT-after-write proves the PUT write landed; the
            // 500 ms poll rules out a slow fetch; PUT-after-poll plus the
            // post-kick PB re-readback show whether the engine touched
            // anything. No new RPC, no new BAR0 offsets, same PUT write.
            setProperty("NVGspControl-disp-kick-flush-ok",
                        dispKickFlushResponse);
            IOMemoryMap *bar0Map = sharedBar0Map(pci_);
            UInt32 putBefore = 0, getBefore = 0, getAfter = 0;
            UInt32 putAfterWrite = 0, putAfterPoll = 0, pollRounds = 0;
            bool kickOk = false, consumed = false, surveyOk = false;
            if (dispKickFlushResponse && bar0Map &&
                bar0Map->getLength() >= 0x00680008) {
                Bar0Io bar0{bar0Map};
                kickOk = bar0.read(0x00680000, &putBefore) &&
                    bar0.read(0x00680004, &getBefore);
                __asm__ __volatile__("sfence" : : : "memory");
                kickOk = kickOk && bar0.write(0x00680000, 8);
                surveyOk = kickOk &&
                    bar0.read(0x00680000, &putAfterWrite);
                for (UInt32 wait = 0; kickOk && wait < 10000; ++wait) {
                    pollRounds = wait + 1;
                    if (!bar0.read(0x00680004, &getAfter)) {
                        kickOk = false;
                        break;
                    }
                    if ((getAfter & 0x3ffU) == 8) {
                        consumed = true;
                        break;
                    }
                    IODelay(50);
                }
                surveyOk = surveyOk && kickOk &&
                    bar0.read(0x00680000, &putAfterPoll);
            }
            if (bar0Map) bar0Map->release();
            UInt64 pbAfter = 0;
            bool pbAfterOk = false;
            if (dispPbBackingOffset_ != 0) {
                PraminPteResult pbAfterRead{};
                pbAfterOk = praminPteAccess(pci_, dispPbBackingOffset_,
                                            &pbAfter, false, false,
                                            &pbAfterRead);
            }
            setProperty("NVGspControl-disp-kick-put-before", putBefore, 32);
            setProperty("NVGspControl-disp-kick-get-before", getBefore, 32);
            setProperty("NVGspControl-disp-kick-get-after", getAfter, 32);
            setProperty("NVGspControl-disp-kick-write-ok", kickOk);
            setProperty("NVGspControl-disp-kick-consumed", consumed);
            setProperty("NVGspControl-disp-kick-put-after-write",
                        putAfterWrite, 32);
            setProperty("NVGspControl-disp-kick-put-after-poll",
                        putAfterPoll, 32);
            setProperty("NVGspControl-disp-kick-poll-rounds",
                        pollRounds, 32);
            setProperty("NVGspControl-disp-kick-survey-ok", surveyOk);
            setProperty("NVGspControl-disp-kick-pb-after", pbAfter, 64);
            setProperty("NVGspControl-disp-kick-pb-after-ok", pbAfterOk);
            if (!consumed)
                setProperty("NVGspControl-channel-skipped-reason",
                            "disp-update-not-consumed");
            // 0.8.9: probe 1 — byte-identical phase-69 control except
            // valid=0 on channel 0. Tests whether the closed handler
            // validates anything at all.
            nvgsp::NvDispChannelPushbufferParams probe1{};
            probe1.addressSpace = nvgsp::kAddressSpaceFbmem;
            probe1.physicalAddr = dispPbBackingOffset_;
            probe1.limit = 4095;
            probe1.hclass = nvgsp::kDispCoreChannelDma;
            probe1.valid = 0;
            probe1.pbTargetAperture = nvgsp::kPbTargetPhysNvm;
            probe1.channelPBSize = nvgsp::kChannelPbSize4K;
            constexpr UInt32 kProbe1Bytes = sizeof(probe1);
            UInt8 probe1Control[24 + kProbe1Bytes]{};
            __builtin_memcpy(probe1Control, &internalClient_, 4);
            __builtin_memcpy(probe1Control + 4, &internalSubdevice_, 4);
            constexpr UInt32 kProbeCommand =
                nvgsp::kDispSetChannelPushbuffer;
            __builtin_memcpy(probe1Control + 8, &kProbeCommand, 4);
            __builtin_memcpy(probe1Control + 16, &kProbe1Bytes, 4);
            __builtin_memcpy(probe1Control + 24, &probe1, kProbe1Bytes);
            if (dispPbBackingOffset_ != 0 && internalClient_ != 0 &&
                internalSubdevice_ != 0 &&
                init_.enqueueRpc(76, probe1Control, sizeof(probe1Control)))
                postInitPhase_ = 71;
            else
                badReason = 65;
        } else if (dispHandlerProbe1Returned) {
            // 0.8.9: probe 2 — valid=1 on nonexistent channel instance 7.
            // Tests instance validation. Always advances: any answered
            // status is data.
            setProperty("NVGspControl-disp-handler-probe1-ok",
                        dispHandlerProbe1Response);
            nvgsp::NvDispChannelPushbufferParams probe2{};
            probe2.addressSpace = nvgsp::kAddressSpaceFbmem;
            probe2.physicalAddr = dispPbBackingOffset_;
            probe2.limit = 4095;
            probe2.hclass = nvgsp::kDispCoreChannelDma;
            probe2.channelInstance = 7;
            probe2.valid = 1;
            probe2.pbTargetAperture = nvgsp::kPbTargetPhysNvm;
            probe2.channelPBSize = nvgsp::kChannelPbSize4K;
            constexpr UInt32 kProbe2Bytes = sizeof(probe2);
            UInt8 probe2Control[24 + kProbe2Bytes]{};
            __builtin_memcpy(probe2Control, &internalClient_, 4);
            __builtin_memcpy(probe2Control + 4, &internalSubdevice_, 4);
            constexpr UInt32 kProbe2Command =
                nvgsp::kDispSetChannelPushbuffer;
            __builtin_memcpy(probe2Control + 8, &kProbe2Command, 4);
            __builtin_memcpy(probe2Control + 16, &kProbe2Bytes, 4);
            __builtin_memcpy(probe2Control + 24, &probe2, kProbe2Bytes);
            if (dispPbBackingOffset_ != 0 && internalClient_ != 0 &&
                internalSubdevice_ != 0 &&
                init_.enqueueRpc(76, probe2Control, sizeof(probe2Control)))
                postInitPhase_ = 72;
            else
                badReason = 66;
        } else if (dispHandlerProbe2Returned) {
            // 0.8.9: record probe 2, re-survey the ch0 PUT/GET window
            // (did valid=0 change anything?), then teardown as usual.
            setProperty("NVGspControl-disp-handler-probe2-ok",
                        dispHandlerProbe2Response);
            IOMemoryMap *bar0Map = sharedBar0Map(pci_);
            UInt32 put = 0, get = 0;
            bool surveyOk = false;
            if (bar0Map && bar0Map->getLength() >= 0x00680008) {
                Bar0Io bar0{bar0Map};
                surveyOk = bar0.read(0x00680000, &put) &&
                    bar0.read(0x00680004, &get);
            }
            if (bar0Map) bar0Map->release();
            setProperty("NVGspControl-disp-handler-probe2-put", put, 32);
            setProperty("NVGspControl-disp-handler-probe2-get", get, 32);
            setProperty("NVGspControl-disp-handler-probe2-survey-ok",
                        surveyOk);
            // 0.9.0: function-97 channel schedule retry — 16B
            // {hClient, hObject=channel, cmd, bEnable=1} per
            // rpc_ctrl_gpfifo_schedule_v1A_0A (params v03_00 = bEnable
            // only). Single variable vs 0.7.1's function-76 0x1f.
            constexpr UInt32 kSched97Client = 0xc0d00001;
            constexpr UInt32 kSched97Channel = 0xc0d0c56f;
            constexpr UInt32 kSched97Cmd = 0xa06f0103;
            constexpr UInt32 kSched97Enable = 1;
            UInt8 sched97[16]{};
            __builtin_memcpy(sched97, &kSched97Client, 4);
            __builtin_memcpy(sched97 + 4, &kSched97Channel, 4);
            __builtin_memcpy(sched97 + 8, &kSched97Cmd, 4);
            __builtin_memcpy(sched97 + 12, &kSched97Enable, 4);
            if (init_.enqueueRpc(97, sched97, sizeof(sched97)))
                postInitPhase_ = 73;
            else
                badReason = 67;
        } else if (sched97chanReturned) {
            // 0.9.0: channel-97 answered — record, then TSG-level via
            // 97 (cmd 0xa06c0101 on the RM-assigned TSG handle).
            // Always advances: any answered status is data.
            setProperty("NVGspControl-sched97chan-ok",
                        sched97chanResponse);
            if (channelTsgHandle_ == 0) {
                setProperty("NVGspControl-channel-skipped-reason",
                            "tsg-handle-unknown");
                UInt64 original = originalPte_;
                PraminPteResult restore{};
                gpfifoPteRestored_ = gpfifoPteInstalled_ &&
                    praminPteAccess(pci_, pte4KAddress_, &original,
                                    true, false, &restore);
                setProperty("NVGspControl-gpfifo-pte-restored",
                            gpfifoPteRestored_);
                constexpr UInt32 kClientHandle = 0xc0d00001;
                UInt8 freeParams[16]{};
                __builtin_memcpy(freeParams, &kClientHandle, 4);
                __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                    postInitPhase_ = 32;
                else
                    badReason = 38;
            } else {
                constexpr UInt32 kSched97Client = 0xc0d00001;
                constexpr UInt32 kSched97Cmd = 0xa06c0101;
                constexpr UInt32 kSched97Enable = 1;
                UInt8 sched97[16]{};
                __builtin_memcpy(sched97, &kSched97Client, 4);
                __builtin_memcpy(sched97 + 4, &channelTsgHandle_, 4);
                __builtin_memcpy(sched97 + 8, &kSched97Cmd, 4);
                __builtin_memcpy(sched97 + 12, &kSched97Enable, 4);
                if (init_.enqueueRpc(97, sched97, sizeof(sched97)))
                    postInitPhase_ = 74;
                else
                    badReason = 68;
            }
        } else if (sched97tsgReturned) {
            // 0.9.0: TSG-97 answered — record, then 0.9.1's C372 probe.
            // (If either schedule unexpectedly ENABLED, the channel is
            // idle with an empty GPFIFO; recursive free tears down
            // scheduling normally. No doorbell is rung in this probe.)
            setProperty("NVGspControl-sched97tsg-ok",
                        sched97tsgResponse);
            // 0.9.1: NVC372_DISPLAY_SW (0xC372) under the device, NULL
            // params (nvkms-exact: nvEvoAllocRmCtrlObjectC3). Class gate
            // open (live class list). Any answer advances.
            constexpr UInt32 kClientHandle75 = 0xc0d00001;
            constexpr UInt32 kDeviceHandle75 = 0xc0d00080;
            constexpr UInt32 kSwHandle75 = 0xc0d0c372;
            constexpr UInt32 kSwClass75 = 0xc372;
            constexpr UInt32 kParamsBytes75 = 0;
            UInt8 alloc75[32]{};
            __builtin_memcpy(alloc75, &kClientHandle75, 4);
            __builtin_memcpy(alloc75 + 4, &kDeviceHandle75, 4);
            __builtin_memcpy(alloc75 + 8, &kSwHandle75, 4);
            __builtin_memcpy(alloc75 + 12, &kSwClass75, 4);
            __builtin_memcpy(alloc75 + 20, &kParamsBytes75, 4);
            if (init_.enqueueRpc(103, alloc75, sizeof(alloc75)))
                postInitPhase_ = 75;
            else
                badReason = 69;
        } else if (dispC372Returned) {
            // 0.9.1: record the C372 answer, run the post-FWSEC head +
            // PUT/GET survey (read-only, live-proven offsets), then
            // teardown. Survey runs even if the alloc refused.
            setProperty("NVGspControl-disp-c372-ok",
                        dispC372Response);
            IOMemoryMap *bar0Map = sharedBar0Map(pci_);
            UInt32 heads[4] = {0, 0, 0, 0};
            UInt32 put = 0, get = 0;
            bool surveyOk = false;
            if (bar0Map && bar0Map->getLength() >= 0x00680008) {
                Bar0Io bar0{bar0Map};
                surveyOk = true;
                for (UInt32 head = 0; head < 4; ++head) {
                    if (!bar0.read(0x00612078 + head * 0x800,
                                   &heads[head])) {
                        surveyOk = false;
                        break;
                    }
                }
                surveyOk = surveyOk &&
                    bar0.read(0x00680000, &put) &&
                    bar0.read(0x00680004, &get);
            }
            if (bar0Map) bar0Map->release();
            setProperty("NVGspControl-disp-survey-head0", heads[0], 32);
            setProperty("NVGspControl-disp-survey-head1", heads[1], 32);
            setProperty("NVGspControl-disp-survey-head2", heads[2], 32);
            setProperty("NVGspControl-disp-survey-head3", heads[3], 32);
            setProperty("NVGspControl-disp-survey-put", put, 32);
            setProperty("NVGspControl-disp-survey-get", get, 32);
            setProperty("NVGspControl-disp-survey-ok", surveyOk);
            // 0.9.2: post-FWSEC GOP surface hash (BAR1+0, first 4 KiB).
            // Match against the stage-time hash => FWSEC preserved the
            // GOP surface (frozen splash); mismatch => clobbered.
            UInt64 gopHashPost = 0;
            bool gopHashPostOk = false;
            IODeviceMemory *bar1Survey =
                bar1Dev();
            if (bar1Survey && bar1Survey->getLength() >= 4096) {
                IOMemoryMap *bar1Map = mapBar1Head(4096);
                if (bar1Map && bar1Map->getLength() >= 4096) {
                    const volatile UInt8 *bytes =
                        reinterpret_cast<const volatile UInt8 *>(
                            static_cast<uintptr_t>(
                                bar1Map->getVirtualAddress()));
                    UInt64 hash = 14695981039346656037ULL;
                    for (unsigned i = 0; i < 4096; ++i) {
                        hash ^= bytes[i];
                        hash *= 1099511628211ULL;
                    }
                    gopHashPost = hash;
                    gopHashPostOk = true;
                }
                if (bar1Map) bar1Map->release();
            }
            setProperty("NVGspControl-gop-surface-hash-post",
                        gopHashPost, 64);
            setProperty("NVGspControl-gop-surface-hash-post-ok",
                        gopHashPostOk);
            // 0.9.4: post-boot first 64 bytes (forensics vs pre-words).
            UInt64 postWords[8] = {};
            if (gopHashPostOk) {
                IODeviceMemory *bar1Post =
                    bar1Dev();
                if (bar1Post && bar1Post->getLength() >= 64) {
                    IOMemoryMap *bar1Map = mapBar1Head(64);
                    if (bar1Map && bar1Map->getLength() >= 64) {
                        const volatile UInt8 *bytes =
                            reinterpret_cast<const volatile UInt8 *>(
                                static_cast<uintptr_t>(
                                    bar1Map->getVirtualAddress()));
                        for (unsigned w = 0; w < 8; ++w) {
                            UInt64 word = 0;
                            for (unsigned b = 0; b < 8; ++b)
                                word |= static_cast<UInt64>(bytes[w * 8 + b])
                                    << (b * 8);
                            postWords[w] = word;
                        }
                    }
                    if (bar1Map) bar1Map->release();
                }
            }
            setProperty("NVGspControl-gop-surface-post-w0",
                        postWords[0], 64);
            setProperty("NVGspControl-gop-surface-post-w1",
                        postWords[1], 64);
            setProperty("NVGspControl-gop-surface-post-w2",
                        postWords[2], 64);
            setProperty("NVGspControl-gop-surface-post-w3",
                        postWords[3], 64);
            setProperty("NVGspControl-gop-surface-post-w4",
                        postWords[4], 64);
            setProperty("NVGspControl-gop-surface-post-w5",
                        postWords[5], 64);
            setProperty("NVGspControl-gop-surface-post-w6",
                        postWords[6], 64);
            setProperty("NVGspControl-gop-surface-post-w7",
                        postWords[7], 64);
            setProperty("NVGspControl-gop-surface-preserved",
                        gopSurfaceHashPreOk_ && gopHashPostOk &&
                        gopHashPost == gopSurfaceHashPre_);
            // 0.9.2: NV04_DISPLAY_COMMON (0x73) under the device, NULL
            // params (nvkms-exact: displayCommonHandle). Class gate
            // open (live class list). Any answer advances.
            constexpr UInt32 kClientHandle76 = 0xc0d00001;
            constexpr UInt32 kDeviceHandle76 = 0xc0d00080;
            constexpr UInt32 kCommonHandle76 = 0xc0d00073;
            constexpr UInt32 kCommonClass76 = 0x73;
            constexpr UInt32 kParamsBytes76 = 0;
            UInt8 alloc76[32]{};
            __builtin_memcpy(alloc76, &kClientHandle76, 4);
            __builtin_memcpy(alloc76 + 4, &kDeviceHandle76, 4);
            __builtin_memcpy(alloc76 + 8, &kCommonHandle76, 4);
            __builtin_memcpy(alloc76 + 12, &kCommonClass76, 4);
            __builtin_memcpy(alloc76 + 20, &kParamsBytes76, 4);
            if (init_.enqueueRpc(103, alloc76, sizeof(alloc76)))
                postInitPhase_ = 76;
            else
                badReason = 70;
        } else if (dispCommonReturned) {
            // 0.9.2: record the display-common answer. 0.9.7: chain
            // SYSTEM_GET_SUPPORTED on the 0x73 object (first RM
            // display query) instead of tearing down.
            setProperty("NVGspControl-disp-common-ok",
                        dispCommonResponse);
            constexpr UInt32 kClientHandle77 = 0xc0d00001;
            constexpr UInt32 kObject77 = 0xc0d00073;
            constexpr UInt32 kCommand77 = 0x730107;
            constexpr UInt32 kParamsBytes77 = 12;
            UInt8 control77[24 + kParamsBytes77]{};
            __builtin_memcpy(control77, &kClientHandle77, 4);
            __builtin_memcpy(control77 + 4, &kObject77, 4);
            __builtin_memcpy(control77 + 8, &kCommand77, 4);
            __builtin_memcpy(control77 + 16, &kParamsBytes77, 4);
            if (init_.enqueueRpc(76, control77, sizeof(control77)))
                postInitPhase_ = 77;
            else
                badReason = 71;
        } else if (dispSysSupportedReturned) {
            // 0.9.7: record supported-mask answer, then GET_NUM_HEADS.
            setProperty("NVGspControl-disp-sys-supported-ok",
                        dispSysSupportedResponse);
            constexpr UInt32 kClientHandle78 = 0xc0d00001;
            constexpr UInt32 kObject78 = 0xc0d00073;
            constexpr UInt32 kCommand78 = 0x730102;
            constexpr UInt32 kParamsBytes78 = 12;
            UInt8 control78[24 + kParamsBytes78]{};
            __builtin_memcpy(control78, &kClientHandle78, 4);
            __builtin_memcpy(control78 + 4, &kObject78, 4);
            __builtin_memcpy(control78 + 8, &kCommand78, 4);
            __builtin_memcpy(control78 + 16, &kParamsBytes78, 4);
            if (init_.enqueueRpc(76, control78, sizeof(control78)))
                postInitPhase_ = 78;
            else
                badReason = 72;
        } else if (dispSysNumHeadsReturned) {
            // 0.9.7: record head count, then GET_ACTIVE on head0.
            setProperty("NVGspControl-disp-sys-num-heads-ok",
                        dispSysNumHeadsResponse);
            constexpr UInt32 kClientHandle79 = 0xc0d00001;
            constexpr UInt32 kObject79 = 0xc0d00073;
            constexpr UInt32 kCommand79 = 0x73010c;
            constexpr UInt32 kParamsBytes79 = 16;
            UInt8 control79[24 + kParamsBytes79]{};
            __builtin_memcpy(control79, &kClientHandle79, 4);
            __builtin_memcpy(control79 + 4, &kObject79, 4);
            __builtin_memcpy(control79 + 8, &kCommand79, 4);
            __builtin_memcpy(control79 + 16, &kParamsBytes79, 4);
            if (init_.enqueueRpc(76, control79, sizeof(control79)))
                postInitPhase_ = 79;
            else
                badReason = 73;
        } else if (dispSysActiveReturned) {
            // 0.9.8: record head0-active answer, then GET_ACTIVE head1.
            setProperty("NVGspControl-disp-sys-active-ok",
                        dispSysActiveResponse);
            constexpr UInt32 kClientHandle80 = 0xc0d00001;
            constexpr UInt32 kObject80 = 0xc0d00073;
            constexpr UInt32 kCommand80 = 0x73010c;
            constexpr UInt32 kParamsBytes80 = 16;
            constexpr UInt32 kHead80 = 1;
            UInt8 control80[24 + kParamsBytes80]{};
            __builtin_memcpy(control80, &kClientHandle80, 4);
            __builtin_memcpy(control80 + 4, &kObject80, 4);
            __builtin_memcpy(control80 + 8, &kCommand80, 4);
            __builtin_memcpy(control80 + 16, &kParamsBytes80, 4);
            __builtin_memcpy(control80 + 28, &kHead80, 4);
            if (init_.enqueueRpc(76, control80, sizeof(control80)))
                postInitPhase_ = 80;
            else
                badReason = 74;
        } else if (dispSysActive1Returned) {
            // 0.9.8: record head1-active answer, then GET_ACTIVE head2.
            setProperty("NVGspControl-disp-sys-active1-ok",
                        dispSysActive1Response);
            constexpr UInt32 kClientHandle81 = 0xc0d00001;
            constexpr UInt32 kObject81 = 0xc0d00073;
            constexpr UInt32 kCommand81 = 0x73010c;
            constexpr UInt32 kParamsBytes81 = 16;
            constexpr UInt32 kHead81 = 2;
            UInt8 control81[24 + kParamsBytes81]{};
            __builtin_memcpy(control81, &kClientHandle81, 4);
            __builtin_memcpy(control81 + 4, &kObject81, 4);
            __builtin_memcpy(control81 + 8, &kCommand81, 4);
            __builtin_memcpy(control81 + 16, &kParamsBytes81, 4);
            __builtin_memcpy(control81 + 28, &kHead81, 4);
            if (init_.enqueueRpc(76, control81, sizeof(control81)))
                postInitPhase_ = 81;
            else
                badReason = 75;
        } else if (dispSysActive2Returned) {
            // 0.9.8: record head2-active answer, then GET_ACTIVE head3.
            setProperty("NVGspControl-disp-sys-active2-ok",
                        dispSysActive2Response);
            constexpr UInt32 kClientHandle82 = 0xc0d00001;
            constexpr UInt32 kObject82 = 0xc0d00073;
            constexpr UInt32 kCommand82 = 0x73010c;
            constexpr UInt32 kParamsBytes82 = 16;
            constexpr UInt32 kHead82 = 3;
            UInt8 control82[24 + kParamsBytes82]{};
            __builtin_memcpy(control82, &kClientHandle82, 4);
            __builtin_memcpy(control82 + 4, &kObject82, 4);
            __builtin_memcpy(control82 + 8, &kCommand82, 4);
            __builtin_memcpy(control82 + 16, &kParamsBytes82, 4);
            __builtin_memcpy(control82 + 28, &kHead82, 4);
            if (init_.enqueueRpc(76, control82, sizeof(control82)))
                postInitPhase_ = 82;
            else
                badReason = 76;
        } else if (dispSysActive3Returned) {
            // 0.9.8: record head3-active answer, then GET_CONNECT_STATE
            // over the supported mask (0x7F00) with DEFAULT detect.
            setProperty("NVGspControl-disp-sys-active3-ok",
                        dispSysActive3Response);
            constexpr UInt32 kClientHandle83 = 0xc0d00001;
            constexpr UInt32 kObject83 = 0xc0d00073;
            constexpr UInt32 kCommand83 = 0x730108;
            constexpr UInt32 kParamsBytes83 = 16;
            constexpr UInt32 kMask83 = 0x7f00;
            UInt8 control83[24 + kParamsBytes83]{};
            __builtin_memcpy(control83, &kClientHandle83, 4);
            __builtin_memcpy(control83 + 4, &kObject83, 4);
            __builtin_memcpy(control83 + 8, &kCommand83, 4);
            __builtin_memcpy(control83 + 16, &kParamsBytes83, 4);
            __builtin_memcpy(control83 + 32, &kMask83, 4);
            if (init_.enqueueRpc(76, control83, sizeof(control83)))
                postInitPhase_ = 83;
            else
                badReason = 77;
        } else if (dispConnectReturned) {
            // 0.9.8: record connect-state answer, then GET_BOOT_DISPLAYS.
            setProperty("NVGspControl-disp-connect-ok",
                        dispConnectResponse);
            constexpr UInt32 kClientHandle84 = 0xc0d00001;
            constexpr UInt32 kObject84 = 0xc0d00073;
            constexpr UInt32 kCommand84 = 0x73011e;
            constexpr UInt32 kParamsBytes84 = 8;
            UInt8 control84[24 + kParamsBytes84]{};
            __builtin_memcpy(control84, &kClientHandle84, 4);
            __builtin_memcpy(control84 + 4, &kObject84, 4);
            __builtin_memcpy(control84 + 8, &kCommand84, 4);
            __builtin_memcpy(control84 + 16, &kParamsBytes84, 4);
            if (init_.enqueueRpc(76, control84, sizeof(control84)))
                postInitPhase_ = 84;
            else
                badReason = 78;
        } else if (dispBootDisplaysReturned) {
            // 0.9.9: record boot-displays answer, then GET_SCANLINE head0.
            setProperty("NVGspControl-disp-boot-displays-ok",
                        dispBootDisplaysResponse);
            constexpr UInt32 kClientHandle85 = 0xc0d00001;
            constexpr UInt32 kObject85 = 0xc0d00073;
            constexpr UInt32 kCommand85 = 0x730104;
            constexpr UInt32 kParamsBytes85 = 20;
            constexpr UInt32 kHead85 = 0;
            UInt8 control85[24 + kParamsBytes85]{};
            __builtin_memcpy(control85, &kClientHandle85, 4);
            __builtin_memcpy(control85 + 4, &kObject85, 4);
            __builtin_memcpy(control85 + 8, &kCommand85, 4);
            __builtin_memcpy(control85 + 16, &kParamsBytes85, 4);
            __builtin_memcpy(control85 + 28, &kHead85, 4);
            if (init_.enqueueRpc(76, control85, sizeof(control85)))
                postInitPhase_ = 85;
            else
                badReason = 79;
        } else if (dispScanlineReturned) {
            // 0.9.9: record scanline answer, then GET_VBLANK_COUNTER.
            setProperty("NVGspControl-disp-scanline-ok",
                        dispScanlineResponse);
            constexpr UInt32 kClientHandle86 = 0xc0d00001;
            constexpr UInt32 kObject86 = 0xc0d00073;
            constexpr UInt32 kCommand86 = 0x730105;
            constexpr UInt32 kParamsBytes86 = 16;
            constexpr UInt32 kHead86 = 0;
            UInt8 control86[24 + kParamsBytes86]{};
            __builtin_memcpy(control86, &kClientHandle86, 4);
            __builtin_memcpy(control86 + 4, &kObject86, 4);
            __builtin_memcpy(control86 + 8, &kCommand86, 4);
            __builtin_memcpy(control86 + 16, &kParamsBytes86, 4);
            __builtin_memcpy(control86 + 28, &kHead86, 4);
            if (init_.enqueueRpc(76, control86, sizeof(control86)))
                postInitPhase_ = 86;
            else
                badReason = 80;
        } else if (dispVblankReturned) {
            // 0.9.9: record vblank answer, then GET_HEAD_ROUTING_MAP.
            setProperty("NVGspControl-disp-vblank-ok",
                        dispVblankResponse);
            constexpr UInt32 kClientHandle87 = 0xc0d00001;
            constexpr UInt32 kObject87 = 0xc0d00073;
            constexpr UInt32 kCommand87 = 0x73010b;
            constexpr UInt32 kParamsBytes87 = 20;
            constexpr UInt32 kMask87 = 0x7f00;
            UInt8 control87[24 + kParamsBytes87]{};
            __builtin_memcpy(control87, &kClientHandle87, 4);
            __builtin_memcpy(control87 + 4, &kObject87, 4);
            __builtin_memcpy(control87 + 8, &kCommand87, 4);
            __builtin_memcpy(control87 + 16, &kParamsBytes87, 4);
            __builtin_memcpy(control87 + 28, &kMask87, 4);
            if (init_.enqueueRpc(76, control87, sizeof(control87)))
                postInitPhase_ = 87;
            else
                badReason = 81;
        } else if (dispRoutingReturned) {
            // 0.10.0: record routing-map answer, then C372
            // GET_ACTIVE_VIEWPORT_POINT_IN window 0.
            setProperty("NVGspControl-disp-routing-ok",
                        dispRoutingResponse);
            constexpr UInt32 kClientHandle88 = 0xc0d00001;
            constexpr UInt32 kObject88 = 0xc0d0c372;
            constexpr UInt32 kCommand88 = 0xc3720104;
            constexpr UInt32 kParamsBytes88 = 16;
            constexpr UInt32 kWindow88 = 0;
            UInt8 control88[24 + kParamsBytes88]{};
            __builtin_memcpy(control88, &kClientHandle88, 4);
            __builtin_memcpy(control88 + 4, &kObject88, 4);
            __builtin_memcpy(control88 + 8, &kCommand88, 4);
            __builtin_memcpy(control88 + 16, &kParamsBytes88, 4);
            __builtin_memcpy(control88 + 28, &kWindow88, 4);
            if (init_.enqueueRpc(76, control88, sizeof(control88)))
                postInitPhase_ = 88;
            else
                badReason = 82;
        } else if (dispViewportReturned) {
            // 0.11.0: record viewport answer, then DFP_GET_INFO on
            // display 0x200 via the 0x73 object (nvkms GetDfpInfo).
            setProperty("NVGspControl-disp-viewport-ok",
                        dispViewportResponse);
            constexpr UInt32 kClientHandle89 = 0xc0d00001;
            constexpr UInt32 kObject89 = 0xc0d00073;
            constexpr UInt32 kCommand89 = 0x731140;
            constexpr UInt32 kParamsBytes89 = 16;
            constexpr UInt32 kDisplay89 = 0x200;
            UInt8 control89[24 + kParamsBytes89]{};
            __builtin_memcpy(control89, &kClientHandle89, 4);
            __builtin_memcpy(control89 + 4, &kObject89, 4);
            __builtin_memcpy(control89 + 8, &kCommand89, 4);
            __builtin_memcpy(control89 + 16, &kParamsBytes89, 4);
            __builtin_memcpy(control89 + 28, &kDisplay89, 4);
            if (init_.enqueueRpc(76, control89, sizeof(control89)))
                postInitPhase_ = 89;
            else
                badReason = 83;
        } else if (dispDfpReturned) {
            // 0.13.0: record DFP info answer, then live DDC EDID read
            // (COPY_CACHE_NO = flags 0, what nvkms uses; the 0.12.0
            // cached read returned an empty cache).
            setProperty("NVGspControl-disp-dfp-ok",
                        dispDfpResponse);
            constexpr UInt32 kClientHandle90 = 0xc0d00001;
            constexpr UInt32 kObject90 = 0xc0d00073;
            constexpr UInt32 kCommand90 = 0x730245;
            constexpr UInt32 kParamsBytes90 = 2064;
            constexpr UInt32 kDisplay90 = 0x200;
            constexpr UInt32 kFlags90 = 0x0;
            UInt8 control90[24 + kParamsBytes90]{};
            __builtin_memcpy(control90, &kClientHandle90, 4);
            __builtin_memcpy(control90 + 4, &kObject90, 4);
            __builtin_memcpy(control90 + 8, &kCommand90, 4);
            __builtin_memcpy(control90 + 16, &kParamsBytes90, 4);
            __builtin_memcpy(control90 + 28, &kDisplay90, 4);
            __builtin_memcpy(control90 + 36, &kFlags90, 4);
            if (init_.enqueueRpc(76, control90, sizeof(control90)))
                postInitPhase_ = 90;
            else
                badReason = 84;
        } else if (dispEdidReturned) {
            // 0.14.0: record EDID answer, then PCLK_LIMIT on 0x200.
            setProperty("NVGspControl-disp-edid-ok",
                        dispEdidResponse);
            constexpr UInt32 kClientHandle91 = 0xc0d00001;
            constexpr UInt32 kObject91 = 0xc0d00073;
            constexpr UInt32 kCommand91 = 0x73028a;
            constexpr UInt32 kParamsBytes91 = 20;
            constexpr UInt32 kDisplay91 = 0x200;
            UInt8 control91[24 + kParamsBytes91]{};
            __builtin_memcpy(control91, &kClientHandle91, 4);
            __builtin_memcpy(control91 + 4, &kObject91, 4);
            __builtin_memcpy(control91 + 8, &kCommand91, 4);
            __builtin_memcpy(control91 + 16, &kParamsBytes91, 4);
            __builtin_memcpy(control91 + 28, &kDisplay91, 4);
            if (init_.enqueueRpc(76, control91, sizeof(control91)))
                postInitPhase_ = 91;
            else
                badReason = 85;
        } else if (dispPclkReturned) {
            // 0.14.0: record pclk answer, then OR_GET_INFO index 0.
            setProperty("NVGspControl-disp-pclk-ok",
                        dispPclkResponse);
            constexpr UInt32 kClientHandle92 = 0xc0d00001;
            constexpr UInt32 kObject92 = 0xc0d00073;
            constexpr UInt32 kCommand92 = 0x73028b;
            constexpr UInt32 kParamsBytes92 = 52;
            constexpr UInt32 kDisplay92 = 0x200;
            constexpr UInt32 kIndex92 = 0;
            UInt8 control92[24 + kParamsBytes92]{};
            __builtin_memcpy(control92, &kClientHandle92, 4);
            __builtin_memcpy(control92 + 4, &kObject92, 4);
            __builtin_memcpy(control92 + 8, &kCommand92, 4);
            __builtin_memcpy(control92 + 16, &kParamsBytes92, 4);
            __builtin_memcpy(control92 + 28, &kDisplay92, 4);
            __builtin_memcpy(control92 + 32, &kIndex92, 4);
            if (init_.enqueueRpc(76, control92, sizeof(control92)))
                postInitPhase_ = 92;
            else
                badReason = 86;
        } else if (dispOrReturned) {
            // 0.15.0: record OR info answer, then SYSTEM_GET_CAPS_V2.
            setProperty("NVGspControl-disp-or-ok",
                        dispOrResponse);
            constexpr UInt32 kClientHandle93 = 0xc0d00001;
            constexpr UInt32 kObject93 = 0xc0d00073;
            constexpr UInt32 kCommand93 = 0x730101;
            constexpr UInt32 kParamsBytes93 = 2;
            UInt8 control93[24 + kParamsBytes93]{};
            __builtin_memcpy(control93, &kClientHandle93, 4);
            __builtin_memcpy(control93 + 4, &kObject93, 4);
            __builtin_memcpy(control93 + 8, &kCommand93, 4);
            __builtin_memcpy(control93 + 16, &kParamsBytes93, 4);
            if (init_.enqueueRpc(76, control93, sizeof(control93)))
                postInitPhase_ = 93;
            else
                badReason = 87;
        } else if (dispCapsReturned) {
            // 0.15.0: record caps answer, then VBLANK_ENABLE head0.
            setProperty("NVGspControl-disp-caps-ok",
                        dispCapsResponse);
            constexpr UInt32 kClientHandle94 = 0xc0d00001;
            constexpr UInt32 kObject94 = 0xc0d00073;
            constexpr UInt32 kCommand94 = 0x730106;
            constexpr UInt32 kParamsBytes94 = 12;
            constexpr UInt32 kHead94 = 0;
            UInt8 control94[24 + kParamsBytes94]{};
            __builtin_memcpy(control94, &kClientHandle94, 4);
            __builtin_memcpy(control94 + 4, &kObject94, 4);
            __builtin_memcpy(control94 + 8, &kCommand94, 4);
            __builtin_memcpy(control94 + 16, &kParamsBytes94, 4);
            __builtin_memcpy(control94 + 28, &kHead94, 4);
            if (init_.enqueueRpc(76, control94, sizeof(control94)))
                postInitPhase_ = 94;
            else
                badReason = 88;
        } else if (dispVbEnReturned) {
            // 0.16.0: record vblank-enable answer, then C372
            // IS_MODE_POSSIBLE with a zeroed struct (numHeads=0).
            setProperty("NVGspControl-disp-vblank-en-ok",
                        dispVbEnResponse);
            constexpr UInt32 kClientHandle95 = 0xc0d00001;
            constexpr UInt32 kObject95 = 0xc0d0c372;
            constexpr UInt32 kCommand95 = 0xc3720101;
            constexpr UInt32 kParamsBytes95 = 2048;
            UInt8 control95[24 + kParamsBytes95]{};
            __builtin_memcpy(control95, &kClientHandle95, 4);
            __builtin_memcpy(control95 + 4, &kObject95, 4);
            __builtin_memcpy(control95 + 8, &kCommand95, 4);
            __builtin_memcpy(control95 + 16, &kParamsBytes95, 4);
            if (init_.enqueueRpc(76, control95, sizeof(control95)))
                postInitPhase_ = 95;
            else
                badReason = 89;
        } else if (dispImpReturned) {
            // 0.17.0: record empty-validation answer, then
            // IS_MODE_POSSIBLE with head0 CTA-861 4K60 timing
            // (594 MHz; blank (4016,2168)-(4400,2250)), windows 0.
            setProperty("NVGspControl-disp-imp-ok",
                        dispImpResponse);
            constexpr UInt32 kClientHandle96 = 0xc0d00001;
            constexpr UInt32 kObject96 = 0xc0d0c372;
            constexpr UInt32 kCommand96 = 0xc3720101;
            constexpr UInt32 kParamsBytes96 = 2048;
            constexpr UInt8 kNumHeads96 = 1;
            constexpr UInt32 kPixClk96 = 594000;
            constexpr UInt32 kRw96 = 3840, kRh96 = 2160;
            constexpr UInt32 kBsx96 = 4016, kBsy96 = 2168;
            constexpr UInt32 kBex96 = 4400, kBey96 = 2250;
            UInt8 control96[24 + kParamsBytes96]{};
            __builtin_memcpy(control96, &kClientHandle96, 4);
            __builtin_memcpy(control96 + 4, &kObject96, 4);
            __builtin_memcpy(control96 + 8, &kCommand96, 4);
            __builtin_memcpy(control96 + 16, &kParamsBytes96, 4);
            control96[24 + 4] = kNumHeads96;
            __builtin_memcpy(control96 + 24 + 8 + 4, &kPixClk96, 4);
            __builtin_memcpy(control96 + 24 + 8 + 8, &kRw96, 4);
            __builtin_memcpy(control96 + 24 + 8 + 12, &kRh96, 4);
            __builtin_memcpy(control96 + 24 + 8 + 16, &kBsx96, 4);
            __builtin_memcpy(control96 + 24 + 8 + 20, &kBsy96, 4);
            __builtin_memcpy(control96 + 24 + 8 + 24, &kBex96, 4);
            __builtin_memcpy(control96 + 24 + 8 + 28, &kBey96, 4);
            if (init_.enqueueRpc(76, control96, sizeof(control96)))
                postInitPhase_ = 96;
            else
                badReason = 90;
        } else if (dispModeReturned) {
            // 0.18.0: record 4K60-validation answer, then
            // GET_CONNECTOR_DATA on display 0x200 (72B).
            setProperty("NVGspControl-disp-mode-ok",
                        dispModeResponse);
            constexpr UInt32 kClientHandle97 = 0xc0d00001;
            constexpr UInt32 kObject97 = 0xc0d00073;
            constexpr UInt32 kCommand97 = 0x730250;
            constexpr UInt32 kParamsBytes97 = 72;
            constexpr UInt32 kDisplay97 = 0x200;
            UInt8 control97[24 + kParamsBytes97]{};
            __builtin_memcpy(control97, &kClientHandle97, 4);
            __builtin_memcpy(control97 + 4, &kObject97, 4);
            __builtin_memcpy(control97 + 8, &kCommand97, 4);
            __builtin_memcpy(control97 + 16, &kParamsBytes97, 4);
            __builtin_memcpy(control97 + 28, &kDisplay97, 4);
            if (init_.enqueueRpc(76, control97, sizeof(control97)))
                postInitPhase_ = 97;
            else
                badReason = 91;
        } else if (dispConnReturned) {
            // 0.19.0: record connector answer, then MC_GET_ARCH_INFO
            // on the subdevice (13B).
            setProperty("NVGspControl-disp-conn-ok",
                        dispConnResponse);
            constexpr UInt32 kClientHandle98 = 0xc0d00001;
            constexpr UInt32 kObject98 = 0xc0d02080;
            constexpr UInt32 kCommand98 = 0x20801701;
            constexpr UInt32 kParamsBytes98 = 13;
            UInt8 control98[24 + kParamsBytes98]{};
            __builtin_memcpy(control98, &kClientHandle98, 4);
            __builtin_memcpy(control98 + 4, &kObject98, 4);
            __builtin_memcpy(control98 + 8, &kCommand98, 4);
            __builtin_memcpy(control98 + 16, &kParamsBytes98, 4);
            if (init_.enqueueRpc(76, control98, sizeof(control98)))
                postInitPhase_ = 98;
            else
                badReason = 92;
        } else if (dispArchReturned) {
            // 0.19.0: record arch answer, then GPU name string.
            setProperty("NVGspControl-disp-arch-ok",
                        dispArchResponse);
            constexpr UInt32 kClientHandle99 = 0xc0d00001;
            constexpr UInt32 kObject99 = 0xc0d02080;
            constexpr UInt32 kCommand99 = 0x20800110;
            constexpr UInt32 kParamsBytes99 = 68;
            constexpr UInt32 kFlags99 = 0;
            UInt8 control99[24 + kParamsBytes99]{};
            __builtin_memcpy(control99, &kClientHandle99, 4);
            __builtin_memcpy(control99 + 4, &kObject99, 4);
            __builtin_memcpy(control99 + 8, &kCommand99, 4);
            __builtin_memcpy(control99 + 16, &kParamsBytes99, 4);
            __builtin_memcpy(control99 + 28, &kFlags99, 4);
            if (init_.enqueueRpc(76, control99, sizeof(control99)))
                postInitPhase_ = 99;
            else
                badReason = 93;
        } else if (dispNameReturned) {
            // 0.20.0: record name answer, then FIFO physical
            // channel count on the subdevice (8B).
            setProperty("NVGspControl-disp-name-ok",
                        dispNameResponse);
            constexpr UInt32 kClientHandle100 = 0xc0d00001;
            constexpr UInt32 kObject100 = 0xc0d02080;
            constexpr UInt32 kCommand100 = 0x20801108;
            constexpr UInt32 kParamsBytes100 = 8;
            UInt8 control100[24 + kParamsBytes100]{};
            __builtin_memcpy(control100, &kClientHandle100, 4);
            __builtin_memcpy(control100 + 4, &kObject100, 4);
            __builtin_memcpy(control100 + 8, &kCommand100, 4);
            __builtin_memcpy(control100 + 16, &kParamsBytes100, 4);
            if (init_.enqueueRpc(76, control100, sizeof(control100)))
                postInitPhase_ = 100;
            else
                badReason = 94;
        } else if (dispFifoReturned) {
            // 0.21.0: record FIFO count answer, then allocated
            // channels on runlist 0 (516B).
            setProperty("NVGspControl-disp-fifo-ok",
                        dispFifoResponse);
            constexpr UInt32 kClientHandle101 = 0xc0d00001;
            constexpr UInt32 kObject101 = 0xc0d02080;
            constexpr UInt32 kCommand101 = 0x20801119;
            constexpr UInt32 kParamsBytes101 = 516;
            constexpr UInt32 kRunlist101 = 0;
            UInt8 control101[24 + kParamsBytes101]{};
            __builtin_memcpy(control101, &kClientHandle101, 4);
            __builtin_memcpy(control101 + 4, &kObject101, 4);
            __builtin_memcpy(control101 + 8, &kCommand101, 4);
            __builtin_memcpy(control101 + 16, &kParamsBytes101, 4);
            __builtin_memcpy(control101 + 28, &kRunlist101, 4);
            if (init_.enqueueRpc(76, control101, sizeof(control101)))
                postInitPhase_ = 101;
            else
                badReason = 95;
        } else if (dispRunlistReturned) {
            // 0.22.0: record runlist answer, then BIND the channel
            // to GR (engineType=1). Empty GPFIFO idles if scheduled.
            setProperty("NVGspControl-disp-runlist-ok",
                        dispRunlistResponse);
            constexpr UInt32 kClientHandle102 = 0xc0d00001;
            constexpr UInt32 kChannelHandle102 = 0xc0d0c56f;
            constexpr UInt32 kCommand102 = 0xa06f0104;
            constexpr UInt32 kParamsBytes102 = 4;
            constexpr UInt32 kEngine102 = nvgsp::kEngineTypeGraphics;
            UInt8 control102[24 + kParamsBytes102]{};
            __builtin_memcpy(control102, &kClientHandle102, 4);
            __builtin_memcpy(control102 + 4, &kChannelHandle102, 4);
            __builtin_memcpy(control102 + 8, &kCommand102, 4);
            __builtin_memcpy(control102 + 16, &kParamsBytes102, 4);
            __builtin_memcpy(control102 + 24, &kEngine102, 4);
            if (init_.enqueueRpc(76, control102, sizeof(control102)))
                postInitPhase_ = 102;
            else
                badReason = 96;
        } else if (dispGrBindReturned) {
            // 0.23.0: record GR-bind answer, then re-query runlist 0
            // (same 516B command) to test post-bind scheduling.
            setProperty("NVGspControl-disp-grbind-ok",
                        dispGrBindResponse);
            constexpr UInt32 kClientHandle103 = 0xc0d00001;
            constexpr UInt32 kObject103 = 0xc0d02080;
            constexpr UInt32 kCommand103 = 0x20801119;
            constexpr UInt32 kParamsBytes103 = 516;
            constexpr UInt32 kRunlist103 = 0;
            UInt8 control103[24 + kParamsBytes103]{};
            __builtin_memcpy(control103, &kClientHandle103, 4);
            __builtin_memcpy(control103 + 4, &kObject103, 4);
            __builtin_memcpy(control103 + 8, &kCommand103, 4);
            __builtin_memcpy(control103 + 16, &kParamsBytes103, 4);
            __builtin_memcpy(control103 + 28, &kRunlist103, 4);
            if (init_.enqueueRpc(76, control103, sizeof(control103)))
                postInitPhase_ = 103;
            else
                badReason = 97;
        } else if (dispReRunReturned) {
            // 0.24.0: record re-query answer, then runlist 0 third
            // sample, then runlists 1 and 2 (same 516B command).
            setProperty("NVGspControl-disp-rerun-ok",
                        dispReRunResponse);
            constexpr UInt32 kClientHandle104 = 0xc0d00001;
            constexpr UInt32 kObject104 = 0xc0d02080;
            constexpr UInt32 kCommand104 = 0x20801119;
            constexpr UInt32 kParamsBytes104 = 516;
            constexpr UInt32 kRunlist104 = 0;
            UInt8 control104[24 + kParamsBytes104]{};
            __builtin_memcpy(control104, &kClientHandle104, 4);
            __builtin_memcpy(control104 + 4, &kObject104, 4);
            __builtin_memcpy(control104 + 8, &kCommand104, 4);
            __builtin_memcpy(control104 + 16, &kParamsBytes104, 4);
            __builtin_memcpy(control104 + 28, &kRunlist104, 4);
            if (init_.enqueueRpc(76, control104, sizeof(control104)))
                postInitPhase_ = 104;
            else
                badReason = 98;
        } else if (dispRl0cReturned) {
            // 0.24.0: record rl0 sample, then runlist 1.
            setProperty("NVGspControl-disp-rl0c-ok",
                        dispRl0cResponse);
            constexpr UInt32 kClientHandle105 = 0xc0d00001;
            constexpr UInt32 kObject105 = 0xc0d02080;
            constexpr UInt32 kCommand105 = 0x20801119;
            constexpr UInt32 kParamsBytes105 = 516;
            constexpr UInt32 kRunlist105 = 1;
            UInt8 control105[24 + kParamsBytes105]{};
            __builtin_memcpy(control105, &kClientHandle105, 4);
            __builtin_memcpy(control105 + 4, &kObject105, 4);
            __builtin_memcpy(control105 + 8, &kCommand105, 4);
            __builtin_memcpy(control105 + 16, &kParamsBytes105, 4);
            __builtin_memcpy(control105 + 28, &kRunlist105, 4);
            if (init_.enqueueRpc(76, control105, sizeof(control105)))
                postInitPhase_ = 105;
            else
                badReason = 99;
        } else if (dispRl1Returned) {
            // 0.24.0: record rl1 answer, then runlist 2.
            setProperty("NVGspControl-disp-rl1-ok",
                        dispRl1Response);
            constexpr UInt32 kClientHandle106 = 0xc0d00001;
            constexpr UInt32 kObject106 = 0xc0d02080;
            constexpr UInt32 kCommand106 = 0x20801119;
            constexpr UInt32 kParamsBytes106 = 516;
            constexpr UInt32 kRunlist106 = 2;
            UInt8 control106[24 + kParamsBytes106]{};
            __builtin_memcpy(control106, &kClientHandle106, 4);
            __builtin_memcpy(control106 + 4, &kObject106, 4);
            __builtin_memcpy(control106 + 8, &kCommand106, 4);
            __builtin_memcpy(control106 + 16, &kParamsBytes106, 4);
            __builtin_memcpy(control106 + 28, &kRunlist106, 4);
            if (init_.enqueueRpc(76, control106, sizeof(control106)))
                postInitPhase_ = 106;
            else
                badReason = 100;
        } else if (dispRl2Returned) {
            // 0.26.0: record rl2 answer, then post-bind
            // GPFIFO_SCHEDULE with exact 2B params
            // ({bEnable=1, bSkipSubmit=0} per the open header).
            setProperty("NVGspControl-disp-rl2-ok",
                        dispRl2Response);
            constexpr UInt32 kClientHandle107 = 0xc0d00001;
            constexpr UInt32 kChannelHandle107 = 0xc0d0c56f;
            constexpr UInt32 kCommand107 = 0xa06f0103;
            constexpr UInt32 kParamsBytes107 = 2;
            constexpr UInt8 kEnable107 = 1, kSkip107 = 0;
            UInt8 control107[24 + kParamsBytes107]{};
            __builtin_memcpy(control107, &kClientHandle107, 4);
            __builtin_memcpy(control107 + 4, &kChannelHandle107, 4);
            __builtin_memcpy(control107 + 8, &kCommand107, 4);
            __builtin_memcpy(control107 + 16, &kParamsBytes107, 4);
            control107[24] = kEnable107;
            control107[25] = kSkip107;
            if (init_.enqueueRpc(76, control107, sizeof(control107)))
                postInitPhase_ = 107;
            else
                badReason = 101;
        } else if (dispSchedReturned) {
            // 0.26.0: record schedule answer, then rl0 re-query.
            setProperty("NVGspControl-disp-sched-ok",
                        dispSchedResponse);
            constexpr UInt32 kClientHandle108 = 0xc0d00001;
            constexpr UInt32 kObject108 = 0xc0d02080;
            constexpr UInt32 kCommand108 = 0x20801119;
            constexpr UInt32 kParamsBytes108 = 516;
            constexpr UInt32 kRunlist108 = 0;
            UInt8 control108[24 + kParamsBytes108]{};
            __builtin_memcpy(control108, &kClientHandle108, 4);
            __builtin_memcpy(control108 + 4, &kObject108, 4);
            __builtin_memcpy(control108 + 8, &kCommand108, 4);
            __builtin_memcpy(control108 + 16, &kParamsBytes108, 4);
            __builtin_memcpy(control108 + 28, &kRunlist108, 4);
            if (init_.enqueueRpc(76, control108, sizeof(control108)))
                postInitPhase_ = 108;
            else
                badReason = 102;
        } else if (dispPostReturned) {
            // 0.27.0: record post-schedule rl0, then rl1 (109)
            // and rl2 (110) post-schedule samples (same 516B).
            setProperty("NVGspControl-disp-post-ok",
                        dispPostResponse);
            constexpr UInt32 kClientHandle109 = 0xc0d00001;
            constexpr UInt32 kObject109 = 0xc0d02080;
            constexpr UInt32 kCommand109 = 0x20801119;
            constexpr UInt32 kParamsBytes109 = 516;
            constexpr UInt32 kRunlist109 = 1;
            UInt8 control109[24 + kParamsBytes109]{};
            __builtin_memcpy(control109, &kClientHandle109, 4);
            __builtin_memcpy(control109 + 4, &kObject109, 4);
            __builtin_memcpy(control109 + 8, &kCommand109, 4);
            __builtin_memcpy(control109 + 16, &kParamsBytes109, 4);
            __builtin_memcpy(control109 + 28, &kRunlist109, 4);
            if (init_.enqueueRpc(76, control109, sizeof(control109)))
                postInitPhase_ = 109;
            else
                badReason = 103;
        } else if (dispPs1Returned) {
            // 0.27.0: record rl1 post-schedule, then rl2.
            setProperty("NVGspControl-disp-ps1-ok",
                        dispPs1Response);
            constexpr UInt32 kClientHandle110 = 0xc0d00001;
            constexpr UInt32 kObject110 = 0xc0d02080;
            constexpr UInt32 kCommand110 = 0x20801119;
            constexpr UInt32 kParamsBytes110 = 516;
            constexpr UInt32 kRunlist110 = 2;
            UInt8 control110[24 + kParamsBytes110]{};
            __builtin_memcpy(control110, &kClientHandle110, 4);
            __builtin_memcpy(control110 + 4, &kObject110, 4);
            __builtin_memcpy(control110 + 8, &kCommand110, 4);
            __builtin_memcpy(control110 + 16, &kParamsBytes110, 4);
            __builtin_memcpy(control110 + 28, &kRunlist110, 4);
            if (init_.enqueueRpc(76, control110, sizeof(control110)))
                postInitPhase_ = 110;
            else
                badReason = 104;
        } else if (dispPs2Returned) {
            // 0.29.0: record rl2 post-schedule, then device info
            // table (baseIndex 0, numEntries 32, 3212B).
            setProperty("NVGspControl-disp-ps2-ok",
                        dispPs2Response);
            constexpr UInt32 kClientHandle111 = 0xc0d00001;
            constexpr UInt32 kObject111 = 0xc0d02080;
            constexpr UInt32 kCommand111 = 0x20801112;
            constexpr UInt32 kParamsBytes111 = 3212;
            constexpr UInt32 kBase111 = 0, kNum111 = 32;
            UInt8 control111[24 + kParamsBytes111]{};
            __builtin_memcpy(control111, &kClientHandle111, 4);
            __builtin_memcpy(control111 + 4, &kObject111, 4);
            __builtin_memcpy(control111 + 8, &kCommand111, 4);
            __builtin_memcpy(control111 + 16, &kParamsBytes111, 4);
            __builtin_memcpy(control111 + 28, &kBase111, 4);
            __builtin_memcpy(control111 + 32, &kNum111, 4);
            if (init_.enqueueRpc(76, control111, sizeof(control111)))
                postInitPhase_ = 111;
            else
                badReason = 105;
        } else if (dispDevInfoReturned) {
            // 0.31.0: record device-info answer, then USERD
            // baseline survey (8x u64 at the USERD backing base).
            setProperty("NVGspControl-disp-devinfo-ok",
                        dispDevInfoResponse);
            UInt64 userdW[8]{};
            bool userdOk = false;
            if (userdBackingOffset_ != 0) {
                userdOk = true;
                for (UInt32 i = 0; i < 8 && userdOk; ++i) {
                    UInt64 word = 0;
                    PraminPteResult pr{};
                    userdOk = praminPteAccess(
                        pci_, userdBackingOffset_ + i * 8, &word,
                        false, false, &pr);
                    userdW[i] = word;
                }
            }
            dispUserdReturned = true;
            dispUserdResponse = userdOk;
            setProperty("NVGspControl-disp-userd-ok", dispUserdResponse);
            for (UInt32 i = 0; i < 8; ++i) {
                char key[64]{};
                size_t pos = 0;
                appendStr(key, sizeof(key), &pos,
                          "NVGspControl-disp-userd-w");
                appendDec(key, sizeof(key), &pos, i);
                key[pos] = '\0';
                setProperty(key, userdW[i], 64);
            }
            postInitPhase_ = 112;
        } else if (postInitPhase_ == 112) {
            // 0.31.1: local phases branch on the PERSISTENT phase
            // (poll-local Returned flags evaporate; reply-driven
            // chain would park). USERD recorded; survey GPFIFO.
            UInt64 fifoW[4]{};
            bool fifoOk = false;
            if (gpfifoBackingOffset_ != 0) {
                fifoOk = true;
                for (UInt32 i = 0; i < 4 && fifoOk; ++i) {
                    UInt64 word = 0;
                    PraminPteResult pr{};
                    fifoOk = praminPteAccess(
                        pci_, gpfifoBackingOffset_ + i * 8, &word,
                        false, false, &pr);
                    fifoW[i] = word;
                }
            }
            dispGpfifoReturned = true;
            dispGpfifoResponse = fifoOk;
            setProperty("NVGspControl-disp-gpfifo-ok", dispGpfifoResponse);
            for (UInt32 i = 0; i < 4; ++i) {
                char key[64]{};
                size_t pos = 0;
                appendStr(key, sizeof(key), &pos,
                          "NVGspControl-disp-gpfifo-w");
                appendDec(key, sizeof(key), &pos, i);
                key[pos] = '\0';
                setProperty(key, fifoW[i], 64);
            }
            postInitPhase_ = 113;
        } else if (postInitPhase_ == 113) {
            // 0.32.0: GPFIFO recorded; survey RAMFC/instance
            // (8x u64 at instanceBackingOffset_), then 114.
            UInt64 ramfcW[8]{};
            bool ramfcOk = false;
            if (instanceBackingOffset_ != 0) {
                ramfcOk = true;
                for (UInt32 i = 0; i < 8 && ramfcOk; ++i) {
                    UInt64 word = 0;
                    PraminPteResult pr{};
                    ramfcOk = praminPteAccess(
                        pci_, instanceBackingOffset_ + i * 8, &word,
                        false, false, &pr);
                    ramfcW[i] = word;
                }
            }
            dispRamfcReturned = true;
            dispRamfcResponse = ramfcOk;
            setProperty("NVGspControl-disp-ramfc-ok", dispRamfcResponse);
            for (UInt32 i = 0; i < 8; ++i) {
                char key[64]{};
                size_t pos = 0;
                appendStr(key, sizeof(key), &pos,
                          "NVGspControl-disp-ramfc-w");
                appendDec(key, sizeof(key), &pos, i);
                key[pos] = '\0';
                setProperty(key, ramfcW[i], 64);
            }
            postInitPhase_ = 114;
        } else if (postInitPhase_ == 114) {
            // 0.33.0: RAMFC recorded; DEVICE FIFO_GET_CAPS_V2 (2B)
            // on 0xc0d00080, then ENGINE_CONTEXT_PROPERTIES.
            constexpr UInt32 kClientHandle115 = 0xc0d00001;
            constexpr UInt32 kObject115 = 0xc0d00080;
            constexpr UInt32 kCommand115 = 0x801713;
            constexpr UInt32 kParamsBytes115 = 2;
            UInt8 control115[24 + kParamsBytes115]{};
            __builtin_memcpy(control115, &kClientHandle115, 4);
            __builtin_memcpy(control115 + 4, &kObject115, 4);
            __builtin_memcpy(control115 + 8, &kCommand115, 4);
            __builtin_memcpy(control115 + 16, &kParamsBytes115, 4);
            if (init_.enqueueRpc(76, control115, sizeof(control115)))
                postInitPhase_ = 115;
            else
                badReason = 106;
        } else if (dispFifoCapsReturned) {
            // 0.33.0: record FIFO caps, then ENGINE_CONTEXT
            // PROPERTIES GRAPHICS (engineId 0, 12B).
            setProperty("NVGspControl-disp-fifocaps-ok",
                        dispFifoCapsResponse);
            constexpr UInt32 kClientHandle116 = 0xc0d00001;
            constexpr UInt32 kObject116 = 0xc0d00080;
            constexpr UInt32 kCommand116 = 0x801707;
            constexpr UInt32 kParamsBytes116 = 12;
            constexpr UInt32 kEngine116 = 0;
            UInt8 control116[24 + kParamsBytes116]{};
            __builtin_memcpy(control116, &kClientHandle116, 4);
            __builtin_memcpy(control116 + 4, &kObject116, 4);
            __builtin_memcpy(control116 + 8, &kCommand116, 4);
            __builtin_memcpy(control116 + 16, &kParamsBytes116, 4);
            __builtin_memcpy(control116 + 28, &kEngine116, 4);
            if (init_.enqueueRpc(76, control116, sizeof(control116)))
                postInitPhase_ = 116;
            else
                badReason = 107;
        } else if (dispCtxPropReturned) {
            // 0.34.0: record ctx-prop answer, then TPC partition
            // GET with our live TSG handle (no guessing).
            setProperty("NVGspControl-disp-ctxprop-ok",
                        dispCtxPropResponse);
            constexpr UInt32 kClientHandle117 = 0xc0d00001;
            constexpr UInt32 kObject117 = 0xc0d00080;
            constexpr UInt32 kCommand117 = 0x801107;
            constexpr UInt32 kParamsBytes117 = 32;
            UInt8 control117[24 + kParamsBytes117]{};
            __builtin_memcpy(control117, &kClientHandle117, 4);
            __builtin_memcpy(control117 + 4, &kObject117, 4);
            __builtin_memcpy(control117 + 8, &kCommand117, 4);
            __builtin_memcpy(control117 + 16, &kParamsBytes117, 4);
            __builtin_memcpy(control117 + 24, &channelTsgHandle_, 4);
            if (init_.enqueueRpc(76, control117, sizeof(control117)))
                postInitPhase_ = 117;
            else
                badReason = 108;
        } else if (dispTpcReturned) {
            // 0.35.0: record TPC answer; validate the usermode map by
            // reading NV_VIRTUAL_FUNCTION_TIME_0/1 (BAR0 0xBB0080/84,
            // ns timer, must tick), then fresh submit token.
            setProperty("NVGspControl-disp-tpc-ok",
                        dispTpcResponse);
            IOMemoryMap *bar0Map = sharedBar0Map(pci_);
            UInt32 t0a = 0, t1a = 0, t0b = 0, t1b = 0;
            bool timeRead = false;
            if (bar0Map && bar0Map->getLength() >= 0x00BB0094) {
                Bar0Io bar0{bar0Map};
                timeRead = bar0.read(0x00BB0080, &t0a) &&
                    bar0.read(0x00BB0084, &t1a);
                IODelay(10);
                timeRead = timeRead && bar0.read(0x00BB0080, &t0b) &&
                    bar0.read(0x00BB0084, &t1b);
            }
            if (bar0Map) bar0Map->release();
            kickTimeOk_ = timeRead && t0a != t0b &&
                t0a != 0xffffffffU && (t0a >> 16) != 0xbadfU &&
                t0b != 0xffffffffU && (t0b >> 16) != 0xbadfU;
            setProperty("NVGspControl-kick-time0-a", t0a, 32);
            setProperty("NVGspControl-kick-time1-a", t1a, 32);
            setProperty("NVGspControl-kick-time0-b", t0b, 32);
            setProperty("NVGspControl-kick-time1-b", t1b, 32);
            setProperty("NVGspControl-kick-time-ok", kickTimeOk_);
            constexpr UInt32 kClientHandle118 = 0xc0d00001;
            constexpr UInt32 kChannelHandle118 = 0xc0d0c56f;
            constexpr UInt32 kCommand118 = 0xc36f0108;
            constexpr UInt32 kParamsBytes118 = 4;
            UInt8 control118[24 + kParamsBytes118]{};
            __builtin_memcpy(control118, &kClientHandle118, 4);
            __builtin_memcpy(control118 + 4, &kChannelHandle118, 4);
            __builtin_memcpy(control118 + 8, &kCommand118, 4);
            __builtin_memcpy(control118 + 16, &kParamsBytes118, 4);
            if (init_.enqueueRpc(76, control118, sizeof(control118)))
                postInitPhase_ = 118;
            else
                badReason = 109;
        } else if (dispTokenReturned) {
            // 0.35.0: NOP kick. Only with ticking usermode timer, a
            // live token, installed GPFIFO PTE and VRAM USERD/GPFIFO.
            // GPFIFO entry0 = 0 (control entry, LENGTH 0, OPCODE NOP:
            // no pushbuffer fetch), USERD GP_PUT (+0x8C) = 1, then
            // doorbell BAR0 0xBB0090 = token (UVM-exact sequence).
            setProperty("NVGspControl-kick-token-ok", dispTokenResponse);
            const char *skip = nullptr;
            if (!dispTokenResponse) skip = "token-refused";
            else if (!kickTimeOk_) skip = "usermode-time-not-ticking";
            else if (!gpfifoPteInstalled_) skip = "gpfifo-pte-absent";
            else if (!gpfifoBackingOffset_ || !userdBackingOffset_)
                skip = "no-vram-backing";
            UInt64 entry0 = 0;
            PraminPteResult entryWrite{};
            if (!skip && !praminPteAccess(pci_, gpfifoBackingOffset_,
                                          &entry0, true, false,
                                          &entryWrite))
                skip = "gpfifo-entry-write-failed";
            UInt64 getPut = 0;
            PraminPteResult userdRead{};
            if (!skip && !praminPteAccess(pci_, userdBackingOffset_ + 0x88,
                                          &getPut, false, false,
                                          &userdRead))
                skip = "userd-read-failed";
            setProperty("NVGspControl-kick-userd-before", getPut, 64);
            if (!skip) {
                getPut = (getPut & 0xffffffffULL) | (1ULL << 32);
                PraminPteResult putWrite{};
                if (!praminPteAccess(pci_, userdBackingOffset_ + 0x88,
                                     &getPut, true, false, &putWrite))
                    skip = "gp-put-write-failed";
            }
            if (!skip) {
                IOMemoryMap *bar0Map = sharedBar0Map(pci_);
                if (bar0Map && bar0Map->getLength() >= 0x00BB0094) {
                    Bar0Io bar0{bar0Map};
                    kickRung_ = bar0.write(0x00BB0090, kickToken_);
                }
                if (bar0Map) bar0Map->release();
                if (!kickRung_) skip = "doorbell-write-failed";
            }
            setProperty("NVGspControl-kick-rung", kickRung_);
            if (skip)
                setProperty("NVGspControl-kick-skipped-reason", skip);
            kickPolls_ = 0;
            postInitPhase_ = 119;
        } else if (postInitPhase_ == 119) {
            // 0.35.0: GP_GET poll (USERD +0x88 low word), max 8 polls,
            // outside the RPC watchdog range. GET==1: entry consumed.
            UInt64 getPut = 0;
            PraminPteResult userdRead{};
            const bool readOk = kickRung_ && praminPteAccess(
                pci_, userdBackingOffset_ + 0x88, &getPut, false, false,
                &userdRead);
            ++kickPolls_;
            const UInt32 gpGet = static_cast<UInt32>(getPut);
            const bool kickConsumed = readOk && gpGet == 1;
            if (kickRung_) {
                setProperty("NVGspControl-kick-userd-after", getPut, 64);
                setProperty("NVGspControl-kick-gp-get", gpGet, 32);
                setProperty("NVGspControl-kick-polls", kickPolls_, 32);
                setProperty("NVGspControl-kick-consumed", kickConsumed);
            }
            // 0.36.0: NOP consumed -> pushbuffer kick in the same run.
            // Unused tail of the 512-entry GPFIFO page (PBDMA reads only
            // GET..PUT entries): PB at +0x800, semaphore at +0xF00, both
            // reached through the installed GPFIFO PTE (VA + offset).
            // PB: INC_METHOD count 5 @SEM_ADDR_LO (0x20050017), addr lo/hi,
            // payload lo/hi, SEM_EXECUTE = RELEASE, WFI off, 32-bit,
            // no timestamp (host-only; no GR method).
            if (kickConsumed) {
                const UInt64 pbVa = channelGpFifoVa_ + 0x800;
                const UInt64 semVa = channelGpFifoVa_ + 0xF00;
                UInt64 words[6] = {
                    0x20050017ULL | ((semVa & 0xfffffffcULL) << 32),
                    ((semVa >> 32) & 0xffULL) | (0xC0FFEE35ULL << 32),
                    0x0ULL | (0x1ULL << 32),
                    0, 0, 0};
                // words[3] = semaphore clear, words[4] = GP entry1.
                words[4] = (pbVa & 0xfffffffcULL) |
                    ((((pbVa >> 32) & 0xffULL) | (6ULL << 10)) << 32);
                bool ok = (channelGpFifoVa_ & 0xfff) == 0;
                for (UInt32 i = 0; i < 3 && ok; ++i) {
                    PraminPteResult w{};
                    ok = praminPteAccess(pci_, gpfifoBackingOffset_ +
                                         0x800 + i * 8, &words[i], true,
                                         false, &w);
                }
                PraminPteResult semClear{}, entryWrite{}, putWrite{};
                ok = ok && praminPteAccess(pci_, gpfifoBackingOffset_ +
                                           0xF00, &words[3], true, false,
                                           &semClear);
                ok = ok && praminPteAccess(pci_, gpfifoBackingOffset_ + 8,
                                           &words[4], true, false,
                                           &entryWrite);
                UInt64 newGetPut = 1ULL | (2ULL << 32);
                ok = ok && praminPteAccess(pci_, userdBackingOffset_ + 0x88,
                                           &newGetPut, true, false,
                                           &putWrite);
                if (ok) {
                    IOMemoryMap *bar0Map =
                        sharedBar0Map(pci_);
                    if (bar0Map && bar0Map->getLength() >= 0x00BB0094) {
                        Bar0Io bar0{bar0Map};
                        pbRung_ = bar0.write(0x00BB0090, kickToken_);
                    }
                    if (bar0Map) bar0Map->release();
                }
                setProperty("NVGspControl-pb-gp-entry1", words[4], 64);
                setProperty("NVGspControl-pb-rung", pbRung_);
                if (!pbRung_)
                    setProperty("NVGspControl-pb-skipped-reason",
                                ok ? "doorbell-write-failed"
                                   : "pb-stage-write-failed");
                pbPolls_ = 0;
                if (pbRung_) postInitPhase_ = 120;
            }
            // Keep polling (no early return: per-poll buffers are
            // freed at the end of this function).
            if (!pbRung_ &&
                (!kickRung_ || kickConsumed || kickPolls_ >= 8)) {
                UInt64 original = originalPte_;
                PraminPteResult restore{};
                gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                    pci_, pte4KAddress_, &original, true, false, &restore);
                setProperty("NVGspControl-gpfifo-pte-restored",
                            gpfifoPteRestored_);
                constexpr UInt32 kClientHandle = 0xc0d00001;
                UInt8 freeParams[16]{};
                __builtin_memcpy(freeParams, &kClientHandle, 4);
                __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                    postInitPhase_ = 32;
                else
                    badReason = 38;
            }
        } else if (dispCtxBufReturned && dispCtxBufResponse && fbFreeBase_) {
            // 0.64.0: carve the 42 MiB GR ctx block from the client heap
            // (r2) instead of GSP's ~86 MiB heap (0.63.0: CE channel alloc
            // then failed NO_MEMORY). Local phase 217 = "ctx memory ready".
            setProperty("NVGspControl-ctxbuf-ok", dispCtxBufResponse);
            ctxBackingOffset_ = (fbFreeBase_ + 0x1fffff) & ~0x1fffffULL;
            setProperty("NVGspControl-ctxmem-offset", ctxBackingOffset_, 64);
            setProperty("NVGspControl-ctxmem-from-client-heap", true);
            postInitPhase_ = 217;
        } else if (dispCtxBufReturned && dispCtxBufResponse) {
            // 0.39.0: nouveau-r535 ctx set, one 42 MiB contiguous 2 MiB-
            // page block (2 MiB aligned), layout in kCtxLayout below.
            setProperty("NVGspControl-ctxbuf-ok", dispCtxBufResponse);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00050;
            constexpr UInt32 kMemoryClass = 0x40;  // NV01_MEMORY_LOCAL_USER
            constexpr UInt32 kMemoryParamsBytes = 128;
            UInt8 alloc[32 + kMemoryParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kMemoryHandle, 4);
            __builtin_memcpy(alloc + 12, &kMemoryClass, 4);
            __builtin_memcpy(alloc + 20, &kMemoryParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &kClientHandle, 4);
            constexpr UInt32 kDmaType = 6;
            constexpr UInt32 kFlags = 0x100;       // ALIGNMENT_FORCE
            constexpr UInt32 kAttr = 0x11800000;   // HUGE page, CONTIGUOUS
            constexpr UInt32 kAttr2 = 0x00100000;  // PAGE_SIZE_HUGE_2MB
            constexpr UInt64 kMemoryBytes = 0x2A00000;
            constexpr UInt64 kAlign = 0x200000;
            __builtin_memcpy(alloc + 36, &kDmaType, 4);
            __builtin_memcpy(alloc + 40, &kFlags, 4);
            __builtin_memcpy(alloc + 56, &kAttr, 4);
            __builtin_memcpy(alloc + 60, &kAttr2, 4);
            __builtin_memcpy(alloc + 96, &kMemoryBytes, 8);
            __builtin_memcpy(alloc + 104, &kAlign, 8);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 122;
            else
                badReason = 111;
        } else if (((ctxMemReturned && ctxMemResponse) ||
                    postInitPhase_ == 217) && fbFreeBase_) {
            // 0.62.0: 256 MiB scratch carved directly from the client heap
            // region (no RPC: GSP's own heap is only ~86 MiB).
            // 0.64.0: placed after the ctx block when that is in r2 too.
            scratchOffset_ = postInitPhase_ == 217
                ? ctxBackingOffset_ + 0x2A00000
                : (fbFreeBase_ + 0x1fffff) & ~0x1fffffULL;
            scratchTry_ = 0x10000000;
            setProperty("NVGspControl-scratch-offset", scratchOffset_, 64);
            postInitPhase_ = 211;
        } else if (ctxMemReturned && ctxMemResponse) {
            scratchAlloc();
        } else if (scratchReturned && scratchRetry_) {
            // 0.60.0: retry the scratch alloc at half the size.
            scratchRetry_ = false;
            scratchTry_ >>= 1;
            setProperty("NVGspControl-scratch-retry-size", scratchTry_, 64);
            scratchAlloc();
        } else if (scratchReturned || postInitPhase_ == 211) {
            // 0.39.0: map the block with 21 x 2 MiB PTEs in PD0 slots
            // 32..52 (VA 0x1_0400_0000, 64 MiB aligned for ATTRIBUTE_CB),
            // only if those slots are empty; then PROMOTE_CTX with the
            // nouveau r535 golden set (9 entries, init where nouveau does).
            constexpr UInt64 kCtxVa = 0x104000000ULL;
            constexpr UInt32 kFirstSlot = 32, kSlots = 21;
            UInt64 slotBefore = ~0ULL;
            PraminPteResult probe{};
            const bool probeOk = pd0Address_ &&
                channelGpFifoVa_ == 0x100000000ULL &&
                praminPteAccess(pci_, pd0Address_ + kFirstSlot * 16,
                                &slotBefore, false, false, &probe);
            setProperty("NVGspControl-pd0-address", pd0Address_, 64);
            setProperty("NVGspControl-pd0-slot32-before", slotBefore, 64);
            if (probeOk && slotBefore == 0 &&
                praminWritePteRun(pci_, pd0Address_ + kFirstSlot * 16,
                                  ctxBackingOffset_ >> 12, kSlots, 1ULL,
                                  16, 0x200))
                ctxHugeInstalled_ = kSlots;
            setProperty("NVGspControl-ctx-huge-installed",
                        ctxHugeInstalled_, 32);
            // 0.44.0: FB (GOP scanout) mapping installed HERE, before the
            // GPU walks this VAS at all: 0.43.0 installed it after GR ran
            // and hit Xid 31 PDE fault at 0x1_06A0_0000 (stale walk cache;
            // no usable TLB invalidate). Same hash gate as 0.43.0.
            if (ctxHugeInstalled_) {
                UInt64 vramHash = 0;
                const bool hashOk = praminHashRange(pci_, 0, 4096, &vramHash);
                const bool matches = hashOk && gopAtVram0_;
                setProperty("NVGspControl-fb-vram-hash", vramHash, 64);
                setProperty("NVGspControl-fb-vram-matches-gop", matches);
                UInt64 fbSlotBefore = ~0ULL;
                PraminPteResult fbProbe{};
                if (matches &&
                    praminPteAccess(pci_, pd0Address_ + 53 * 16,
                                    &fbSlotBefore, false, false, &fbProbe) &&
                    fbSlotBefore == 0 &&
                    praminWritePteRun(pci_, pd0Address_ + 53 * 16, 0, 18,
                                      1ULL, 16, 0x200))
                    fbHugeInstalled_ = 18;
                setProperty("NVGspControl-fb-huge-installed",
                            fbHugeInstalled_, 32);
                // 0.58.0: scratch 256 MiB -> PD0 slots 128..255.
                UInt64 scSlot = ~0ULL;
                PraminPteResult scProbe{};
                if (scratchOffset_ &&
                    praminPteAccess(pci_, pd0Address_ + 128 * 16, &scSlot,
                                    false, false, &scProbe) &&
                    scSlot == 0 &&
                    praminWritePteRun(pci_, pd0Address_ + 128 * 16,
                                      scratchOffset_ >> 12,
                                      static_cast<UInt32>(scratchTry_ >> 21),
                                      1ULL, 16, 0x200))
                    scratchHuge_ = static_cast<UInt32>(scratchTry_ >> 21);
                setProperty("NVGspControl-scratch-huge-installed",
                            scratchHuge_, 32);
                // 0.63.0: CE chunk (2 MiB after the scratch) at slot 71,
                // zeroed, mapped now (before any GPU walk of this range).
                if (scratchHuge_ && fbFreeBase_) {
                    const UInt64 chunk = scratchOffset_ + (UInt64(scratchHuge_) << 21);
                    UInt64 s71 = ~0ULL;
                    PraminPteResult p71{};
                    if (chunk + 0x200000 <= fbFreeLimit_ &&
                        praminZeroRange(pci_, chunk, 0x200000) &&
                        praminPteAccess(pci_, pd0Address_ + 71 * 16, &s71,
                                        false, false, &p71) && s71 == 0 &&
                        praminWritePteRun(pci_, pd0Address_ + 71 * 16,
                                          chunk >> 12, 1, 1ULL, 16, 0x200)) {
                        ceChunk_ = chunk;
                        ceMapped_ = true;
                    }
                    setProperty("NVGspControl-ce-chunk", ceChunk_, 64);
                }
                // 0.101.0 (B0): the whole client-visible VRAM [0, fb-free-limit)
                // at GPU VA kVramVa + phys with 2 MiB PTEs, installed here before
                // any GPU walk (no usable TLB invalidate: host MEM_OP from our
                // channel = RC Xid 32). RM uses only PD1[8] (0x1_0000_0000);
                // PD1 entries cover 512 MiB, so kVramVa = PD1[256..]. The PD0
                // tables (4 KiB each) live in our scratch at +0xF800000.
                // Proven from user space first (nvrun --map-vram, 26 Sep).
                if (scratchHuge_ && pdbAddress_ && fbFreeLimit_)
                    vramVaTables_ = installVramWindow();
                setProperty("NVGspControl-vram-va-tables", vramVaTables_, 32);
                // 0.102.0: shared CPU/GPU window, same timing rule.
                // 0.104.0: also after a GPU reset (new VAS): reuse the chunks.
                if (vramVaTables_)
                    shmChunkCount_ = installSharedWindow();
                setProperty("NVGspControl-shm-chunks", shmChunkCount_, 32);
            }
            UInt8 *promote = nullptr;
            constexpr UInt32 kPromoteBytes = 560;
            if (ctxHugeInstalled_)
                promote = static_cast<UInt8 *>(IOMalloc(24 + kPromoteBytes));
            if (promote) {
                bzero(promote, 24 + kPromoteBytes);
                constexpr UInt32 kClientHandle = 0xc0d00001;
                constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
                constexpr UInt32 kChannelHandle = 0xc0d0c56f;
                constexpr UInt32 kCommand = 0x2080012b;
                __builtin_memcpy(promote, &kClientHandle, 4);
                __builtin_memcpy(promote + 4, &kSubdeviceHandle, 4);
                __builtin_memcpy(promote + 8, &kCommand, 4);
                __builtin_memcpy(promote + 16, &kPromoteBytes, 4);
                UInt8 *pp = promote + 24;
                const UInt32 engineType = nvgsp::kEngineTypeGraphics;
                constexpr UInt32 kEntries = 9;
                __builtin_memcpy(pp, &engineType, 4);
                __builtin_memcpy(pp + 12, &kClientHandle, 4);
                __builtin_memcpy(pp + 16, &kChannelHandle, 4);
                __builtin_memcpy(pp + 40, &kEntries, 4);
                // {bufferId, offset, size, init, nonmapped} in nouveau
                // order; MAIN = 0x14C000 + 64 subctx header pages.
                struct Ent { UInt16 id; UInt32 off, size; UInt8 init, nm; };
                static const Ent kCtxLayout[kEntries] = {
                    {0, 0x2600000, 0x18C000, 1, 0},   // MAIN
                    {2, 0x2790000, 0x5000, 1, 0},     // PATCH
                    {3, 0x27A0000, 0x3000, 0, 0},     // BUFFER_BUNDLE_CB
                    {4, 0x27B0000, 0x20000, 0, 0},    // PAGEPOOL
                    {5, 0x0, 0x25E8800, 0, 0},        // ATTRIBUTE_CB
                    {6, 0x2800000, 0x80000, 0, 0},    // RTV_CB_GLOBAL
                    {9, 0x2880000, 0x10000, 1, 0},    // FECS_EVENT
                    {10, 0x2890000, 0x80000, 1, 1},   // PRIV_ACCESS_MAP
                    {11, 0x2910000, 0x80000, 1, 0},   // UNRESTRICTED_PAM
                };
                // PROMOTE_CTX requires client-owned initialized buffers to
                // be cleared before RM initializes them. This carved pool is
                // reused after reset; old MAIN/PATCH/FECS/PAM contents are not
                // a valid starting state for a new golden context.
                bool ctxInitializedCleared = true;
                for (UInt32 i = 0; i < kEntries; ++i) {
                    const Ent &c = kCtxLayout[i];
                    if (c.init && !praminZeroRange(pci_, ctxBackingOffset_ + c.off, c.size)) {
                        ctxInitializedCleared = false;
                        break;
                    }
                }
                setProperty("NVGspControl-ctx-initialized-cleared", ctxInitializedCleared);
                for (UInt32 i = 0; i < kEntries; ++i) {
                    UInt8 *e = pp + 48 + i * 32;
                    const Ent &c = kCtxLayout[i];
                    const UInt64 va = c.nm ? 0 : kCtxVa + c.off;
                    __builtin_memcpy(e + 8, &va, 8);
                    if (c.init) {
                        const UInt64 phys = ctxBackingOffset_ + c.off;
                        const UInt64 size = c.size;
                        const UInt32 physAttr = 0x4;  // VIDMEM, uncached
                        __builtin_memcpy(e, &phys, 8);
                        __builtin_memcpy(e + 16, &size, 8);
                        __builtin_memcpy(e + 24, &physAttr, 4);
                    }
                    __builtin_memcpy(e + 28, &c.id, 2);
                    e[30] = c.init;
                    e[31] = c.nm;
                }
                const bool sent = ctxInitializedCleared && init_.enqueueRpc(76, promote,
                                                                            24 + kPromoteBytes);
                IOFree(promote, 24 + kPromoteBytes);
                if (sent)
                    postInitPhase_ = 123;
                else
                    badReason = 112;
            } else {
                setProperty("NVGspControl-channel-skipped-reason",
                            "ctx-huge-pte-install-failed");
                ctxTeardown();
            }
        } else if (promoteReturned && promoteResponse) {
            // 0.39.0: ADA_A (0xc997) under the channel, NULL params —
            // nouveau's golden-context trigger; compute follows (126).
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kChannelHandle = 0xc0d0c56f;
            constexpr UInt32 kObjHandle = 0xc0d0c997;
            constexpr UInt32 kObjClass = 0xc997;
            UInt8 alloc[32]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kChannelHandle, 4);
            __builtin_memcpy(alloc + 8, &kObjHandle, 4);
            __builtin_memcpy(alloc + 12, &kObjClass, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 124;
            else
                badReason = 113;
        } else if (grObjReturned && grObjResponse &&
                   postInitPhase_ == 124) {
            // 0.39.0: golden init done (3D alloc returned OK); now
            // ADA_COMPUTE_A (0xc9c0) on the same channel, NULL params.
            gr3dOk_ = true;
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kChannelHandle = 0xc0d0c56f;
            constexpr UInt32 kObjHandle = 0xc0d0c9c0;
            constexpr UInt32 kObjClass = 0xc9c0;
            UInt8 alloc[32]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kChannelHandle, 4);
            __builtin_memcpy(alloc + 8, &kObjHandle, 4);
            __builtin_memcpy(alloc + 12, &kObjClass, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 126;
            else
                badReason = 114;
        } else if (grObjReturned && grObjResponse && postInitPhase_ == 126) {
            // 0.57.0: FERMI_TWOD_A (0x902d) on the channel for 2D fill/blit.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kChannelHandle = 0xc0d0c56f;
            constexpr UInt32 kObjHandle = 0xc0d0902d;
            constexpr UInt32 kObjClass = 0x902d;
            UInt8 alloc[32]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kChannelHandle, 4);
            __builtin_memcpy(alloc + 8, &kObjHandle, 4);
            __builtin_memcpy(alloc + 12, &kObjClass, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 209;
            else
                badReason = 124;
        } else if (twoDReturned) {
            setProperty("NVGspControl-grobj2d-status", twoDStatus, 32);
            // 0.41.0: PB2 at GPFIFO page +0x840 (16 dwords): SET_OBJECT
            // subch 0 = ADA_A 0xc997, subch 1 = ADA_COMPUTE_A 0xc9c0
            // (standard 3D/compute subchannels; 0.40.0 bound compute on
            // subch 0 -> GR exception Xid 13 trapped at subch 0 mthd 0),
            // SEM A 0xC0FFEE36 -> +0xF08 no WFI, SEM B 0xC0FFEE37 ->
            // +0xF10 WITH WFI (proves GR idle after both binds).
            const UInt64 pbVa = channelGpFifoVa_ + 0x840;
            const UInt64 semA = channelGpFifoVa_ + 0xF08;
            const UInt64 semB = channelGpFifoVa_ + 0xF10;
            UInt64 words[11] = {
                0x20010000ULL | (0xc997ULL << 32),
                0x20012000ULL | (0xc9c0ULL << 32),
                0x20050017ULL | ((semA & 0xfffffffcULL) << 32),
                ((semA >> 32) & 0xffULL) | (0xC0FFEE36ULL << 32),
                0x0ULL | (0x00000001ULL << 32),
                0x20050017ULL | ((semB & 0xfffffffcULL) << 32),
                ((semB >> 32) & 0xffULL) | (0xC0FFEE37ULL << 32),
                0x0ULL | (0x00100001ULL << 32),
                0, 0, 0};
            words[10] = (pbVa & 0xfffffffcULL) |
                ((((pbVa >> 32) & 0xffULL) | (16ULL << 10)) << 32);
            bool ok = true;
            for (UInt32 i = 0; i < 8 && ok; ++i) {
                PraminPteResult w{};
                ok = praminPteAccess(pci_, gpfifoBackingOffset_ + 0x840 +
                                     i * 8, &words[i], true, false, &w);
            }
            PraminPteResult semClear{}, semClearB{}, entryWrite{}, putWrite{};
            ok = ok && praminPteAccess(pci_, gpfifoBackingOffset_ + 0xF08,
                                       &words[8], true, false, &semClear);
            ok = ok && praminPteAccess(pci_, gpfifoBackingOffset_ + 0xF10,
                                       &words[9], true, false, &semClearB);
            ok = ok && praminPteAccess(pci_, gpfifoBackingOffset_ + 0x10,
                                       &words[10], true, false, &entryWrite);
            UInt64 newGetPut = 2ULL | (3ULL << 32);
            ok = ok && praminPteAccess(pci_, userdBackingOffset_ + 0x88,
                                       &newGetPut, true, false, &putWrite);
            if (ok) {
                IOMemoryMap *bar0Map = sharedBar0Map(pci_);
                if (bar0Map && bar0Map->getLength() >= 0x00BB0094) {
                    Bar0Io bar0{bar0Map};
                    grRung_ = bar0.write(0x00BB0090, kickToken_);
                }
                if (bar0Map) bar0Map->release();
            }
            setProperty("NVGspControl-gr-rung", grRung_);
            grPolls_ = 0;
            if (grRung_)
                postInitPhase_ = 125;
            else
                ctxTeardown();
        } else if (postInitPhase_ == 125) {
            // 0.40.0: poll SEM A (+0xF08, no WFI) and SEM B (+0xF10, WFI)
            // plus the read-only GR/MMU diag dump, <=8 polls.
            UInt64 sem = 0, semBv = 0, getPut = 0;
            PraminPteResult semRead{}, semReadB{}, userdRead{};
            const bool semOk = praminPteAccess(
                pci_, gpfifoBackingOffset_ + 0xF08, &sem, false, false,
                &semRead);
            praminPteAccess(pci_, gpfifoBackingOffset_ + 0xF10, &semBv,
                            false, false, &semReadB);
            praminPteAccess(pci_, userdBackingOffset_ + 0x88, &getPut,
                            false, false, &userdRead);
            ++grPolls_;
            const bool releasedA = semOk &&
                static_cast<UInt32>(sem) == 0xC0FFEE36U;
            const bool released =
                static_cast<UInt32>(semBv) == 0xC0FFEE37U;
            UInt32 diag[kGrDiagCount]{};
            setProperty("NVGspControl-gr-diag-ok", readGrDiag(pci_, diag));
            setProperty("NVGspControl-gr-diag", diag, sizeof(diag));
            setProperty("NVGspControl-gr-semb", semBv, 64);
            setProperty("NVGspControl-gr-sema-released", releasedA);
            setProperty("NVGspControl-gr-sem", sem, 64);
            setProperty("NVGspControl-gr-userd-after", getPut, 64);
            setProperty("NVGspControl-gr-polls", grPolls_, 32);
            setProperty("NVGspControl-gr-sem-released", released);
            // 0.42.0: GR idle after both binds -> PB3 at +0x8C0 (18 dw):
            // compute subch 1 inline-to-memory of 16 bytes to +0xF20
            // (LINE_LENGTH_IN/LINE_COUNT/OFFSET_OUT_UPPER/OFFSET_OUT,
            // LAUNCH_DMA pitch, 4x LOAD_INLINE_DATA non-inc), then SEM C
            // WFI 0xC0FFEE38 -> +0xF18. GP entry3, PUT 4, doorbell.
            bool i2mRung = false;
            if (released) {
                const UInt64 pbVa = channelGpFifoVa_ + 0x8C0;
                const UInt64 dst = channelGpFifoVa_ + 0xF20;
                const UInt64 semC = channelGpFifoVa_ + 0xF18;
                UInt64 w[13] = {
                    0x20042060ULL | (16ULL << 32),
                    1ULL | (((dst >> 32) & 0xffULL) << 32),
                    (dst & 0xffffffffULL) | (0x2001206CULL << 32),
                    1ULL | (0x6004206DULL << 32),
                    0x0A0DF00DULL | (0x4E564F4BULL << 32),
                    0x4D41434FULL | (0x00000041ULL << 32),
                    0x20050017ULL | ((semC & 0xfffffffcULL) << 32),
                    ((semC >> 32) & 0xffULL) | (0xC0FFEE38ULL << 32),
                    0x0ULL | (0x00100001ULL << 32),
                    0, 0, 0, 0};
                w[12] = (pbVa & 0xfffffffcULL) |
                    ((((pbVa >> 32) & 0xffULL) | (18ULL << 10)) << 32);
                bool ok = true;
                for (UInt32 i = 0; i < 9 && ok; ++i) {
                    PraminPteResult r{};
                    ok = praminPteAccess(pci_, gpfifoBackingOffset_ + 0x8C0 +
                                         i * 8, &w[i], true, false, &r);
                }
                // clear SEM C (+0xF18) and the 16-byte target (+0xF20).
                for (UInt32 i = 0; i < 3 && ok; ++i) {
                    PraminPteResult r{};
                    ok = praminPteAccess(pci_, gpfifoBackingOffset_ + 0xF18 +
                                         i * 8, &w[9 + i], true, false, &r);
                }
                PraminPteResult entryWrite{}, putWrite{};
                ok = ok && praminPteAccess(pci_, gpfifoBackingOffset_ + 0x18,
                                           &w[12], true, false, &entryWrite);
                UInt64 newGetPut = 3ULL | (4ULL << 32);
                ok = ok && praminPteAccess(pci_, userdBackingOffset_ + 0x88,
                                           &newGetPut, true, false, &putWrite);
                if (ok) {
                    IOMemoryMap *bar0Map = sharedBar0Map(pci_);
                    if (bar0Map && bar0Map->getLength() >= 0x00BB0094) {
                        Bar0Io bar0{bar0Map};
                        i2mRung = bar0.write(0x00BB0090, kickToken_);
                    }
                    if (bar0Map) bar0Map->release();
                }
                setProperty("NVGspControl-i2m-rung", i2mRung);
            }
            if (i2mRung) {
                grPolls_ = 0;
                postInitPhase_ = 127;
            } else if (released || grPolls_ >= 8) {
                ctxTeardown();
            }
        } else if (postInitPhase_ == 127) {
            // 0.42.0: poll SEM C (+0xF18) + the I2M target (+0xF20/+0xF28).
            UInt64 semC = 0, d0 = 0, d1 = 0;
            PraminPteResult r0{}, r1{}, r2{};
            praminPteAccess(pci_, gpfifoBackingOffset_ + 0xF18, &semC, false,
                            false, &r0);
            praminPteAccess(pci_, gpfifoBackingOffset_ + 0xF20, &d0, false,
                            false, &r1);
            praminPteAccess(pci_, gpfifoBackingOffset_ + 0xF28, &d1, false,
                            false, &r2);
            ++grPolls_;
            const bool semDone = static_cast<UInt32>(semC) == 0xC0FFEE38U;
            const bool dataOk = d0 == 0x4E564F4B0A0DF00DULL &&
                d1 == 0x000000414D41434FULL;
            setProperty("NVGspControl-i2m-sem", semC, 64);
            setProperty("NVGspControl-i2m-data0", d0, 64);
            setProperty("NVGspControl-i2m-data1", d1, 64);
            setProperty("NVGspControl-i2m-polls", grPolls_, 32);
            setProperty("NVGspControl-i2m-sem-released", semDone);
            setProperty("NVGspControl-i2m-data-ok", dataOk);
            // 0.43.0: GPU draws on the GOP scanout surface. Gate: PRAMIN
            // hash of VRAM[0,4K) must equal the pre-GSP BAR1+0 GOP hash
            // (proves VRAM 0 is the intact scanout surface). Map VRAM
            // [0,36 MiB) with 18 x 2 MiB PTEs (PD0 slots 53..70, VA
            // 0x1_06A0_0000), PB4 in the spare ctx block tail (+0x2990000):
            // compute subch 1 I2M 256x8 px, pitch 16384 (4096 px ARGB),
            // red 0xFFFF0000, then SEM D WFI 0xC0FFEE39 -> +0xF30.
            bool fbRung = false;
            if (semDone && dataOk && fbHugeInstalled_ && drawTest_) {
                constexpr UInt64 kFbVa = 0x106A00000ULL;
                const bool mapped = true;
                const UInt64 pbPhys = ctxBackingOffset_ + 0x2990000;
                const UInt64 pbVa = 0x104000000ULL + 0x2990000;
                const UInt64 semD = channelGpFifoVa_ + 0xF30;
                const UInt32 head[9] = {
                    0x20052060, 1024, 8,
                    static_cast<UInt32>(kFbVa >> 32),
                    static_cast<UInt32>(kFbVa), 16384,
                    0x2001206C, 1, 0x6800206D};
                const UInt32 tail[6] = {
                    0x20050017, static_cast<UInt32>(semD & 0xfffffffcULL),
                    static_cast<UInt32>((semD >> 32) & 0xff), 0xC0FFEE39, 0,
                    0x00100001};
                bool ok = mapped && praminWritePattern(
                    pci_, pbPhys, head, 9, 0xFFFF0000U, 2048, tail, 6);
                UInt64 zero = 0;
                PraminPteResult semClear{}, entryWrite{}, putWrite{};
                ok = ok && praminPteAccess(pci_, gpfifoBackingOffset_ + 0xF30,
                                           &zero, true, false, &semClear);
                UInt64 entry4 = (pbVa & 0xfffffffcULL) |
                    ((((pbVa >> 32) & 0xffULL) | (2063ULL << 10)) << 32);
                ok = ok && praminPteAccess(pci_, gpfifoBackingOffset_ + 0x20,
                                           &entry4, true, false, &entryWrite);
                UInt64 newGetPut = 4ULL | (5ULL << 32);
                ok = ok && praminPteAccess(pci_, userdBackingOffset_ + 0x88,
                                           &newGetPut, true, false, &putWrite);
                if (ok) {
                    IOMemoryMap *bar0Map = sharedBar0Map(pci_);
                    if (bar0Map && bar0Map->getLength() >= 0x00BB0094) {
                        Bar0Io bar0{bar0Map};
                        fbRung = bar0.write(0x00BB0090, kickToken_);
                    }
                    if (bar0Map) bar0Map->release();
                }
                setProperty("NVGspControl-fb-rung", fbRung);
            }
            if (fbRung) {
                grPolls_ = 0;
                postInitPhase_ = 128;
            } else if ((semDone && dataOk) || grPolls_ >= 8) {
                ctxTeardown();
            }
        } else if (postInitPhase_ == 128) {
            // 0.43.0: poll SEM D + read back the first two FB pixels.
            UInt64 semD = 0, px = 0;
            PraminPteResult r0{}, r1{};
            praminPteAccess(pci_, gpfifoBackingOffset_ + 0xF30, &semD, false,
                            false, &r0);
            praminPteAccess(pci_, 0, &px, false, false, &r1);
            ++grPolls_;
            const bool done = static_cast<UInt32>(semD) == 0xC0FFEE39U;
            setProperty("NVGspControl-fb-sem", semD, 64);
            setProperty("NVGspControl-fb-pixels01", px, 64);
            setProperty("NVGspControl-fb-polls", grPolls_, 32);
            setProperty("NVGspControl-fb-sem-released", done);
            setProperty("NVGspControl-fb-drawn",
                        done && px == 0xFFFF0000FFFF0000ULL);
            if (done || grPolls_ >= 8) {
                ctxTeardown();
            }
        } else if (dispCtxBufReturned || ctxMemReturned ||
                   promoteReturned || grObjReturned) {
            // 0.38.0: any refusal in 121-124 -> recorded; teardown.
            setProperty("NVGspControl-ctxbuf-ok", dispCtxBufResponse);
            ctxTeardown();
        } else if (postInitPhase_ == 120) {
            // 0.36.0: semaphore poll (+0xF00 == 0xC0FFEE35) + GP_GET
            // (== 2), max 8 polls, outside the RPC watchdog range.
            UInt64 sem = 0, getPut = 0;
            PraminPteResult semRead{}, userdRead{};
            const bool semOk = praminPteAccess(
                pci_, gpfifoBackingOffset_ + 0xF00, &sem, false, false,
                &semRead);
            const bool getOk = praminPteAccess(
                pci_, userdBackingOffset_ + 0x88, &getPut, false, false,
                &userdRead);
            ++pbPolls_;
            const bool released = semOk &&
                static_cast<UInt32>(sem) == 0xC0FFEE35U;
            const UInt32 gpGet = static_cast<UInt32>(getPut);
            setProperty("NVGspControl-pb-sem", sem, 64);
            setProperty("NVGspControl-pb-userd-after", getPut, 64);
            setProperty("NVGspControl-pb-gp-get", gpGet, 32);
            setProperty("NVGspControl-pb-polls", pbPolls_, 32);
            setProperty("NVGspControl-pb-sem-released", released);
            setProperty("NVGspControl-pb-consumed", getOk && gpGet == 2);
            UInt8 *ctxbufControl = nullptr;
            constexpr UInt32 kCtxbufBytes = 1664;
            if (released && internalClient_ && internalSubdevice_)
                ctxbufControl = static_cast<UInt8 *>(
                    IOMalloc(24 + kCtxbufBytes));
            if (ctxbufControl) {
                bzero(ctxbufControl, 24 + kCtxbufBytes);
                constexpr UInt32 kCommand121 = 0x20800a32;
                __builtin_memcpy(ctxbufControl, &internalClient_, 4);
                __builtin_memcpy(ctxbufControl + 4, &internalSubdevice_, 4);
                __builtin_memcpy(ctxbufControl + 8, &kCommand121, 4);
                __builtin_memcpy(ctxbufControl + 16, &kCtxbufBytes, 4);
                const bool sent = init_.enqueueRpc(76, ctxbufControl,
                                                   24 + kCtxbufBytes);
                IOFree(ctxbufControl, 24 + kCtxbufBytes);
                if (sent)
                    postInitPhase_ = 121;
                else
                    badReason = 110;
            } else if (released || pbPolls_ >= 8) {
                UInt64 original = originalPte_;
                PraminPteResult restore{};
                gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                    pci_, pte4KAddress_, &original, true, false, &restore);
                setProperty("NVGspControl-gpfifo-pte-restored",
                            gpfifoPteRestored_);
                constexpr UInt32 kClientHandle = 0xc0d00001;
                UInt8 freeParams[16]{};
                __builtin_memcpy(freeParams, &kClientHandle, 4);
                __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                    postInitPhase_ = 32;
                else
                    badReason = 38;
            }
        } else if (dispPbProgramReturned && dispPbProgramResponse &&
                   (experimentFlags_ & 2)) {
            // 0.51.0: C77D core channel on physical RM, nouveau r535
            // r535_dmac_alloc params: channelInstance 0, offset (PUT) 0,
            // no ctxdma. Parent = NVC770_DISPLAY 0xc0d0c770.
            setProperty("NVGspControl-disp-pb-program-ok", true);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDisplayHandle = 0xc0d0c770;
            constexpr UInt32 kCoreHandle = 0xc0d0c77d;
            constexpr UInt32 kCoreClass = nvgsp::kDispCoreChannelDma;
            constexpr UInt32 kParamsBytes = 40;
            UInt8 alloc[32 + kParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDisplayHandle, 4);
            __builtin_memcpy(alloc + 8, &kCoreHandle, 4);
            __builtin_memcpy(alloc + 12, &kCoreClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 202;
            else
                badReason = 122;
        } else if (coreChanReturned) {
            // 0.51.0: if the core channel exists, stage UPDATE (0x00040200,
            // data 0) at PB offset 0, PUT=8 at 0x680000, poll GET (100 ms).
            bool coreConsumed = false;
            UInt32 putAfter = 0, getAfter = 0, putBefore = 0, getBefore = 0;
            if (coreChanResponse && dispPbBackingOffset_) {
                // 0.82.0 (experiment bit4): the first core UPDATE is
                // nouveau's corec37d_init + corec37d_update instead of a bare
                // UPDATE(0): WINDOW_SET_CONTROL owner = head(i>>1), format
                // usage bounds 0x1f, usage bounds 0x7fff px | TAPS_2, then
                // SET_INTERLOCK_FLAGS 0, SET_WINDOW_INTERLOCK_FLAGS 0 and
                // UPDATE 1 (RELEASE_ELV). A bare UPDATE(0) left the core BUSY
                // from boot, so no later core state ever armed.
                UInt32 init[2 + 8 * 7 + 3 + 2 + 6]{};
                UInt32 n = 0;
                // 0.83.0 (bit5): nouveau's notifier too — ctxdma at RAMIN+0x2040
                // over a 4 KiB notifier at RAMIN+0xF000, RAMHT entry for core
                // (chid 0), SET_CONTEXT_DMA_NOTIFIER, and SET_NOTIFIER_CONTROL
                // (WRITE|NOTIFY) around the UPDATE.
                const bool ntfy = (experimentFlags_ & 0x30) == 0x30;
                if (ntfy) {
                    const UInt64 nb = dispInstOffset_ + 0xF000;
                    const UInt32 dma[4] = {0x5, static_cast<UInt32>(nb >> 8), 0,
                                           static_cast<UInt32>((nb + 0xfff) >> 8)};
                    constexpr UInt32 kNtfyHandle = 0xc0d0d003;
                    UInt32 h = kNtfyHandle, hash = 0;
                    while (h) { hash ^= h & 0x3ff; h >>= 10; }
                    const UInt32 ent[2] = {kNtfyHandle,
                        (0U << 25) | (0xc0d00001 & 0x3fff) | (0x2040U << 9)};
                    const UInt32 zero[4] = {0, 0, 0, 0};
                    const bool w = praminWriteWords(pci_, dispInstOffset_ + 0x2040, dma, 4) &&
                        praminWriteWords(pci_, dispInstOffset_ + hash * 8, ent, 2) &&
                        praminWriteWords(pci_, nb, zero, 4);
                    setProperty("NVGspControl-corechan-ntfy-setup", w);
                    init[n++] = (1U << 18) | 0x208; init[n++] = kNtfyHandle;
                }
                // 0.85.0: minimal first push by default for bit4 — no window
                // owner/usage-bound changes (0.84.0's nouveau bounds 0x107fff /
                // 0x1f replaced the VBIOS 0x110f00 / 0x197 and window 0 stopped
                // fetching: no LAST_DATA, black screen). bit6 restores the full
                // nouveau window init.
                if ((experimentFlags_ & 0x10) && !(experimentFlags_ & 0x40)) {
                    init[n++] = (2U << 18) | 0x218; init[n++] = 0; init[n++] = 0;
                    init[n++] = (1U << 18) | 0x200; init[n++] = 1;
                } else if (experimentFlags_ & 0x10) {
                    for (UInt32 i = 0; i < 8; ++i) {
                        const UInt32 base = 0x1000 + i * 0x80;
                        init[n++] = (1U << 18) | base;          init[n++] = i >> 1;
                        init[n++] = (2U << 18) | (base + 4);    init[n++] = 0x1f;
                        init[n++] = 0;
                        init[n++] = (1U << 18) | (base + 0x10); init[n++] = 0x107fff;
                    }
                    if (ntfy) { init[n++] = (1U << 18) | 0x20C; init[n++] = 1U << 12; }
                    init[n++] = (2U << 18) | 0x218; init[n++] = 0; init[n++] = 0;
                    init[n++] = (1U << 18) | 0x200; init[n++] = 1;
                    if (ntfy) { init[n++] = (1U << 18) | 0x20C; init[n++] = 0; }
                } else {
                    init[n++] = 0x00040200; init[n++] = 0;
                }
                coreInitWords_ = n;
                const bool pbOk = praminWriteWords(pci_, dispPbBackingOffset_,
                                                   init, n);
                IOMemoryMap *bar0Map = sharedBar0Map(pci_);
                if (pbOk && bar0Map && bar0Map->getLength() >= 0x00680008) {
                    Bar0Io bar0{bar0Map};
                    bar0.read(0x00680000, &putBefore);
                    bar0.read(0x00680004, &getBefore);
                    // 0.84.0: let the freshly allocated core settle first.
                    if (experimentFlags_ & 0x10) IODelay(50000);
                    bar0.write(0x00680000, coreInitWords_ * 4);
                    bar0.read(0x00680000, &putAfter);
                    for (UInt32 i = 0; i < 2000; ++i) {
                        if (!bar0.read(0x00680004, &getAfter)) break;
                        if (getAfter == coreInitWords_ * 4) { coreConsumed = true; break; }
                        IODelay(50);
                    }
                    // 0.84.0: wait for the init UPDATE to arm (ARMED
                    // SET_INTERLOCK_FLAGS/WINDOW_INTERLOCK_FLAGS 0x688218/1c
                    // drop from the VBIOS 0x10000/1 to our 0/0) before RM
                    // is asked to allocate the window (0.83.0: window alloc
                    // NV_ERR_TIMEOUT when it had not armed). One re-push of
                    // interlocks + UPDATE(1) if it has not armed after 1 s.
                    if ((experimentFlags_ & 0x10) && coreConsumed) {
                        UInt32 a0 = ~0U, a1 = ~0U, polls = 0, repush = 0;
                        for (; polls < 4000; ++polls) {
                            bar0.read(0x00688218, &a0);
                            bar0.read(0x0068821c, &a1);
                            if (a0 == 0 && a1 == 0) break;
                            if (polls == 2000 && !repush) {
                                const UInt32 again[5] = {(2U << 18) | 0x218, 0, 0,
                                                         (1U << 18) | 0x200, 1};
                                if (praminWriteWords(pci_, dispPbBackingOffset_ +
                                                     coreInitWords_ * 4, again, 5)) {
                                    coreInitWords_ += 5;
                                    bar0.write(0x00680000, coreInitWords_ * 4);
                                    repush = 1;
                                }
                            }
                            IODelay(500);
                        }
                        setProperty("NVGspControl-corechan-arm-polls", polls, 32);
                        setProperty("NVGspControl-corechan-armed", a0 == 0 && a1 == 0);
                        setProperty("NVGspControl-corechan-repush", repush, 32);
                    }
                }
                if (bar0Map) bar0Map->release();
            }
            setProperty("NVGspControl-corechan-put-before", putBefore, 32);
            setProperty("NVGspControl-corechan-get-before", getBefore, 32);
            setProperty("NVGspControl-corechan-put-after", putAfter, 32);
            setProperty("NVGspControl-corechan-get-after", getAfter, 32);
            setProperty("NVGspControl-corechan-consumed", coreConsumed);
            setProperty("NVGspControl-corechan-init-words", coreInitWords_, 32);
            if ((experimentFlags_ & 0x30) == 0x30) {
                UInt32 nw[4] = {};
                for (UInt32 i = 0; i < 400; ++i) {
                    UInt64 v = 0;
                    PraminPteResult r{};
                    praminPteAccess(pci_, dispInstOffset_ + 0xF000, &v, false, false, &r);
                    nw[0] = static_cast<UInt32>(v);
                    nw[1] = static_cast<UInt32>(v >> 32);
                    if (nw[0]) break;
                    IODelay(250);
                }
                setProperty("NVGspControl-corechan-ntfy", nw, sizeof(nw));
            }
            corePut_ = coreConsumed ? coreInitWords_ * 4 : 0;
            // 0.54.0: window flip path (bit2): RAMHT entry + VRAM ctxdma in
            // RAMIN (nouveau gv100_dmaobj_bind / nvkm_ramht_insert: 0x2000
            // byte RAMHT at RAMIN+0, bits 10, context = chid<<25 |
            // client&0x3fff | inst<<9), window PB at RAMIN+0x8000, then
            // set_pushbuf C67E instance 0.
            bool windowStarted = false;
            if ((experimentFlags_ & 4) && coreConsumed) {
                constexpr UInt32 kIsoHandle = 0xc0d0d001;
                constexpr UInt32 kWndUser = 1;  // r535_wndw .user
                UInt32 h = kIsoHandle, hash = 0;
                while (h) { hash ^= h & 0x3ff; h >>= 10; }
                hash ^= kWndUser << 6;
                UInt64 ramht = UInt64(kIsoHandle) |
                    (UInt64((kWndUser << 25) | (0xc0d00001 & 0x3fff) |
                            (0x2000 << 9)) << 32);
                UInt64 dma0 = 0x5;  // VRAM | RW, kind 0 (pitch), start 0
                UInt64 dma1 = UInt64(0x03FFFFFFU) << 32;  // limit 16 GiB-1 >>8
                UInt64 dma2 = 0;
                PraminPteResult r{};
                bool ok = praminPteAccess(pci_, dispInstOffset_ + 0x2000,
                                          &dma0, true, false, &r) &&
                    praminPteAccess(pci_, dispInstOffset_ + 0x2008, &dma1,
                                    true, false, &r) &&
                    praminPteAccess(pci_, dispInstOffset_ + 0x2010, &dma2,
                                    true, false, &r) &&
                    praminPteAccess(pci_, dispInstOffset_ + hash * 8,
                                    &ramht, true, false, &r);
                setProperty("NVGspControl-wnd-ramht-slot", hash, 32);
                setProperty("NVGspControl-wnd-ramht-ok", ok);
                nvgsp::NvDispChannelPushbufferParams wp{};
                wp.addressSpace = nvgsp::kAddressSpaceFbmem;
                wp.physicalAddr = dispInstOffset_ + 0x8000;
                wp.limit = 4095;
                wp.hclass = 0xc67e;
                wp.channelInstance = 0;
                wp.valid = 1;
                wp.pbTargetAperture = nvgsp::kPbTargetPhysNvm;
                wp.channelPBSize = nvgsp::kChannelPbSize4K;
                constexpr UInt32 kWpBytes = sizeof(wp);
                UInt8 control[24 + kWpBytes]{};
                const UInt32 cmd = nvgsp::kDispSetChannelPushbuffer;
                __builtin_memcpy(control, &internalClient_, 4);
                __builtin_memcpy(control + 4, &internalSubdevice_, 4);
                __builtin_memcpy(control + 8, &cmd, 4);
                __builtin_memcpy(control + 16, &kWpBytes, 4);
                __builtin_memcpy(control + 24, &wp, kWpBytes);
                if (ok && init_.enqueueRpc(76, control, sizeof(control))) {
                    postInitPhase_ = 207;
                    windowStarted = true;
                }
            }
            if (!windowStarted)
                dispHealthStart();
        } else if (wndPbReturned && wndPbOk) {
            // 0.54.0: C67E window 0 channel, parent C770, nouveau params.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDisplayHandle = 0xc0d0c770;
            constexpr UInt32 kWndHandle = 0xc0d0c67e;
            constexpr UInt32 kWndClass = 0xc67e;
            constexpr UInt32 kParamsBytes = 40;
            UInt8 alloc[32 + kParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDisplayHandle, 4);
            __builtin_memcpy(alloc + 8, &kWndHandle, 4);
            __builtin_memcpy(alloc + 12, &kWndClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 208;
            else
                dispHealthStart();
        } else if (wndChanReturned && wndChanOk) {
            // 0.55.0: flip window 0 onto the GOP surface itself (offset 0),
            // where WindowServer draws through physical-mode BAR1, so the
            // desktop keeps updating through OUR window channel.
            // (0.54.0 used +200 lines.) A8R8G8B8 3840x2160,
            // pitch 16384 (PLANAR_STORAGE pitch>>6), ctxdma 0xc0d0d001,
            // no interlock, UPDATE(1). Method header (count<<18)|mthd.
            const UInt32 wh = 3840U | (2160U << 16);
            const UInt32 pb[] = {
                (1U << 18) | 0x308, 0x0,              // PRESENT_CONTROL
                (4U << 18) | 0x224, wh, 0x0, 0xCF, 16384U >> 6,
                (1U << 18) | 0x240, 0xc0d0d001,       // CONTEXT_DMA_ISO(0)
                (1U << 18) | 0x260, 0x0,              // OFFSET(0) = VRAM 0
                (1U << 18) | 0x290, 0x0,              // POINT_IN(0)
                (1U << 18) | 0x298, wh,               // SIZE_IN
                (1U << 18) | 0x2A4, wh,               // SIZE_OUT
                (2U << 18) | 0x370, 0x0, 0x0,         // INTERLOCK flags
                (1U << 18) | 0x200, 0x1};             // UPDATE
            constexpr UInt32 kWords = sizeof(pb) / 4;
            const bool pbOk = praminWritePattern(pci_,
                dispInstOffset_ + 0x8000, pb, kWords, 0, 0, nullptr, 0);
            UInt32 put = 0, get = 0;
            bool consumedW = false;
            IOMemoryMap *bar0Map = sharedBar0Map(pci_);
            if (pbOk && bar0Map && bar0Map->getLength() >= 0x00690008) {
                Bar0Io bar0{bar0Map};
                bar0.write(0x00690000, kWords * 4);
                bar0.read(0x00690000, &put);
                for (UInt32 i = 0; i < 4000; ++i) {
                    if (!bar0.read(0x00690004, &get)) break;
                    if (get == kWords * 4) { consumedW = true; break; }
                    IODelay(50);
                }
            }
            if (bar0Map) bar0Map->release();
            setProperty("NVGspControl-wnd-pb-write-ok", pbOk);
            setProperty("NVGspControl-wnd-put", put, 32);
            setProperty("NVGspControl-wnd-get", get, 32);
            setProperty("NVGspControl-wnd-consumed", consumedW);
            wndOwnsScreen_ = consumedW;
            markBoot(consumedW ? "wnd-owns" : "wnd-fail");
            wndPut_ = consumedW ? kWords * 4 : 0;
            dispHealthStart();
        } else if (wndPbReturned || wndChanReturned) {
            dispHealthStart();
        } else if (ceUserdReturned && ceBackingOk && postInitPhase_ < 220) {
            ceBackingAlloc(postInitPhase_ + 1);
        } else if (ceUserdReturned) {
            // 0.65.0: CE channel alloc (engine COPY0) with the RPC USERD.
            // 0.66.0: instance + method buffer are RPC memory too.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kCeChannel = 0xc0d1c56f;
            constexpr UInt32 kChannelClass = nvgsp::kAmpereChannelGpfifoA;
            nvgsp::NvChannelAllocParams chan{};
            const bool built = ceUserdOffset_ && ceInstOffset_ &&
                ceMthdOffset_ &&
                praminZeroRange(pci_, ceUserdOffset_, 4096) &&
                praminZeroRange(pci_, ceInstOffset_, 4096) &&
                nvgsp::buildChannelAllocParams(0xc0d090f1, 0xc0d10043,
                    0x108E00000ULL, 512, nvgsp::kEngineTypeCopy0, &chan);
            chan.hObjectError = 0;
            // 0.67.0: CPU-RM (us) owns chid assignment and passes it in the
            // USERD index/page flags (kernel_channel.c GSP-client path);
            // chid 3 is the GR channel, so the CE channel takes chid 4:
            // PAGE_FIXED | INDEX_VALUE 4 | PAGE_VALUE 0.
            chan.flags = 0x00200400;
            chan.internalFlags = nvgsp::kChannelInternalFlagsNotifierNone;
            chan.instanceMem = {ceInstOffset_, 4096, 2, 1};
            chan.ramfcMem = {ceInstOffset_, 512, 2, 1};
            chan.userdMem = {ceUserdOffset_, 512, 2, 1};
            chan.mthdbufMem = {ceMthdOffset_,
                               methodBufferBytes_ ? methodBufferBytes_ : 20480,
                               2, 0};
            constexpr UInt32 kParamsBytes = sizeof(chan);
            UInt8 alloc[32 + kParamsBytes]{};
            __builtin_memcpy(alloc, &kClientHandle, 4);
            __builtin_memcpy(alloc + 4, &kDeviceHandle, 4);
            __builtin_memcpy(alloc + 8, &kCeChannel, 4);
            __builtin_memcpy(alloc + 12, &kChannelClass, 4);
            __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
            __builtin_memcpy(alloc + 32, &chan, kParamsBytes);
            if (built && init_.enqueueRpc(103, alloc, sizeof(alloc)))
                postInitPhase_ = 212;
            else
                ctxTeardown();
        } else if (ceReturned && ceOk && postInitPhase_ >= 212 &&
                   postInitPhase_ <= 215) {
            // 0.63.0: CE chain step: bind COPY0 (213), schedule (214),
            // token (215), then the CE object 0xc7b5 (216).
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kCeChannel = 0xc0d1c56f;
            const UInt32 next = postInitPhase_ + 1;
            if (postInitPhase_ == 215) ceToken_ = ceTokenReply;
            if (next <= 215) {
                const UInt32 cmd = next == 213 ? 0xa06f0104
                    : next == 214 ? 0xa06f0103 : 0xc36f0108;
                const UInt32 bytes = next == 214 ? 2 : 4;
                UInt8 control[24 + 4]{};
                __builtin_memcpy(control, &kClientHandle, 4);
                __builtin_memcpy(control + 4, &kCeChannel, 4);
                __builtin_memcpy(control + 8, &cmd, 4);
                __builtin_memcpy(control + 16, &bytes, 4);
                if (next == 213) {
                    const UInt32 eng = nvgsp::kEngineTypeCopy0;
                    __builtin_memcpy(control + 24, &eng, 4);
                } else if (next == 214) {
                    control[24] = 1;  // bEnable
                }
                if (init_.enqueueRpc(76, control, 24 + bytes))
                    postInitPhase_ = next;
                else
                    ctxTeardown();
            } else {
                constexpr UInt32 kCeObj = 0xc0d1c7b5, kCeClass = 0xc7b5;
                UInt8 alloc[32]{};
                __builtin_memcpy(alloc, &kClientHandle, 4);
                __builtin_memcpy(alloc + 4, &kCeChannel, 4);
                __builtin_memcpy(alloc + 8, &kCeObj, 4);
                __builtin_memcpy(alloc + 12, &kCeClass, 4);
                if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                    postInitPhase_ = 216;
                else
                    ctxTeardown();
            }
        } else if (ceReturned) {
            // 0.63.0: CE object done (216) or a CE step refused -> park.
            cePersistent_ = ceOk && postInitPhase_ == 216 && ceToken_;
            setProperty("NVGspControl-ce-persistent", cePersistent_);
            syncRingSeqLocked(false, true);              // 0.164.0
            publishSemVa();                              // 0.128.0
            setProperty("NVGspControl-ce-token", ceToken_, 32);
            // 0.68.0: request max perf (kperfBoostSet → INTERNAL_PERF_BOOST_
            // SET_2X {flags BOOST_TO_MAX, duration INFINITE}) before parking.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdevice = 0xc0d02080;
            constexpr UInt32 kCmd = 0x20800a9a, kBytes = 8;
            UInt8 control[24 + 8]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kSubdevice, 4);
            __builtin_memcpy(control + 8, &kCmd, 4);
            __builtin_memcpy(control + 16, &kBytes, 4);
            // 0.133.0: BOOST_TO_MAX with an infinite duration pinned the GPU in
            // P0 forever (idle ~P0 power). Clearing it lets GSP drop to P8 at
            // idle (like Windows, ~11 W) and GPU Boost still reaches ~2850 MHz
            // under load (measured 27 Sep: 47.7 TFLOPS). nvram nvgsp-boost=1
            // brings the old pinned-max behaviour back.
            const bool pinMax = nvramU32("nvgsp-boost") == 1;
            nvramBoostPinned_ = pinMax;
            control[24] = pinMax ? 2 : 0;  // BOOST_TO_MAX : CLEAR
            const UInt32 duration = pinMax ? 0xffffffff : 0;
            __builtin_memcpy(control + 28, &duration, 4);
            setProperty("NVGspControl-perf-boost-pinned", pinMax);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 221;
            else
                ctxTeardown();
        } else if (perfReturned && postInitPhase_ == 221) {
            // 0.68.0: then read the current P-state (0x20802068, 4B).
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdevice = 0xc0d02080;
            constexpr UInt32 kCmd = 0x20802068, kBytes = 4;
            UInt8 control[24 + 4]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kSubdevice, 4);
            __builtin_memcpy(control + 8, &kCmd, 4);
            __builtin_memcpy(control + 16, &kBytes, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 222;
            else
                ctxTeardown();
        } else if (perfReturned && postInitPhase_ == 222) {
            // 0.69.0: interrupt routing table (INTERNAL_INTR_GET_KERNEL_TABLE
            // 0x20800a5c, 2068B) — which stall/non-stall vector each engine
            // (GSP, DISP, FIFO, CE, faults) raises in the CPU interrupt tree.
            constexpr UInt32 kCmd = 0x20800a5c, kBytes = 2068;
            const UInt32 client = internalClient_ ? internalClient_ : 0xc0d00001;
            const UInt32 subdev = internalSubdevice_ ? internalSubdevice_
                                                     : 0xc0d02080;
            UInt8 *control = static_cast<UInt8 *>(IOMalloc(24 + kBytes));
            bool sent = false;
            if (control) {
                bzero(control, 24 + kBytes);
                __builtin_memcpy(control, &client, 4);
                __builtin_memcpy(control + 4, &subdev, 4);
                __builtin_memcpy(control + 8, &kCmd, 4);
                __builtin_memcpy(control + 16, &kBytes, 4);
                sent = init_.enqueueRpc(76, control, 24 + kBytes);
                IOFree(control, 24 + kBytes);
            }
            if (sent)
                postInitPhase_ = 223;
            else
                ctxTeardown();
        } else if (perfReturned && (postInitPhase_ == 223 ||
                                    postInitPhase_ == 224)) {
            // 0.77.0: DP AUX over RM (NV0073_CTRL_CMD_DP_AUXCH_CTRL 0x731341,
            // nvkms-rm.c pattern) on display 0x200 (DP_EXT): DPCD 0x000,
            // 0x100, 0x200 (16 B each), then EDID via I2C-over-AUX at 0x50:
            // offset write (MOT) + 16 x 16 B reads (256 B, base + 1 ext).
            if (postInitPhase_ == 223) {
                auxStep_ = 0;
                auxRetry_ = 0;
                edidBytes_ = 0;
            } else if (!auxOk_ && auxLastReply_ == 2 && auxRetry_ < 5) {
                ++auxRetry_;                 // DEFER: repeat the step
            } else if (!auxOk_ && auxStep_ >= 5) {
                auxStep_ = 20;               // EDID ended early (1 block)
            } else {
                auxRetry_ = 0;
                ++auxStep_;
            }
            if (auxStep_ >= 20) {
                setProperty("NVGspControl-dpcd", dpcd_, sizeof(dpcd_));
                if (edidBytes_) setProperty("NVGspControl-edid", edid_, edidBytes_);
                setProperty("NVGspControl-edid-bytes", edidBytes_, 32);
                setProperty("NVGspControl-aux-last-status", auxLastStatus_, 32);
                setProperty("NVGspControl-aux-last-reply", auxLastReply_, 32);
                ctxTeardown();
            } else {
                constexpr UInt32 kClientHandle = 0xc0d00001;
                constexpr UInt32 kCommon = 0xc0d00073;
                constexpr UInt32 kCmd = 0x731341, kBytes = 48;
                UInt8 control[24 + 48]{};
                __builtin_memcpy(control, &kClientHandle, 4);
                __builtin_memcpy(control + 4, &kCommon, 4);
                __builtin_memcpy(control + 8, &kCmd, 4);
                __builtin_memcpy(control + 16, &kBytes, 4);
                UInt8 *pp = control + 24;
                const UInt32 displayId = 0x200;
                UInt32 cmd = 0, addr = 0, size = 15;
                if (auxStep_ <= 2) {
                    cmd = 0x9;                      // AUX | READ
                    addr = auxStep_ * 0x100;
                } else if (auxStep_ == 3) {
                    cmd = 0x4;                      // I2C | MOT | WRITE
                    addr = 0x50;
                    size = 0;
                    pp[20] = 0;                     // EDID offset 0
                } else {
                    cmd = auxStep_ < 19 ? 0x5 : 0x1;   // I2C READ (+MOT)
                    addr = 0x50;
                }
                __builtin_memcpy(pp + 4, &displayId, 4);
                __builtin_memcpy(pp + 12, &cmd, 4);
                __builtin_memcpy(pp + 16, &addr, 4);
                __builtin_memcpy(pp + 36, &size, 4);
                if (init_.enqueueRpc(76, control, sizeof(control)))
                    postInitPhase_ = 224;
                else
                    ctxTeardown();
            }
        } else if (perfReturned) {
            ctxTeardown();
        } else if (dispHealthReturned && dispHealthOk &&
                   postInitPhase_ >= 203 && postInitPhase_ <= 205) {
            // 0.53.0: 204 GET_ACTIVE (0x73010c, 16B), 205/206 GET_SCANLINE
            // (0x730104, 20B), all head 0 on display common 0xc0d00073.
            const UInt32 next = postInitPhase_ + 1;
            const UInt32 cmd = next == 204 ? 0x73010c : 0x730104;
            const UInt32 bytes = next == 204 ? 16 : 20;
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kCommon = 0xc0d00073;
            UInt8 control[24 + 20]{};
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kCommon, 4);
            __builtin_memcpy(control + 8, &cmd, 4);
            __builtin_memcpy(control + 16, &bytes, 4);
            if (next == 206) IODelay(3000);  // let the raster move
            if (init_.enqueueRpc(76, control, 24 + bytes))
                postInitPhase_ = next;
            else
                ctxTeardown();
        } else if (dispHealthReturned) {
            // 0.53.0: health sweep finished (or refused) -> teardown.
            // 0.55.0: if the window channel owns the screen, freeing the
            // client disables the window (0.51-0.54 black screen). Keep the
            // client, display channels, GR context and mappings alive
            // (persistent client) and finish at phase 33 without a free.
            if (wndOwnsScreen_) {
                // 0.57.0: display owned -> continue into the GR chain
                // (FIFO/runlist/bind/schedule/golden ctx/objects) and end
                // persistent there.
                constexpr UInt32 kClientHandle100 = 0xc0d00001;
                constexpr UInt32 kObject100 = 0xc0d02080;
                constexpr UInt32 kCommand100 = 0x20801108;
                constexpr UInt32 kParamsBytes100 = 8;
                UInt8 control100[24 + kParamsBytes100]{};
                __builtin_memcpy(control100, &kClientHandle100, 4);
                __builtin_memcpy(control100 + 4, &kObject100, 4);
                __builtin_memcpy(control100 + 8, &kCommand100, 4);
                __builtin_memcpy(control100 + 16, &kParamsBytes100, 4);
                if (init_.enqueueRpc(76, control100, sizeof(control100)))
                    postInitPhase_ = 100;
                else
                    ctxTeardown();
            } else {
                ctxTeardown();
            }
        } else if (dispInstMemReturned || dispInstWriteReturned) {
            // 0.51.0: display probe refused -> teardown (BAR1 still bound).
            ctxTeardown();
        } else if (dispPbProgramReturned) {
            // 0.8.7: stage a harmless UPDATE method with zero data. Header:
            // METHOD opcode, count=1, offset=NVC77D_UPDATE (0x200).
            setProperty("NVGspControl-disp-pb-program-ok",
                        dispPbProgramResponse);
            UInt64 updatePacket = 0x0000000000040200ULL;
            PraminPteResult pbWrite{};
            const bool pbWriteOk = dispPbProgramResponse &&
                dispPbBackingOffset_ != 0 && praminPteAccess(
                    pci_, dispPbBackingOffset_, &updatePacket,
                    true, true, &pbWrite);
            setProperty("NVGspControl-disp-pb-write-ok", pbWriteOk);
            setProperty("NVGspControl-disp-pb-write-readback",
                        pbWrite.observedPte, 64);
            if (pbWriteOk) {
                constexpr UInt32 kClientHandle = 0xc0d00001;
                constexpr UInt32 kDeviceHandle = 0xc0d00080;
                constexpr UInt32 kCommand = 0x00801805;
                constexpr UInt32 kParamsBytes = 4;
                constexpr UInt32 kFbFlush = 0x4;
                UInt8 control[24 + kParamsBytes]{};
                __builtin_memcpy(control, &kClientHandle, 4);
                __builtin_memcpy(control + 4, &kDeviceHandle, 4);
                __builtin_memcpy(control + 8, &kCommand, 4);
                __builtin_memcpy(control + 16, &kParamsBytes, 4);
                __builtin_memcpy(control + 24, &kFbFlush, 4);
                if (init_.enqueueRpc(76, control, sizeof(control))) {
                    postInitPhase_ = 70;
                    return kIOReturnSuccess;
                }
            }
            // Read-only PUT/GET survey at
            // BAR0+0x680000 (+0 PUT, +4 GET) via the existing BAR0 map,
            // then teardown. NO PUT write in this probe.
            IOMemoryMap *bar0Map = sharedBar0Map(pci_);
            UInt32 put = 0, get = 0;
            bool surveyOk = false;
            if (bar0Map && bar0Map->getLength() >= 0x00680008) {
                Bar0Io bar0{bar0Map};
                surveyOk = bar0.read(0x00680000, &put) &&
                    bar0.read(0x00680004, &get);
            }
            if (bar0Map) bar0Map->release();
            setProperty("NVGspControl-disp-put", put, 32);
            setProperty("NVGspControl-disp-get", get, 32);
            setProperty("NVGspControl-disp-survey-ok", surveyOk);
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restore-readback",
                        restore.observedPte, 64);
            setProperty("NVGspControl-gpfifo-pte-restore-window-restored",
                        restore.windowRestored, 32);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        } else if (tsgScheduleReturned) {
            // 0.7.2 stops at a scheduled TSG: no method is submitted
            // yet. Teardown is identical on success and refusal.
            setProperty("NVGspControl-tsg-schedule-ok", tsgScheduleResponse);
            setProperty("NVGspControl-channel-live", tsgScheduleResponse);
            if (!tsgScheduleResponse)
                setProperty("NVGspControl-channel-skipped-reason",
                            "tsg-schedule-refused");
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            gpfifoPteRestored_ = gpfifoPteInstalled_ && praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-gpfifo-pte-restore-readback",
                        restore.observedPte, 64);
            setProperty("NVGspControl-gpfifo-pte-restore-window-restored",
                        restore.windowRestored, 32);
            setProperty("NVGspControl-gpfifo-pte-restored",
                        gpfifoPteRestored_);
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 38;
        } else if (virtualMemoryAllocResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            constexpr UInt32 kParamsBytes = 192;
            UInt8 control[24 + kParamsBytes]{};
            constexpr UInt32 kCommand = 0x00801806;
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kDeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            __builtin_memcpy(control + 24 + 164, &kVaspaceHandle, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 34;
            else
                badReason = 25;
        } else if (vaCapsResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            constexpr UInt32 kParamsBytes = 208;
            UInt8 control[24 + kParamsBytes]{};
            constexpr UInt32 kCommand = 0x00801809;
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kDeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            __builtin_memcpy(control + 24, &virtualOffset_, 8);
            __builtin_memcpy(control + 24 + 200, &kVaspaceHandle, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 35;
            else
                badReason = 26;
        } else if (pdeInfoResponse) {
            // 0.6.2: install ONE fresh Ada PTE mapping the GPFIFO backing VRAM
            // page at the virtual window (proven PRAMIN path; readback must
            // match). Fresh VA in a fresh VASpace has no stale TLB entry, so NO
            // invalidate RPC is enqueued (the known phase-39 stall is avoided
            // by construction, not retried). Then allocate the channel.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            const UInt64 physicalPage = gpfifoBackingOffset_ >> 12;
            UInt64 mappedPte = 1ULL |
                ((physicalPage & 0x01ffffffULL) << 8) | (6ULL << 36);
            PraminPteResult access{};
            gpfifoPteInstalled_ =
                praminPteReadOk_ && originalPte_ == 0 &&
                (gpfifoBackingOffset_ & 0xfff) == 0 && pte4KAddress_ &&
                praminPteAccess(pci_, pte4KAddress_, &mappedPte, true, true,
                                &access);
            setProperty("NVGspControl-gpfifo-pte-encoded", mappedPte, 64);
            setProperty("NVGspControl-gpfifo-pte-readback",
                        access.observedPte, 64);
            setProperty("NVGspControl-gpfifo-pte-window-restored",
                        access.windowRestored, 32);
            setProperty("NVGspControl-gpfifo-pte-installed",
                        gpfifoPteInstalled_);
            if (!gpfifoPteInstalled_) {
                setProperty("NVGspControl-channel-skipped-reason",
                            "pte-install-failed");
                UInt8 freeParams[16]{};
                __builtin_memcpy(freeParams, &kClientHandle, 4);
                __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                    postInitPhase_ = 32;
                else
                    badReason = 27;
            } else {
                constexpr UInt32 kChannelHandle = 0xc0d0c56f;
                constexpr UInt32 kChannelClass = nvgsp::kAmpereChannelGpfifoA;
                nvgsp::NvChannelAllocParams chan{};
                // 0.39.0: GR engine (nouveau r535 golden-init channel uses
                // engineType 1; a COPY0-allocated channel hung GSP in the
                // 0.38.0 GR object alloc). Was COPY0 since 0.7.7.
                const bool chanOk = nvgsp::buildChannelAllocParams(
                    0xc0d090f1, 0xc0d00043, virtualOffset_, 512,
                    nvgsp::kEngineTypeGraphics, &chan);
                // Zero is valid: KernelChannel selects notifier type NONE.
                chan.hObjectError = 0;
                // Mirror CPU-RM's split-GSP channel payload. GSP-RM describes
                // these physical tuples; it does not allocate them itself.
                chan.flags = nvgsp::kChannelFlagsUserdPageSlot3;
                chan.internalFlags =
                    nvgsp::kChannelInternalFlagsNotifierNone;
                chan.instanceMem = {instanceBackingOffset_, 4096, 2, 1};
                chan.ramfcMem = {instanceBackingOffset_, 512, 2, 1};
                chan.userdMem = {userdBackingOffset_, 512, 2, 1};
                chan.mthdbufMem = {methodBackingOffset_,
                                   methodBufferBytes_, 2, 0};
                const bool instanceZero = praminZeroRange(
                    pci_, instanceBackingOffset_, instanceBackingSize_);
                const bool userdZero = praminZeroRange(
                    pci_, userdBackingOffset_, userdBackingSize_);
                const bool methodZero = praminZeroRange(
                    pci_, methodBackingOffset_, methodBackingSize_);
                setProperty("NVGspControl-instance-backing-zeroed",
                            instanceZero);
                setProperty("NVGspControl-userd-backing-zeroed", userdZero);
                setProperty("NVGspControl-method-backing-zeroed", methodZero);
                const bool splitBackingZeroed =
                    instanceZero && userdZero && methodZero;
                if (!splitBackingZeroed) {
                    setProperty("NVGspControl-channel-skipped-reason",
                                "split-backing-zero-failed");
                    UInt8 freeParams[16]{};
                    __builtin_memcpy(freeParams, &kClientHandle, 4);
                    __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                    if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                        postInitPhase_ = 32;
                    else
                        badReason = 44;
                } else {
                    channelGpFifoVa_ = virtualOffset_;
                    setProperty("NVGspControl-channel-params-built", chanOk);
                    setProperty("NVGspControl-channel-gpfifo-va",
                                channelGpFifoVa_, 64);
                    if (!chanOk) {
                        badReason = 27;
                    } else {
                        constexpr UInt32 kParamsBytes = sizeof(chan);
                        UInt8 alloc[32 + kParamsBytes]{};
                        __builtin_memcpy(alloc, &kClientHandle, 4);
                        __builtin_memcpy(alloc + 4, &kDeviceHandle, 4);
                        __builtin_memcpy(alloc + 8, &kChannelHandle, 4);
                        __builtin_memcpy(alloc + 12, &kChannelClass, 4);
                        __builtin_memcpy(alloc + 20, &kParamsBytes, 4);
                        __builtin_memcpy(alloc + 32, &chan, kParamsBytes);
                        if (init_.enqueueRpc(103, alloc, sizeof(alloc)))
                            postInitPhase_ = 46;
                        else
                            badReason = 27;
                    }
                }
            }
        } else if (bar2MapResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00040;
            constexpr UInt32 kParamsBytes = 12;
            UInt8 control[24 + kParamsBytes]{};
            constexpr UInt32 kCommand = 0x2080180b;
            constexpr UInt32 kOffset = 0;
            constexpr UInt32 kSize = 4096;
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            __builtin_memcpy(control + 24, &kMemoryHandle, 4);
            __builtin_memcpy(control + 28, &kOffset, 4);
            __builtin_memcpy(control + 32, &kSize, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 37;
            else
                badReason = 28;
        } else if (bar2MapUnsupportedResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kEngineMask = 0;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            // Ada uses the version-2 8-byte PTE format. For video memory,
            // physical page number occupies bits 32:8, VALID is bit 0, and
            // generic-memory kind 6 occupies bits 43:36.
            const UInt64 physicalPage = localMemoryOffset_ >> 12;
            UInt64 mappedPte = 1ULL |
                ((physicalPage & 0x01ffffffULL) << 8) | (6ULL << 36);
            PraminPteResult access{};
            hostPteMapped_ = praminPteReadOk_ && originalPte_ == 0 &&
                (localMemoryOffset_ & 0xfff) == 0 && pte4KAddress_ &&
                praminPteAccess(pci_, pte4KAddress_, &mappedPte, true, true,
                                &access);
            setProperty("NVGspControl-host-pte-encoded", mappedPte, 64);
            setProperty("NVGspControl-host-pte-map-readback",
                        access.observedPte, 64);
            setProperty("NVGspControl-host-pte-map-window-before",
                        access.windowBefore, 32);
            setProperty("NVGspControl-host-pte-map-window-restored",
                        access.windowRestored, 32);
            setProperty("NVGspControl-host-pte-mapped", hostPteMapped_);
            if (hostPteMapped_) {
                constexpr UInt32 kParamsBytes = 16;
                UInt8 control[24 + kParamsBytes]{};
                constexpr UInt32 kCommand = 0x20802502;
                __builtin_memcpy(control, &kClientHandle, 4);
                __builtin_memcpy(control + 4, &kSubdeviceHandle, 4);
                __builtin_memcpy(control + 8, &kCommand, 4);
                __builtin_memcpy(control + 16, &kParamsBytes, 4);
                __builtin_memcpy(control + 24, &kClientHandle, 4);
                __builtin_memcpy(control + 28, &kDeviceHandle, 4);
                __builtin_memcpy(control + 32, &kEngineMask, 4);
                __builtin_memcpy(control + 36, &kVaspaceHandle, 4);
                if (init_.enqueueRpc(76, control, sizeof(control)))
                    postInitPhase_ = 39;
                else
                    badReason = 29;
            } else {
                // If any write/readback step failed, force the saved invalid
                // entry back before releasing the object tree.
                PraminPteResult restore{};
                UInt64 original = originalPte_;
                hostPteRestored_ = praminPteAccess(
                    pci_, pte4KAddress_, &original, true, false, &restore);
                UInt8 freeParams[16]{};
                __builtin_memcpy(freeParams, &kClientHandle, 4);
                __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
                if (hostPteRestored_ &&
                    init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                    postInitPhase_ = 32;
                else
                    badReason = 29;
            }
        } else if (pteMapInvalidateResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            constexpr UInt32 kParamsBytes = 184;
            UInt8 control[24 + kParamsBytes]{};
            constexpr UInt32 kCommand = 0x00801801;
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kDeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            __builtin_memcpy(control + 24, &virtualOffset_, 8);
            __builtin_memcpy(control + 24 + 176, &kVaspaceHandle, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 40;
            else
                badReason = 32;
        } else if (pteInfoReturned) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kEngineMask = 0;
            constexpr UInt32 kVaspaceHandle = 0xc0d090f1;
            constexpr UInt32 kParamsBytes = 16;
            UInt64 original = originalPte_;
            PraminPteResult restore{};
            hostPteRestored_ = praminPteAccess(
                pci_, pte4KAddress_, &original, true, false, &restore);
            setProperty("NVGspControl-host-pte-restore-readback",
                        restore.observedPte, 64);
            setProperty("NVGspControl-host-pte-restore-window-before",
                        restore.windowBefore, 32);
            setProperty("NVGspControl-host-pte-restore-window-restored",
                        restore.windowRestored, 32);
            setProperty("NVGspControl-host-pte-restored", hostPteRestored_);
            if (hostPteRestored_) {
                UInt8 control[24 + kParamsBytes]{};
                constexpr UInt32 kCommand = 0x20802502;
                __builtin_memcpy(control, &kClientHandle, 4);
                __builtin_memcpy(control + 4, &kSubdeviceHandle, 4);
                __builtin_memcpy(control + 8, &kCommand, 4);
                __builtin_memcpy(control + 16, &kParamsBytes, 4);
                __builtin_memcpy(control + 24, &kClientHandle, 4);
                __builtin_memcpy(control + 28, &kDeviceHandle, 4);
                __builtin_memcpy(control + 32, &kEngineMask, 4);
                __builtin_memcpy(control + 36, &kVaspaceHandle, 4);
                if (init_.enqueueRpc(76, control, sizeof(control)))
                    postInitPhase_ = 41;
                else
                    badReason = 33;
            } else {
                badReason = 33;
            }
        } else if (pteRestoreInvalidateResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 34;
        } else if (bar2VerifyResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kSubdeviceHandle = 0xc0d02080;
            constexpr UInt32 kMemoryHandle = 0xc0d00040;
            constexpr UInt32 kParamsBytes = 4;
            UInt8 control[24 + kParamsBytes]{};
            constexpr UInt32 kCommand = 0x2080180a;
            __builtin_memcpy(control, &kClientHandle, 4);
            __builtin_memcpy(control + 4, &kSubdeviceHandle, 4);
            __builtin_memcpy(control + 8, &kCommand, 4);
            __builtin_memcpy(control + 16, &kParamsBytes, 4);
            __builtin_memcpy(control + 24, &kMemoryHandle, 4);
            if (init_.enqueueRpc(76, control, sizeof(control)))
                postInitPhase_ = 38;
            else
                badReason = 30;
        } else if (bar2UnmapResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVirtualHandle = 0xc0d050a0;
            constexpr UInt32 kMemoryHandle = 0xc0d00040;
            UInt8 map[56]{};
            __builtin_memcpy(map, &kClientHandle, 4);
            __builtin_memcpy(map + 4, &kDeviceHandle, 4);
            __builtin_memcpy(map + 8, &kVirtualHandle, 4);
            __builtin_memcpy(map + 12, &kMemoryHandle, 4);
            constexpr UInt64 kBytes = 4096;
            constexpr UInt32 kMapFlags = 0x110; // cache snoop + 4 KiB page
            __builtin_memcpy(map + 24, &kBytes, 8);
            __builtin_memcpy(map + 32, &kMapFlags, 4);
            __builtin_memcpy(map + 40, &virtualOffset_, 8);
            if (init_.enqueueRpc(14, map, sizeof(map)))
                postInitPhase_ = 30;
            else
                badReason = 31;
        } else if (dmaMapResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            constexpr UInt32 kDeviceHandle = 0xc0d00080;
            constexpr UInt32 kVirtualHandle = 0xc0d050a0;
            constexpr UInt32 kMemoryHandle = 0xc0d00040;
            UInt8 unmap[40]{};
            __builtin_memcpy(unmap, &kClientHandle, 4);
            __builtin_memcpy(unmap + 4, &kDeviceHandle, 4);
            __builtin_memcpy(unmap + 8, &kVirtualHandle, 4);
            __builtin_memcpy(unmap + 12, &kMemoryHandle, 4);
            __builtin_memcpy(unmap + 24, &virtualOffset_, 8);
            if (init_.enqueueRpc(15, unmap, sizeof(unmap)))
                postInitPhase_ = 31;
            else
                badReason = 32;
        } else if (dmaMapUnsupportedResponse) {
            // Modern bare-metal GSP uses split VAS management: GPU page-table
            // writes stay in the host RM, so the server deliberately has no
            // function-14 MAP_MEMORY_DMA handler. Preserve that capability
            // result and cleanly release the isolated discovery tree.
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 33;
        } else if (dmaUnmapResponse) {
            constexpr UInt32 kClientHandle = 0xc0d00001;
            UInt8 freeParams[16]{};
            __builtin_memcpy(freeParams, &kClientHandle, 4);
            __builtin_memcpy(freeParams + 8, &kClientHandle, 4);
            if (init_.enqueueRpc(10, freeParams, sizeof(freeParams)))
                postInitPhase_ = 32;
            else
                badReason = 34;
        } else if (ownedTreeFreeResponse) {
            postInitPhase_ = 33;
        }
    }
    setProperty("NVGspControl-status-records", records,
                recordCount * sizeof(nvgsp::GspStatusRecordSummary));
    setProperty("NVGspControl-status-record-count", recordCount, 32);
    setProperty("NVGspControl-status-consumed", consumed, 32);
    setProperty("NVGspControl-status-next-sequence", statusSequence_, 32);
    setProperty("NVGspControl-status-libos-prints", libosPrints, 32);
    setProperty("NVGspControl-status-os-errors", osErrors, 32);
    setProperty("NVGspControl-status-lockdown-notices", lockdownNotices, 32);
    setProperty("NVGspControl-status-bad-reason", badReason, 32);
    setProperty("NVGspControl-status-blocked-function", blockedFunction, 32);
    if (blockedData) {
        setProperty("NVGspControl-status-blocked-data", blockedData, blockedBytes);
        IOFree(blockedData, blockedBytes);
    }
    setProperty("NVGspControl-status-init-done", initDone);
    setProperty("NVGspControl-status-init-result", initResult, 32);
    setProperty("NVGspControl-status-init-private-result", initPrivateResult, 32);
    setProperty("NVGspControl-post-init-phase", postInitPhase_, 32);
    setProperty("NVGspControl-post-init-complete", postInitPhase_ >= 4);
    setProperty("NVGspControl-engine-enumeration-complete", postInitPhase_ >= 6);
    setProperty("NVGspControl-class-list-complete", postInitPhase_ >= 8);
    setProperty("NVGspControl-client-lifecycle-complete", postInitPhase_ >= 12);
    setProperty("NVGspControl-gpu-id-discovery-complete", postInitPhase_ >= 16);
    setProperty("NVGspControl-device-lifecycle-complete", postInitPhase_ == 33);
    setProperty("NVGspControl-subdevice-lifecycle-complete", postInitPhase_ == 33);
    setProperty("NVGspControl-vaspace-lifecycle-complete", postInitPhase_ == 33);
    setProperty("NVGspControl-local-memory-lifecycle-complete", postInitPhase_ == 33);
    setProperty("NVGspControl-virtual-memory-lifecycle-complete", postInitPhase_ == 33);
    setProperty("NVGspControl-va-caps-complete", postInitPhase_ == 33);
    setProperty("NVGspControl-pde-info-complete", postInitPhase_ == 33);
    setProperty("NVGspControl-pramin-pte-read-complete",
                postInitPhase_ == 33 && praminPteReadOk_);
    setProperty("NVGspControl-host-gmmu-map-lifecycle-complete",
                postInitPhase_ == 33 && hostPteMapped_ &&
                hostPteValidated_ && hostPteRestored_ &&
                pteMapInvalidateOk_ && pteRestoreInvalidateOk_);
    setProperty("NVGspControl-channel-probe-lifecycle-complete",
                postInitPhase_ == 33 && gpfifoPteRestored_);
    setProperty("NVGspControl-bar2-lifecycle-complete",
                postInitPhase_ == 33 && !bar2MapUnsupported_);
    setProperty("NVGspControl-host-aperture-discovery-complete",
                postInitPhase_ == 33 && bar2MapUnsupported_);
    if (postInitPhase_ == 33) {
        IOMemoryMap *bar0Map = sharedBar0Map(pci_);
        if (bar0Map && bar0Map->getLength() > 0x1708) {
            volatile UInt32 *bar0 = reinterpret_cast<volatile UInt32 *>(
                bar0Map->getVirtualAddress());
            OSSynchronizeIO();
            const UInt32 bar1Block = bar0[0x1704 / 4];
            const UInt32 bindStatus = bar0[0x1708 / 4];
            OSSynchronizeIO();
            setProperty("NVGspControl-pbus-bar1-block", bar1Block, 32);
            setProperty("NVGspControl-pbus-bind-status", bindStatus, 32);
            bar0Map->release();
        } else if (bar0Map) {
            bar0Map->release();
        }
        IODeviceMemory *bar1 = bar1Dev();
        IODeviceMemory *bar3 = bar3Dev();
        if (bar1) {
            setProperty("NVGspControl-host-bar1-physical",
                        bar1->getPhysicalAddress(), 64);
            setProperty("NVGspControl-host-bar1-length", bar1->getLength(), 64);
        }
        if (bar3) {
            setProperty("NVGspControl-host-bar3-physical",
                        bar3->getPhysicalAddress(), 64);
            setProperty("NVGspControl-host-bar3-length", bar3->getLength(), 64);
        }
    }
    setProperty("NVGspControl-dma-map-lifecycle-complete", dmaMapCompleted_);
    // 0.6.3: LibOS log-heads snapshot REMOVED. Its 20 KiB per-poll OSData
    // churn was the exact crash site of the 0.6.2 panic (double fault in
    // OSData::free while replacing the property). The logs served 0.2.9-era
    // INIT_DONE diagnosis and are not needed now; the GSP queue path is
    // unaffected.
    if (deviceInfo) IOFree(deviceInfo, deviceInfoBytes);
    IOFree(staticInfo, kStaticInfoBytes);
    IOFree(records, kMaxRecords * sizeof(nvgsp::GspStatusRecordSummary));
    return initDone_ && initResult_ == 0 && !badReason
        ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn NVGspControl::stagePackage(const void *bytes, size_t length) {
    if (!pci_ || !bytes || length != kNVGspPackageBytes)
        return kIOReturnBadArgument;
    if (staged_) return kIOReturnExclusiveAccess;

    nvgsp::PackageView package[5]{};
    const bool packageOk = nvgsp::parsePackage(bytes, length, package);
    setProperty("NVGspControl-package-ok", packageOk);
    setProperty("NVGspControl-package-bytes", static_cast<uint64_t>(length), 64);
    if (!packageOk) return kIOReturnBadMedia;

    IODeviceMemory *bar0 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IOMemoryMap *map = bar0 ? bar0->map() : nullptr;
    UInt32 fbSizeMb = 0, vgaWorkspace = 0, fuseRaw = 0;
    const bool registersOk = map && read32(map, kUsableFbSizeMb, &fbSizeMb) &&
        read32(map, kVgaWorkspaceBase, &vgaWorkspace) &&
        read32(map, kSec2Ucode3Fuse, &fuseRaw);
    if (map) map->release();
    setProperty("NVGspControl-registers-ok", registersOk);
    if (!registersOk || !fbSizeMb) return kIOReturnNotReady;
    const UInt32 fuseVersion = fuseRaw & 0xff;

    nvgsp::BootUcodeDesc bootDesc{};
    __builtin_memcpy(&bootDesc,
        package[nvgsp::kPackageBootDescriptor - 1].data, sizeof(bootDesc));
    nvgsp::BooterView booterView{};
    const bool booterParseOk = nvgsp::parseBooterLoad(
        package[nvgsp::kPackageBooterLoad - 1].data,
        package[nvgsp::kPackageBooterLoad - 1].size, &booterView);
    nvgsp::FbLayout layout{};
    const bool layoutOk = nvgsp::planAd103FbLayout(
        static_cast<uint64_t>(fbSizeMb) << 20,
        (vgaWorkspace & (1U << 3)) != 0,
        static_cast<uint64_t>(vgaWorkspace & 0xffffff00U) << 8,
        package[nvgsp::kPackageBootImage - 1].size,
        package[nvgsp::kPackageFwImage - 1].size, &layout);
    // Reserve every small physically contiguous object before the 63 MiB
    // scattered firmware mapping. On a live system the reverse order can
    // fragment the DMA aperture enough to reject Booter Load's 60 KiB block.
    const bool booterOk = booterParseOk && booter_.stage(booterView, fuseVersion);
    // 0.100.0: Booter Unload for GSP SR (same ucode id 3 / fuse as Load).
    fuseVersion_ = fuseVersion;
    {
        nvgsp::BooterView unloadView{};
        const bool unloadOk = !booterUnload_.ready() &&
            nvgsp::parseBooterLoad(nvgsp::kBooterUnloadAd10x,
                                   sizeof(nvgsp::kBooterUnloadAd10x), &unloadView) &&
            booterUnload_.stage(unloadView, fuseVersion);
        setProperty("NVGspControl-sr-unload-staged", unloadOk || booterUnload_.ready());
    }
    nvgsp::GspSystemInfoParameters system{};
    IODeviceMemory *bar1 = bar1Dev();
    IODeviceMemory *bar2 = bar3Dev();
    system.bar0 = bar0 ? bar0->getPhysicalAddress() : 0;
    system.bar1 = bar1 ? bar1->getPhysicalAddress() : 0;
    system.bar2 = bar2 ? bar2->getPhysicalAddress() : 0;
    // 0.9.2: capture the GOP surface hash BEFORE FWSEC runs. BAR1+0 is
    // the live GOP scan-out surface on a fresh boot; FWSEC/GSP boot may
    // clobber it. Compared against the post-boot hash at phase 75.
    gopSurfaceHashPre_ = 0;
    gopSurfaceHashPreOk_ = false;
    if (bar1 && bar1->getLength() >= 4096) {
        IOMemoryMap *bar1Map = mapBar1Head(4096);
        if (bar1Map && bar1Map->getLength() >= 4096) {
            const volatile UInt8 *bytes =
                reinterpret_cast<const volatile UInt8 *>(
                    static_cast<uintptr_t>(bar1Map->getVirtualAddress()));
            UInt64 hash = 14695981039346656037ULL;
            for (unsigned i = 0; i < 4096; ++i) {
                hash ^= bytes[i];
                hash *= 1099511628211ULL;
            }
            gopSurfaceHashPre_ = hash;
            gopSurfaceHashPreOk_ = true;
            // 0.9.4: also keep the first 64 bytes for forensics.
            for (unsigned w = 0; w < 8; ++w) {
                UInt64 word = 0;
                for (unsigned b = 0; b < 8; ++b)
                    word |= static_cast<UInt64>(bytes[w * 8 + b])
                        << (b * 8);
                gopSurfaceBytesPre_[w] = word;
            }
        }
        if (bar1Map) bar1Map->release();
    }
    setProperty("NVGspControl-gop-surface-hash-pre",
                gopSurfaceHashPre_, 64);
    setProperty("NVGspControl-gop-surface-hash-pre-ok",
                gopSurfaceHashPreOk_);
    setProperty("NVGspControl-gop-surface-pre-w0",
                gopSurfaceBytesPre_[0], 64);
    setProperty("NVGspControl-gop-surface-pre-w1",
                gopSurfaceBytesPre_[1], 64);
    setProperty("NVGspControl-gop-surface-pre-w2",
                gopSurfaceBytesPre_[2], 64);
    setProperty("NVGspControl-gop-surface-pre-w3",
                gopSurfaceBytesPre_[3], 64);
    setProperty("NVGspControl-gop-surface-pre-w4",
                gopSurfaceBytesPre_[4], 64);
    setProperty("NVGspControl-gop-surface-pre-w5",
                gopSurfaceBytesPre_[5], 64);
    setProperty("NVGspControl-gop-surface-pre-w6",
                gopSurfaceBytesPre_[6], 64);
    setProperty("NVGspControl-gop-surface-pre-w7",
                gopSurfaceBytesPre_[7], 64);
    system.domainBusDevice = (static_cast<UInt64>(pci_->getBusNumber()) << 8) |
                             pci_->getDeviceNumber();
    system.maxUserVa = 0x7fffffffffffULL;
    // 0.9.3: reserve the low-FB console region so GSP-RM never treats
    // the GOP scan-out surface as heap/scrub range. 64 MiB covers any
    // GOP mode up to 5K (our 4K surface is 33 MiB at BAR1+0).
    system.consoleMemSize = 64ULL * 1024 * 1024;
    system.pciDeviceId = pci_->configRead32(kIOPCIConfigVendorID);
    system.pciSubDeviceId = pci_->configRead32(kIOPCIConfigSubSystemVendorID);
    system.pciRevisionId = pci_->configRead8(kIOPCIConfigRevisionID);
    UInt8 pcieCapability = 0;
    pci_->findPCICapability(kIOPCIPCIExpressCapability, &pcieCapability);
    system.pcieLinkCap = pcieCapability ? pci_->configRead32(pcieCapability + 0x0c) : 0;
    system.gpuBehindBridge = 1;
    system.flrSupported = 0;
    system.bar0Is64Bit = (pci_->configRead32(kIOPCIConfigBaseAddress0) & 6) == 4;
    system.isPrimary = 1;
    // 0.113.0: optional GSP-RM registry from NVRAM nvgsp-registry
    // ("Key=Value;..."), applied on every GSP boot (cold, reset, resume).
    UInt32 regBytes = 0, regEntries = 0;
    UInt8 *reg = static_cast<UInt8 *>(IOMalloc(kRegistryCap));
    const bool regOk = reg && registryFromNvram(reg, kRegistryCap, &regBytes, &regEntries);
    setProperty("NVGspControl-registry-entries", regOk ? regEntries : 0, 32);
    setProperty("NVGspControl-registry-bytes", regOk ? regBytes : 0, 32);
    const bool initOk = booterOk && system.bar0 && system.bar1 && system.bar2 &&
        init_.stage(system, nullptr, regOk ? reg : nullptr, regOk ? regBytes : 0);
    if (reg) IOFree(reg, kRegistryCap);
    systemInfo_ = system;
    systemInfoValid_ = initOk;
    const bool gspOk = initOk && layoutOk && gsp_.stage(layout, bootDesc,
        package[nvgsp::kPackageFwImage - 1].data,
        package[nvgsp::kPackageFwImage - 1].size,
        package[nvgsp::kPackageSignature - 1].data,
        package[nvgsp::kPackageSignature - 1].size,
        package[nvgsp::kPackageBootImage - 1].data,
        package[nvgsp::kPackageBootImage - 1].size);
    setProperty("NVGspControl-fuse-raw", fuseRaw, 32);
    setProperty("NVGspControl-fuse-version", fuseVersion, 32);
    setProperty("NVGspControl-booter-parse-ok", booterParseOk);
    setProperty("NVGspControl-booter-stage-error", booter_.lastError(), 32);
    setProperty("NVGspControl-layout-ok", layoutOk);
    setProperty("NVGspControl-firmware-stage-ok", gspOk);
    setProperty("NVGspControl-booter-stage-ok", booterOk);
    setProperty("NVGspControl-init-stage-ok", initOk);
    setProperty("NVGspControl-metadata-bus", gsp_.metadataBusAddress(), 64);
    setProperty("NVGspControl-booter-bus", booter_.busAddress(), 64);
    setProperty("NVGspControl-libos-args-bus", init_.libosArgsBus(), 64);
    staged_ = gspOk && booterOk && initOk;
    setProperty("NVGspControl-staged", staged_);
    // 0.99.0: retain a post-stage queue snapshot (516 KiB) for the S3
    // re-boot: identical bytes incl. page table + pre-queued init RPCs.
    if (staged_ && !queueSnap_ && init_.queueBytes()) {
        queueSnap_ = static_cast<UInt8 *>(IOMalloc(nvgsp::kGspSharedBytes));
        if (queueSnap_)
            __builtin_memcpy(queueSnap_, init_.queueBytes(), nvgsp::kGspSharedBytes);
        setProperty("NVGspControl-queue-snapshot", queueSnap_ != nullptr);
    }
    IOLog("NVGspControl: package staged layout=%u gsp=%u booter=%u init=%u\n",
          layoutOk, gspOk, booterOk, initOk);
    if (!staged_) {
        init_.reset(); booter_.reset(); gsp_.reset();
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}


// ---- 0.100.0: GSP suspend/resume (SR) -------------------------------------
// NVIDIA kgspTeardown_TU102 / kgspBootstrap_TU102 (SR_RESUME) + nouveau
// r535_gsp_fini + r570_fbsr. Suspend: stop channel scheduling, save our own
// VRAM (display RAMIN), FBSR_INIT (RM saves its non-WPR/reserved VRAM into
// fbsrBuf_), UNLOADING_GUEST_DRIVER(bInPMTransition, level 3), GSP halts
// (mbox0 0x80000000), GSP falcon reset, FWSEC-SB, SEC2 Booter Unload with the
// SR metadata (WPR2 saved into srData_, WPR2 torn down). Resume: rmargs in
// PM-transition mode, SEC2 Booter Load with the SR metadata, FALCON_OS,
// RISC-V active, INIT_DONE; GSP-RM restores queues and its own state.

namespace {
struct GspFwSRMeta {
    uint64_t magic, revision, sysmemAddrOfSuspendResumeData, sizeOfSuspendResumeData;
    uint32_t internal[32];
    uint32_t flags, subrevision;
    uint32_t padding[22];
};
static_assert(sizeof(GspFwSRMeta) == 256, "GspFwSRMeta ABI");
constexpr uint64_t kSrMetaMagic = 0x8a3bb9e6c6c39d93ULL;
constexpr uint32_t kFbsrMemlist = 0xcaf00003;

bool srTablePage(void *ctx, uint64_t index, uint64_t *address) {
    return static_cast<nvgsp::DmaBuffer *>(ctx)[0].busPage(index, address);
}
}  // namespace

static nvgsp::DmaBuffer *gSrDataForRadix = nullptr;
static bool srImagePage(void *, uint64_t index, uint64_t *address) {
    return gSrDataForRadix && gSrDataForRadix->busPage(index, address);
}

bool NVGspControl::srStageBuffers() {
    if (srBuffersReady_) return true;
    const nvgsp::WprMeta *meta = gsp_.metadata();
    if (!meta || !booterUnload_.ready() || !dispInstOffset_) return false;
    const uint64_t srBytes = (meta->frtsOffset + meta->frtsSize) -
        (meta->nonWprHeapOffset + meta->nonWprHeapSize);
    nvgsp::RadixLayout rl{};
    if (!nvgsp::radixLayout(srBytes, &rl)) return false;
    bool ok = srData_.allocate(rl.paddedDataBytes, false) &&
              srRadix_.allocate(rl.tableBytes, true) &&
              srMeta_.allocate(4096, true);
    if (ok) {
        gSrDataForRadix = &srData_;
        ok = nvgsp::fillRadixTables(rl, srRadix_.bytes(), srRadix_.size(),
                                    srTablePage, srImagePage, &srRadix_) &&
             srRadix_.syncToDevice();
        gSrDataForRadix = nullptr;
    }
    if (ok) {
        GspFwSRMeta m{};
        m.magic = kSrMetaMagic;
        m.revision = 2;
        m.sysmemAddrOfSuspendResumeData = srRadix_.busAddress();
        m.sizeOfSuspendResumeData = srBytes;
        ok = srMeta_.write(0, &m, sizeof(m));
    }
    // RM's own VRAM: non-WPR heap + reserved (below it, above the last usable
    // FB region) + VGA workspace (nouveau r570_fbsr_suspend).
    uint64_t rsvd = 0;
    if (fbFreeLimit_ && meta->nonWprHeapOffset > fbFreeLimit_ + 1)
        rsvd = meta->nonWprHeapOffset - (fbFreeLimit_ + 1);
    const uint64_t fbsrBytes =
        (meta->nonWprHeapSize + rsvd + meta->vgaWorkspaceSize + 4095) & ~4095ULL;
    ok = ok && fbsrBytes && fbsrBuf_.allocate(fbsrBytes, false);
    if (ok && !dispSave_) dispSave_ = static_cast<UInt8 *>(IOMalloc(0x10000));
    ok = ok && dispSave_;
    setProperty("NVGspControl-sr-data-bytes", srBytes, 64);
    setProperty("NVGspControl-sr-fbsr-bytes", fbsrBytes, 64);
    setProperty("NVGspControl-sr-rsvd-bytes", rsvd, 64);
    setProperty("NVGspControl-sr-buffers", ok);
    if (!ok) {
        srData_.release(); srRadix_.release(); srMeta_.release(); fbsrBuf_.release();
        return false;
    }
    srBuffersReady_ = true;
    return true;
}

IOReturn NVGspControl::userRpcLarge(UInt32 function, const UInt8 *payload, UInt32 bytes,
                                    UInt32 *rpcResult) {
    constexpr UInt32 kMaxPayload = 16 * 4096 -
        sizeof(nvgsp::GspQueueElementHeader) - sizeof(nvgsp::RpcMessageHeader);
    if (!lock_ || !payload || !bytes) return kIOReturnBadArgument;
    lk(__LINE__);
    if (!waitRpcSlotLocked(6000)) {
        ulk();
        return kIOReturnBusy;
    }
    userRpcActive_ = true;
    userRpcFunction_ = function;
    userRpcOutstanding_ = true;
    userRpcReplyBytes_ = 0;
    largeReplyOk_ = true;
    bool ok = true;
    UInt32 fn = function;
    for (UInt32 off = 0; ok && off < bytes; ) {
        const UInt32 n = bytes - off > kMaxPayload ? kMaxPayload : bytes - off;
        bool queued = false;
        for (UInt32 t = 0; t < 3000 && !(queued = init_.enqueueRpc(fn, payload + off, n)); ++t) {
            pollStatusLocked();
            ulk();
            IOSleep(1);
            lk(__LINE__);
        }
        ok = queued;
        off += n;
        fn = 71;   // NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD
    }
    for (UInt32 ms = 0; ok && userRpcOutstanding_ && ms < 10000; ++ms) {
        pollStatusLocked();
        if (!userRpcOutstanding_) break;
        ulk();
        IOSleep(1);
        lk(__LINE__);
    }
    // Drain trailing continuation records of the reply.
    for (UInt32 i = 0; ok && i < 50; ++i) {
        pollStatusLocked();
        ulk();
        IOSleep(1);
        lk(__LINE__);
    }
    const bool done = ok && !userRpcOutstanding_;
    userRpcOutstanding_ = false;
    largeReplyOk_ = false;
    if (rpcResult) *rpcResult = done ? userRpcResult_ : ~0U;
    userRpcActive_ = false;
    ulk();
    return done ? kIOReturnSuccess : kIOReturnTimeout;
}

static UInt32 srCtrl(NVGspControl *self, UInt32 client, UInt32 object, UInt32 cmd,
                     const void *params, UInt32 bytes) {
    UInt8 buf[24 + 64]{};
    if (bytes > 64) return ~0U;
    const UInt32 hdr[6] = {client, object, cmd, 0, bytes, 0};
    __builtin_memcpy(buf, hdr, sizeof(hdr));
    __builtin_memcpy(buf + 24, params, bytes);
    UInt8 reply[256];
    UInt32 replyBytes = sizeof(reply), result = ~0U, st = ~0U;
    if (self->userRpc(76, buf, 24 + bytes, reply, &replyBytes, &result) == kIOReturnSuccess &&
        replyBytes >= 96)
        __builtin_memcpy(&st, reply + 92, 4);
    return st;
}

IOReturn NVGspControl::srSuspend() {
    srStep_ = 1;
    setProperty("NVGspControl-sr-step", srStep_, 32);
    if (!srStageBuffers()) return kIOReturnNoMemory;
    const nvgsp::WprMeta *meta = gsp_.metadata();
    // 1. stop channel scheduling
    {
        const UInt8 p[1] = {1};
        const UInt32 st = srCtrl(this, internalClient_, internalSubdevice_, 0x20800ac3, p, 1);
        setProperty("NVGspControl-sr-fifo-off", st, 32);
    }
    srStep_ = 2;
    // 2. our own VRAM: display RAMIN (RAMHT, ctxdmas, core/window PBs)
    bool saved = true;
    for (UInt32 off = 0; saved && off < 0x10000; off += 4096)
        saved = vramAccess(dispInstOffset_ + off,
                           reinterpret_cast<UInt32 *>(dispSave_ + off), 1024, false) ==
                kIOReturnSuccess;
    setProperty("NVGspControl-sr-disp-saved", saved);
    srStep_ = 3;
    // 3. FBSR: memlist over fbsrBuf_ + FBSR_INIT
    {
        const UInt32 pages = static_cast<UInt32>(fbsrBuf_.size() / 4096);
        const UInt32 bytes = 56 + pages * 8;
        UInt8 *am = static_cast<UInt8 *>(IOMalloc(bytes));
        if (!am) return kIOReturnNoMemory;
        bzero(am, bytes);
        const UInt32 w[7] = {internalClient_, internalDevice_, kFbsrMemlist,
                             0x81 /* NV01_MEMORY_LIST_SYSTEM */,
                             0x40000010 /* NONCONTIGUOUS | PCI | NO_MAP */, 0, 0};
        __builtin_memcpy(am, w, sizeof(w));
        const uint64_t length = fbsrBuf_.size();
        __builtin_memcpy(am + 32, &length, 8);
        __builtin_memcpy(am + 40, &pages, 4);
        const UInt32 desc = pages << 16;   // idr 0, length = pages
        __builtin_memcpy(am + 48, &desc, 4);
        bool pagesOk = true;
        for (UInt32 i = 0; pagesOk && i < pages; ++i) {
            uint64_t a = 0;
            pagesOk = fbsrBuf_.busPage(i, &a);
            const uint64_t pfn = a >> 12;
            __builtin_memcpy(am + 56 + i * 8, &pfn, 8);
        }
        UInt32 result = ~0U;
        const IOReturn r = pagesOk ? userRpcLarge(4, am, bytes, &result) : kIOReturnBadArgument;
        IOFree(am, bytes);
        setProperty("NVGspControl-sr-memlist-kr", static_cast<UInt32>(r), 32);
        setProperty("NVGspControl-sr-memlist-result", result, 32);
        if (r != kIOReturnSuccess || result != 0) return kIOReturnIOError;
        UInt8 p[24]{};
        const UInt32 hc = internalClient_, hs = kFbsrMemlist;
        const uint64_t srAddr = srMeta_.busAddress();
        __builtin_memcpy(p, &hc, 4);
        __builtin_memcpy(p + 4, &hs, 4);
        p[8] = 0;   // bEnteringGcoffState
        __builtin_memcpy(p + 16, &srAddr, 8);
        const UInt32 st = srCtrl(this, internalClient_, internalSubdevice_, 0x20800ac2, p, 24);
        setProperty("NVGspControl-sr-fbsr-init", st, 32);
        UInt8 f[16]{};
        const UInt32 fw[3] = {internalClient_, internalDevice_, kFbsrMemlist};
        __builtin_memcpy(f, fw, sizeof(fw));
        UInt8 reply[256];
        UInt32 rb = sizeof(reply), fr = 0;
        userRpc(10, f, sizeof(f), reply, &rb, &fr);
        if (st != 0) return kIOReturnIOError;
    }
    srStep_ = 4;
    IOSleep(200);
    // 4. UNLOADING_GUEST_DRIVER {bInPMTransition 1, bGc6Entering 0, newLevel 3}
    {
        const UInt8 p[8] = {1, 0, 0, 0, 3, 0, 0, 0};
        UInt8 reply[256];
        UInt32 rb = sizeof(reply), result = ~0U;
        const IOReturn r = userRpc(47, p, sizeof(p), reply, &rb, &result);
        setProperty("NVGspControl-sr-unload-rpc", static_cast<UInt32>(r), 32);
        setProperty("NVGspControl-sr-unload-result", result, 32);
        UInt32 mbox = 0;
        for (UInt32 ms = 0; ms < 2000; ++ms) {
            if (peekBar0(0x110040, 1, &mbox) == kIOReturnSuccess && mbox == 0x80000000) break;
            IOSleep(1);
        }
        setProperty("NVGspControl-sr-gsp-mbox0", mbox, 32);
        if (mbox != 0x80000000) return kIOReturnTimeout;
    }
    lk(__LINE__);
    sleeping_ = true;   // GSP-RM is gone: fence every RPC path
    ulk();
    srStep_ = 5;
    // 5. GSP falcon reset + FWSEC-SB + SEC2 Booter Unload
    const IOReturn u = executeUnload();
    srStep_ = u == kIOReturnSuccess ? 6 : 0x80 | srStep_;
    setProperty("NVGspControl-sr-step", srStep_, 32);
    srSuspended_ = u == kIOReturnSuccess;
    (void)meta;
    return u;
}

IOReturn NVGspControl::executeUnload() {
    IODeviceMemory *bar0 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IOMemoryMap *map = bar0 ? bar0->map() : nullptr;
    if (!map || map->getLength() < kPromBase + kPromBytes) {
        if (map) map->release();
        return kIOReturnNoResources;
    }
    Bar0Io io{map};
    UInt8 *rom = static_cast<UInt8 *>(IOMalloc(kPromBytes));
    if (!rom) { map->release(); return kIOReturnNoMemory; }
    bool romRead = true;
    for (size_t i = 0; i < kPromBytes; i += 4) {
        UInt32 word = 0;
        if (!io.read(kPromBase + static_cast<uint32_t>(i), &word)) { romRead = false; break; }
        __builtin_memcpy(rom + i, &word, 4);
    }
    nvgsp::VbiosFwsecView vbios{};
    UInt32 gspFuse = 0, boot0 = 0;
    const bool vbiosOk = romRead && nvgsp::parseVbiosFwsec(rom, kPromBytes, &vbios) &&
        io.read(kBoot0, &boot0) && io.read(kGspUcode9Fuse, &gspFuse);
    const nvgsp::WprMeta *meta = gsp_.metadata();
    // GSP falcon reset, then FWSEC-SB (restore pre-OS apps; non-fatal).
    const bool gspReset = vbiosOk && meta &&
        resetPulse(io, 0x001103c0, 0x001100f4) &&
        io.write(0x00111668, 0) &&
        nvgsp::falconPoll(io, 0x00111668, 1, 1, 200000, 10) &&
        io.write(0x00110084, boot0);
    bool sbOk = false;
    UInt32 sbScratch = ~0U;
    if (gspReset) {
        nvgsp::FwsecStaging fwsec;
        if (fwsec.stage(vbios.fwsec, gspFuse & 0xff, meta->frtsOffset, nullptr,
                        vbios.dmaImageSize, 0x19)) {
            nvgsp::FwsecExecutionResult res{};
            nvgsp::executeFwsecFrts(io, vbios.fwsec, fwsec.busAddress(), fwsec.imageSize(),
                                    meta->frtsOffset, &res);
            sbScratch = res.frtsScratch;
            sbOk = (res.frtsScratch & 0xffff) == 0;
        }
    }
    // SEC2 reset + Booter Unload with the SR metadata.
    uint32_t bcr = 0;
    const bool sec2 = gspReset &&
        resetPulse(io, 0x008403c0, 0x008400f4) &&
        io.write(0x00841668, 0) &&
        nvgsp::sec2Poll(io, 0x00841668, 1, 1, 200000, 10) &&
        io.read(0x00841668, &bcr) &&
        io.write(0x00840084, boot0);
    nvgsp::BooterView view{
        nullptr, booterUnload_.imageSize(), nullptr, 0, 0,
        booterUnload_.patchLocation(), 0, booterUnload_.engineId(), booterUnload_.ucodeId(),
        booterUnload_.layout()};
    nvgsp::BooterExecutionResult res{};
    const bool unloadOk = sec2 && nvgsp::executeBooterLoad(
        io, view, booterUnload_.busAddress(), booterUnload_.imageSize(),
        srMeta_.busAddress(), &res);
    UInt32 wpr2Hi = ~0U;
    io.read(0x001fa828, &wpr2Hi);
    IOFree(rom, kPromBytes);
    map->release();
    setProperty("NVGspControl-sr-sb-ok", sbOk);
    setProperty("NVGspControl-sr-sb-scratch", sbScratch, 32);
    setProperty("NVGspControl-sr-booter-unload-ok", unloadOk);
    setProperty("NVGspControl-sr-booter-unload-mbox0", res.mailbox0, 32);
    setProperty("NVGspControl-sr-wpr2-hi-after", wpr2Hi, 32);
    return (unloadOk && wpr2Hi == 0) ? kIOReturnSuccess : kIOReturnIOError;
}

IOReturn NVGspControl::srResume() {
    if (!srSuspended_ || !pci_) return kIOReturnNotReady;
    init_.setSrArgs(true);
    IODeviceMemory *bar0 = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IOMemoryMap *map = bar0 ? bar0->map() : nullptr;
    if (!map) return kIOReturnNoResources;
    Bar0Io io{map};
    if ((pci_->configRead16(kIOPCIConfigCommand) & 4) == 0) pci_->setBusMasterEnable(true);
    UInt32 boot0 = 0;
    uint32_t bcr = 0;
    const bool sec2 = io.read(kBoot0, &boot0) &&
        resetPulse(io, 0x008403c0, 0x008400f4) &&
        io.write(0x00841668, 0) &&
        nvgsp::sec2Poll(io, 0x00841668, 1, 1, 200000, 10) &&
        io.read(0x00841668, &bcr) &&
        io.write(0x00840084, boot0);
    nvgsp::BooterView view{
        nullptr, booter_.imageSize(), nullptr, 0, 0,
        booter_.patchLocation(), 0, booter_.engineId(), booter_.ucodeId(),
        booter_.layout()};
    nvgsp::BooterExecutionResult res{};
    lk(__LINE__);
    initDone_ = false;
    ulk();
    const bool loadOk = sec2 && nvgsp::executeBooterLoad(
        io, view, booter_.busAddress(), booter_.imageSize(), srMeta_.busAddress(), &res);
    const bool osOk = loadOk && io.write(nvgsp::falcon::kBase + 0x80, gsp_.appVersion());
    uint32_t cpuctl = 0;
    bool active = false;
    for (unsigned i = 0; osOk && i < 200000; ++i) {
        if (io.read(0x00111388, &cpuctl) && (cpuctl & (1U << 7))) { active = true; break; }
        IODelay(10);
    }
    map->release();
    setProperty("NVGspControl-sr-load-ok", loadOk);
    setProperty("NVGspControl-sr-load-mbox0", res.mailbox0, 32);
    setProperty("NVGspControl-sr-riscv-active", active);
    if (!active) return kIOReturnIOError;
    // GSP-RM restores its queues; unfence and wait for INIT_DONE.
    lk(__LINE__);
    sleeping_ = false;
    ulk();
    setProperty("NVGspControl-sleeping", false);
    bool init = false;
    for (UInt32 ms = 0; ms < 20000 && !init; ms += 5) {
        pollStatus();
        lk(__LINE__);
        init = initDone_;
        ulk();
        if (!init) IOSleep(5);
    }
    setProperty("NVGspControl-sr-init-done", init);
    if (!init) return kIOReturnTimeout;
    // Our VRAM back, channel scheduling back on, rmargs back to cold boot.
    bool restored = true;
    for (UInt32 off = 0; restored && off < 0x10000; off += 4096)
        restored = vramAccess(dispInstOffset_ + off,
                              reinterpret_cast<UInt32 *>(dispSave_ + off), 1024, true) ==
                   kIOReturnSuccess;
    setProperty("NVGspControl-sr-disp-restored", restored);
    const UInt8 p[1] = {0};
    setProperty("NVGspControl-sr-fifo-on",
                srCtrl(this, internalClient_, internalSubdevice_, 0x20800ac3, p, 1), 32);
    // 0.100.1: GSP-RM restored its interrupt state; re-arm our MSI vectors.
    lk(__LINE__);
    if (wl_ && irq_) wl_->removeEventSource(irq_);
    OSSafeReleaseNULL(irq_);
    OSSafeReleaseNULL(wl_);
    intrArmed_ = false;
    const bool rearmed = armInterrupts(vec_);
    ulk();
    setProperty("NVGspControl-sr-msi-rearmed", rearmed);
    init_.setSrArgs(false);
    srSuspended_ = false;
    fbsrBuf_.release();   // RM restored its VRAM; a fresh buffer is allocated next time
    srBuffersReady_ = false;
    srData_.release(); srRadix_.release(); srMeta_.release();
    return kIOReturnSuccess;
}

// 0.104.0 (B3): GPU reset without a machine reboot, e.g. after an RC killed
// the GR channel. Clean GSP unload (the SR suspend path: FIFO off, FBSR,
// UNLOADING_GUEST_DRIVER, GSP reset, booter unload -> WPR2 down), then the
// proven cold path: reset kext state, re-stage, full boot (FWSEC FRTS,
// booter load), chain to phase 33, light modeset. Asynchronous: poll
// NVGspControl-gr-persistent / -gpu-reset-count.
IOReturn NVGspControl::gpuReset() {
    if (!lock_ || !pci_ || !resumeCall_ || resumeActive_) return kIOReturnBusy;
    lk(__LINE__);
    noteGpuEventLocked("reset start");                   // 0.170.0
    ulk();
    resetBusy_ = true;                                   // 0.164.0: before the generation moves
    // 0.175.0: user-space waits on this property. Leaving the last resume's
    // false value published makes NotReady fail immediately during reset.
    setProperty("NVGspControl-reset-busy", true);
    resetPrivateDiagnostic_ = gpuResets_ == 0 && postResetPrivateDiagnosticByNvram();
    clientChannelsTeardown();                            // 0.178.10: before the generation moves
    ++gpuResets_;
    setProperty("NVGspControl-gpu-reset-count", gpuResets_, 32);
    setProperty("NVGspControl-ownchannel-reset-diagnostic", resetPrivateDiagnostic_ && gpuResets_ == 1);
    setProperty("NVGspControl-ownchannel-reset-fallback", !privateChannelGenerationAllowed());
    setProperty("NVGspControl-ownchannel-auto", ownChannelAuto());
    // 0.176.0 (T3): keep the user heap. FBSR in srSuspend covers GSP-RM's VRAM only,
    // so every client object (WindowServer's layers, app surfaces) came back as
    // garbage and the desktop stayed black after the reset. Copy them out while the
    // copy engine still runs, put them back at resume end; S3 does the same.
    lk(__LINE__);
    if (!resetVramSaved_) resetVramSaved_ = saveVramLocked(false, true);   // a retry keeps the first copy
    ulk();
    setProperty("NVGspControl-reset-vram-saved", resetVramSaved_);
    setProperty("NVGspControl-reset-vram-bytes", vramSavedBytes_, 64);
    const IOReturn s = srSuspend();
    setProperty("NVGspControl-gpu-reset-unload", static_cast<UInt32>(s), 32);
    // 0.168.0: an unload that fails (0xe00002ca on the third reset, 1 Oct
    // 04:57) used to end the recovery with the GPU half down and
    // WindowServer waiting until the watchdog panicked. The cold path below
    // re-stages and boots GSP from scratch, as after S3; try it anyway.
    if (s != kIOReturnSuccess) {
        ++unloadFailures_;
        setProperty("NVGspControl-gpu-reset-unload-failures", unloadFailures_, 32);
    }
    srSuspended_ = false;
    lk(__LINE__);
    sleeping_ = false;
    grStall_ = RingStall{};                              // 0.164.0: fresh channels
    ceStall_ = RingStall{};
    setProperty("NVGspControl-gr-hung", false);
    setProperty("NVGspControl-ce-hung", false);
    ulk();
    resetForResume();
    resetDriven_ = true;
    thread_call_enter(resumeCall_);
    return kIOReturnSuccess;
}

// 0.115.0 (B3): automatic recovery. After an RC kills our GR channel, and
// if NVRAM nvgsp-autoreset=1, reset the GPU from a thread call 1 s later
// (outside lock_; gpuReset is the same path as `nvrun --reset`). Clients
// from before the reset get kIOReturnOffline (reset generation), so apps
// see VK_ERROR_DEVICE_LOST and new ones find a working GPU. At most one
// automatic reset per 30 s so a GPU that faults on every boot cannot loop.
static bool autoResetEnabledByNvram() {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    bool on = false;
    if (options) {
        OSObject *o = options->getProperty("nvgsp-autoreset");
        if (OSData *d = OSDynamicCast(OSData, o))
            on = d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '1';
        else if (OSString *str = OSDynamicCast(OSString, o))
            on = str->isEqualTo("1");
        options->release();
    }
    return on;
}

void NVGspControl::scheduleAutoResetLocked() {
    if (!autoResetCall_ || resumeActive_ || !autoResetEnabledByNvram()) return;
    UInt64 now = 0, nowNs = 0;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &nowNs);
    // 0.164.0: a fault inside the 30 s window used to be dropped, and the
    // GPU then stayed down for good (a client faulting right after its
    // recovery). It waits for the window instead; three resets in a row
    // without two quiet minutes between them and we stop trying.
    if (lastAutoResetNs_ > nowNs) return;                   // one is already on its way
    if (lastAutoResetNs_ && nowNs - lastAutoResetNs_ > 120ULL * 1000000000ULL) autoResetStreak_ = 0;
    UInt64 delayNs = 1000000000ULL;
    if (autoResetStreak_ >= 3) {
        // the streak is spent: one more try after two quiet minutes, never
        // "down until someone reboots"
        setProperty("NVGspControl-autoreset-skipped", ++autoResetsSkipped_, 32);
        delayNs = 120ULL * 1000000000ULL;
        autoResetStreak_ = 0;
    } else if (lastAutoResetNs_ && nowNs - lastAutoResetNs_ < 30ULL * 1000000000ULL) {
        delayNs = 30ULL * 1000000000ULL - (nowNs - lastAutoResetNs_);
    }
    ++autoResetStreak_;
    lastAutoResetNs_ = nowNs + delayNs;
    setProperty("NVGspControl-autoreset-streak", autoResetStreak_, 32);
    UInt64 deadline = 0;
    clock_interval_to_deadline(static_cast<UInt32>(delayNs / 1000000ULL), kMillisecondScale, &deadline);
    thread_call_enter_delayed(autoResetCall_, deadline);
}

void NVGspControl::autoResetCallout(thread_call_param_t self, thread_call_param_t) {
    NVGspControl *d = static_cast<NVGspControl *>(self);
    const IOReturn r = d->gpuReset();
    d->setProperty("NVGspControl-autoreset-count", ++d->autoResets_, 32);
    d->setProperty("NVGspControl-autoreset-last-result", static_cast<UInt32>(r), 32);
}

IOReturn NVGspControl::srCycle() {
    const IOReturn s = srSuspend();
    setProperty("NVGspControl-sr-cycle-suspend", static_cast<UInt32>(s), 32);
    if (s != kIOReturnSuccess) return s;
    IOSleep(500);
    const IOReturn r = srResume();
    setProperty("NVGspControl-sr-cycle-resume", static_cast<UInt32>(r), 32);
    if (r != kIOReturnSuccess) return r;
    UInt32 code = ~0U;
    modesetHead0(&code);
    setProperty("NVGspControl-sr-cycle-modeset", code, 32);
    return kIOReturnSuccess;
}

#undef super

// 0.178.0 per-client GR channels (T18, docs/GPU-PERCHANNEL-PLAN-20261001.md).
// Proven first by tools/nvgsp_channel_probe.cpp --compact-ctx (2 Oct): a GR
// channel on its own FERMI_VASPACE_A whose PD1 points at the production PD0
// tables for the kernel ranges, with a private 2 MiB MAIN/PATCH context and
// the stock channel's global buffers. Here PD1[320..383] point at the owner's
// own arena tables for good, so its GR work never needs an arena switch and
// does not drain anybody else. CE and video work stays on the shared channels.
namespace {
constexpr UInt32 kCcClient = 0xc0d00001, kCcDevice = 0xc0d00080, kCcSubdev = 0xc0d02080;
constexpr UInt32 kCcHandle = 0xc0d30000;     // | slot << 8 | object
constexpr UInt32 kCcFirstChid = 9;           // GR 3, CE 4, video 5..7, kce 8
constexpr UInt64 kCcVramVa = 0x2000000000ULL;
constexpr UInt64 kCcGlobalVa = 0x104000000ULL;
constexpr UInt64 kCcRmVaBase = 0x800000000000ULL, kCcRmVaEnd = 0x1000000000000ULL;
constexpr UInt64 kCcPd3Off = 0x2000, kCcPd2Off = 0x3000, kCcPd1Off = 0x4000;   // in the ring part
constexpr UInt64 kCcBlockBytes = 0x400000;   // ring part + 2 MiB-aligned context
constexpr UInt64 kCcRingBytes = 0x80000;     // GPFIFO +0, semaphore +0x1000, PB +0x10000
constexpr UInt64 kCcPbOff = 0x10000, kCcPbBytes = 0x60000, kCcSemOff = 0x1000;
enum : UInt32 { kCcVas = 1, kCcVirt = 2, kCcBack0 = 4, kCcChan = 32, kCcSched = 64,
                kCcPd1 = 128, kCcPdSet = 256, kCcLeaked = 0x80000000 };
inline UInt64 ccRing(const UInt64 block, const UInt64 ctx) {
    return ctx - block >= kCcRingBytes ? block : ctx + nvgsp::kGrPrivateBytes;
}
}

static bool ctaPreemptByNvram() {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    bool on = true;
    if (options) {
        OSObject *o = options->copyProperty("nvgsp-cta");
        if (OSData *d = OSDynamicCast(OSData, o))
            on = !(d->getLength() >= 1 && static_cast<const char *>(d->getBytesNoCopy())[0] == '0');
        else if (OSString *str = OSDynamicCast(OSString, o))
            on = !str->isEqualTo("0");
        OSSafeReleaseNULL(o);
        options->release();
    }
    return on;
}

// Serialize whole RM channel lifecycles by default, while other GR channels
// keep executing. Individual RPC serialization does not protect a multi-RPC
// context allocation/free from another lifecycle transaction interleaving.
// NVRAM: 0 restores overlap for diagnosis; 1 also drains/pauses GR; 2 is default.
static UInt32 cchanQuiesceByNvram() {
    IORegistryEntry *options = IORegistryEntry::fromPath("/options", gIODTPlane);
    UInt32 mode = 2;
    if (options) {
        OSObject *o = options->copyProperty("nvgsp-cchanquiesce");
        const char *s = nullptr;
        if (OSData *d = OSDynamicCast(OSData, o)) {
            if (d->getLength() >= 1) s = static_cast<const char *>(d->getBytesNoCopy());
        } else if (OSString *str = OSDynamicCast(OSString, o)) s = str->getCStringNoCopy();
        if (s && s[0] >= '0' && s[0] <= '2') mode = s[0] - '0';
        OSSafeReleaseNULL(o);
        options->release();
    }
    return mode;
}

bool NVGspControl::waitChannelMutationLocked() {
    const UInt32 gen = gpuResets_;
    for (UInt32 ms = 0; cchanMutationBusy_ && ms < 6000; ++ms) {
        ulk(); IOSleep(1); lk(__LINE__);
    }
    return !cchanMutationBusy_ && gen == gpuResets_;
}

bool NVGspControl::waitChannelLifecycleLocked() {
    const UInt32 gen = gpuResets_;
    for (UInt32 ms = 0; cchanLifecycleBusy_ && ms < 6000; ++ms) {
        ulk(); IOSleep(1); lk(__LINE__);
    }
    return !cchanLifecycleBusy_ && gen == gpuResets_;
}

bool NVGspControl::drainClientChannelsLocked() {
    if (!drainEnginesLocked()) return false;
    for (UInt32 i = 0; i < kMaxClientChannels; ++i) {
        Ring r{};
        if (clientRingLocked(&cchan_[i], &r) && !waitRingSem(r, *r.seq, 2000000)) return false;
    }
    stampAdvanceLocked();
    return true;
}

NVGspControl::ClientChannel *NVGspControl::clientChannelLocked(const void *owner) {
    if (!owner) return nullptr;
    for (UInt32 i = 0; i < kMaxClientChannels; ++i)
        if (cchan_[i].state == 2 && cchan_[i].owner == owner && cchan_[i].gen == gpuResets_)
            return &cchan_[i];
    return nullptr;
}

bool NVGspControl::anyClientChannelLocked() const {
    for (UInt32 i = 0; i < kMaxClientChannels; ++i)
        if (cchan_[i].state && !(cchan_[i].made & kCcLeaked)) return true;
    return false;
}

// Caller holds lock_. Writes queued GR stamps whose submission has finished,
// oldest first; stops at the first one still running. A channel that is gone
// (closed after its work drained) no longer holds the queue up.
bool NVGspControl::stampAdvanceLocked() {
    bool moved = false;
    while (stampQHead_ != stampQTail_ && grPersistent_ && ctxBackingOffset_) {
        const PendingStamp &q = stampQ_[stampQHead_ % kStampQ];
        Ring r{};
        bool have = false;
        if (q.slot == 0xffff) {
            have = grRing(&r);
        } else if (q.slot < kMaxClientChannels && cchan_[q.slot].serial != q.serial) {
            // The old channel drained/was freed before this slot was reused.
            // Its completed work must not wait for the new channel's seq.
        } else if (q.slot < kMaxClientChannels && cchan_[q.slot].state == 1 && cchan_[q.slot].memHandle) {
            ClientChannel tmp = cchan_[q.slot];   // closing: its ring still tells
            tmp.state = 2;
            Ring t{};
            if (clientRingLocked(&tmp, &t)) { r = t; r.pbOff = &cchan_[q.slot].pbOff; r.seq = &cchan_[q.slot].seq; have = true; }
        } else if (q.slot < kMaxClientChannels) {
            have = clientRingLocked(&cchan_[q.slot], &r);
        }
        UInt32 v = 0;
        if (have && (!readRingSem(r, &v) || static_cast<SInt32>(v - q.seq) < 0)) break;
        const UInt32 value = static_cast<SInt32>(q.value - stampWritten_) > 0 ? q.value : stampWritten_;
        if (!ringWrite(ctxBackingOffset_ + kStampCtxOff + q.offset, &value, 1)) break;
        stampWritten_ = value;
        ++stampQHead_;
        moved = true;
    }
    if (moved) stampDeliverMask_ |= 1U;
    return moved;
}

bool NVGspControl::clientRingLocked(ClientChannel *c, Ring *out) {
    if (!c || c->state != 2) return false;
    const UInt64 ring = ccRing(c->block, c->ctx);
    *out = Ring{ring + kCcPbOff, kCcVramVa + ring + kCcPbOff, kCcPbBytes,
                ring + kCcSemOff, kCcVramVa + ring + kCcSemOff, ring, c->userd,
                c->token, &c->pbOff, &c->seq};
    return true;
}

bool NVGspControl::grRingFor(const void *owner, Ring *out) {
    ClientChannel *c = clientChannelLocked(owner);
    return c ? clientRingLocked(c, out) : grRing(out);
}

// Caller holds lock_. Points PD1[320..383] of the private VAS at the owner's
// arena tables once they exist (they never move while the arena lives).
IOReturn NVGspControl::clientArenaLocked(ClientChannel *c) {
    if (vramFrozen_) {
        const UInt32 gen = gpuResets_;
        waitThawLocked();
        if (gen != gpuResets_ || c->state != 2) return kIOReturnNotReady;
    }
    ArenaCtx *a = arenaForLocked(c->owner, false);
    if (!a || !a->ready) {
        ++nullArenaRefusals_;
        setProperty("NVGspControl-null-arena-refusals", nullArenaRefusals_, 32);
        return kIOReturnNotReady;
    }
    if (c->arenaTables == a->map.tables) return kIOReturnSuccess;
    PraminPteResult r{};
    bool ok = true;
    for (UInt32 slot = 0; ok && slot < 64; ++slot) {
        UInt64 pde = (((a->map.tables + slot * 4096ULL) >> 12) << 8) | 2;
        ok = praminPteAccess(pci_, c->pd1 + (320 + slot) * 8ULL, &pde, true, true, &r);
    }
    c->pd1Written[5] = ~0ULL;   // PD1[320..383]
    if (!ok) return kIOReturnIOError;
    c->arenaTables = a->map.tables;
    return tlbInvalidate();
}

void NVGspControl::clientChannelsDropLocked() {
    stampQHead_ = stampQTail_ = 0;   // stamps restart with the new GSP/VAS
    stampWritten_ = stampQueued_ = 0;
    for (UInt32 i = 0; i < kMaxClientChannels; ++i) {
        // a channel RM may still hold keeps its memory (and slot) for good
        if (cchan_[i].made & kCcLeaked) continue;
        if (cchan_[i].memHandle) releaseGpuMemLocked(cchan_[i].memHandle - 1);
        cchan_[i] = ClientChannel{};
    }
}

IOReturn NVGspControl::clientChannelOpen(const void *owner, UInt32 *chidOut) {
    if (!lock_ || !pci_ || !owner || !chidOut) return kIOReturnBadArgument;
    lk(__LINE__);
    // The same generation policy gates explicit selector40 and automatic
    // opens. Normal post-reset fallback cannot be bypassed by a Metal opt-in;
    // the NVRAM diagnostic is limited to the first global reset.
    if (!privateChannelGenerationAllowed()) { ulk(); return kIOReturnUnsupported; }
    if (!waitChannelLifecycleLocked()) { ulk(); return kIOReturnNotReady; }
    if (!grPersistent_ || postInitPhase_ != 33 || !pdbAddress_ || resetBusy_) { ulk(); return kIOReturnNotReady; }
    UInt32 slot = kMaxClientChannels;
    for (UInt32 i = 0; i < kMaxClientChannels; ++i) {
        if (cchan_[i].state && cchan_[i].owner == owner && cchan_[i].gen == gpuResets_) {
            const IOReturn r = cchan_[i].state == 2 ? kIOReturnSuccess : kIOReturnBusy;
            *chidOut = kCcFirstChid + i;
            ulk();
            return r;
        }
        if (!cchan_[i].state && slot == kMaxClientChannels) slot = i;
    }
    if (slot == kMaxClientChannels) { ulk(); return kIOReturnNoResources; }
    const UInt32 mode = cchanQuiesceByNvram();
    const bool quiesce = mode == 1;
    if (quiesce && !drainClientChannelsLocked()) { ulk(); return kIOReturnTimeout; }
    if (mode) { cchanLifecycleBusy_ = true; cchanMutationBusy_ = quiesce; }
    ClientChannel &c = cchan_[slot];
    c = ClientChannel{};
    c.owner = owner;
    c.state = 1;
    c.gen = gpuResets_;
    c.serial = ++cchanSerial_;
    UInt64 block = 0;
    bool ok = memAllocLocked(&cchan_[slot], kCcBlockBytes, 0, &c.memHandle, &block) == kIOReturnSuccess;
    c.block = block;
    c.ctx = (block + 0x1fffff) & ~0x1fffffULL;
    const UInt64 ring = ccRing(c.block, c.ctx);
    ok = ok && praminZeroRange(pci_, ring, kCcPbOff) &&
         praminZeroRange(pci_, c.ctx, nvgsp::kGrPrivateBytes);
    const UInt64 prodPdb = pdbAddress_;
    ulk();

    constexpr UInt32 kReplyCap = 1024, kReqCap = 1024;
    UInt8 *reply = static_cast<UInt8 *>(IOMalloc(kReplyCap + kReqCap));
    UInt8 *req = reply ? reply + kReplyCap : nullptr;
    ok = ok && reply;
    const UInt32 h = kCcHandle | (slot << 8), chid = kCcFirstChid + slot;
    UInt32 stage = 0, status = 0, rb = 0;
    auto call = [&](UInt32 fn, UInt32 bytes, UInt32 statusAt, UInt32 step) -> bool {
        UInt32 result = ~0U, st = ~0U;
        rb = kReplyCap;
        stage = step;
        const IOReturn r = userRpc(fn, req, bytes, reply, &rb, &result);
        if (r == kIOReturnSuccess && rb >= statusAt + 4) __builtin_memcpy(&st, reply + statusAt, 4);
        status = r != kIOReturnSuccess ? static_cast<UInt32>(r) : result ? result : st;
        return r == kIOReturnSuccess && result == 0 && st == 0;
    };
    auto control = [&](UInt32 hObject, UInt32 cmd, UInt32 bytes) {
        bzero(req, 24 + bytes);
        const UInt32 hdr[6] = {kCcClient, hObject, cmd, 0, bytes, 0};
        __builtin_memcpy(req, hdr, sizeof(hdr));
        return req + 24;
    };
    auto alloc = [&](UInt32 parent, UInt32 handle, UInt32 cls, UInt32 bytes) {
        bzero(req, 32 + bytes);
        const UInt32 hdr[8] = {kCcClient, parent, handle, cls, 0, bytes, 0, 0};
        __builtin_memcpy(req, hdr, sizeof(hdr));
        return req + 32;
    };
    auto put32 = [](UInt8 *p, UInt32 v) { __builtin_memcpy(p, &v, 4); };
    auto put64 = [](UInt8 *p, UInt64 v) { __builtin_memcpy(p, &v, 8); };
    // 1: VAS, 2: a 4 KiB virtual range in it (RM then builds the PDB/PD2/PD1
    // the channel needs; without it the channel ALLOC is INVALID_STATE)
    if (ok) {
        // 0.178.14: SHARED_MANAGEMENT. RM manages only [2^47, 2^48) (whole
        // top-level PDEs) and never walks PD3[0]; our own directory goes in
        // with RPC 54 SET_PAGE_DIRECTORY (step 5b). In an RM-managed VAS RM
        // placed its mappings among the PD1 entries we share with production
        // and wrote into the production PD0 tables (all channels faulted at
        // VA 0x2046400000, 2 Oct 03:15).
        UInt8 *p = alloc(kCcDevice, h | 0xf1, 0x90f1, 48);
        put32(p + 4, 4);                       // NV_VASPACE_ALLOCATION_FLAGS_SHARED_MANAGEMENT
        put64(p + 8, kCcRmVaEnd); put64(p + 40, kCcRmVaBase);
        ok = call(103, 32 + 48, 96, 1);
        if (ok) c.made |= kCcVas;
    }
    if (ok) {
        UInt8 *p = alloc(kCcDevice, h | 0x50, 0x50a0, 128);
        put32(p, kCcClient); put32(p + 8, 0x08080100); put32(p + 24, 0x00800000);
        put64(p + 64, 4096); put64(p + 72, 4096); put32(p + 108, h | 0xf1);
        ok = call(103, 32 + 128, 96, 2);
        if (ok) c.made |= kCcVirt;
    }
    // 3..5: USERD, instance, method buffer
    for (UInt32 i = 0; ok && i < 3; ++i) {
        UInt8 *p = alloc(kCcSubdev, h | (0x43 + i), 0x40, 128);
        const UInt64 bytes = i == 2
            ? UInt64((methodBufferBytes_ ? methodBufferBytes_ : 20480) + 0xfff) & ~0xfffULL : 4096;
        put32(p, kCcClient); put32(p + 4, 6); put32(p + 24, 0x10800000); put64(p + 64, bytes);
        ok = call(103, 32 + 128, 96, 3 + i);
        if (ok) c.made |= kCcBack0 << i;
        UInt32 pb = 0;
        UInt64 off = 0;
        if (ok && rb >= 112 + 88) {
            __builtin_memcpy(&pb, reply + 100, 4);
            if (pb == 128) __builtin_memcpy(&off, reply + 112 + 80, 8);
        }
        ok = ok && off;
        (i == 0 ? c.userd : i == 1 ? c.inst : c.mthd) = off;
    }
    if (ok) {
        lk(__LINE__);
        ok = praminZeroRange(pci_, c.userd, 4096) && praminZeroRange(pci_, c.inst, 4096);
        ulk();
    }
    // 5b: our page directory (PD3/PD2/PD1 in the ring part). PD1 shares every
    // production PD1 entry outside the client arenas (same PD0 tables);
    // PD1[320..383] get the owner's arena tables at its first submission.
    if (ok) {
        stage = 5;
        lk(__LINE__);
        PraminPteResult r{};
        UInt64 e3 = 0, e2 = 0;
        const UInt64 pd3 = ring + kCcPd3Off, pd2 = ring + kCcPd2Off, pd1 = ring + kCcPd1Off;
        ok = praminZeroRange(pci_, pd3, 0x3000) &&
             praminPteAccess(pci_, prodPdb, &e3, false, false, &r) && e3 &&
             praminPteAccess(pci_, ((e3 >> 8) & ((1ULL << 46) - 1)) << 12, &e2, false, false, &r) && e2;
        const UInt64 prodPd1 = ((e2 >> 8) & ((1ULL << 46) - 1)) << 12;
        for (UInt32 i = 0; ok && i < 512; ++i) {
            if (i >= 320 && i < 384) continue;
            UInt64 v = 0;
            ok = praminPteAccess(pci_, prodPd1 + i * 8ULL, &v, false, false, &r) &&
                 (!v || praminPteAccess(pci_, pd1 + i * 8ULL, &v, true, true, &r));
        }
        UInt64 d2 = (e2 & 0xff) | ((pd1 >> 12) << 8), d3 = (e3 & 0xff) | ((pd2 >> 12) << 8);
        ok = ok && praminPteAccess(pci_, pd2, &d2, true, true, &r) &&
             praminPteAccess(pci_, pd3, &d3, true, true, &r);
        if (ok) c.pd1 = pd1;
        ulk();
        if (ok) {
            // rpc_set_page_directory_v1E_05 {hClient, hDevice, pasid,
            // {physAddress, numEntries, flags (vidmem), hVASpace, chId, subDeviceId, pasid}}
            bzero(req, 48);
            put32(req, kCcClient); put32(req + 4, kCcDevice);
            put64(req + 16, pd3); put32(req + 24, 4); put32(req + 32, h | 0xf1);
            UInt32 result = ~0U;
            rb = kReplyCap;
            stage = 5;
            const IOReturn rr = userRpc(54, req, 48, reply, &rb, &result);
            ok = rr == kIOReturnSuccess && result == 0;
            status = rr ? static_cast<UInt32>(rr) : result;
            if (ok) c.made |= kCcPdSet;
        }
        if (!ok && !status) status = 0xffff0005;
    }
    // 6: GR channel on the private VAS, 7: bind, 8: schedule, 9: token
    if (ok) {
        nvgsp::NvChannelAllocParams chan{};
        ok = nvgsp::buildChannelAllocParams(h | 0xf1, h | 0x43, kCcVramVa + ring, 512,
                                            nvgsp::kEngineTypeGraphics, &chan);
        chan.hObjectError = 0;
        chan.flags = 0x00200000u | ((chid & 7) << 8) | (((chid >> 3) & 0x1ff) << 12);
        chan.internalFlags = nvgsp::kChannelInternalFlagsNotifierNone;
        chan.instanceMem = {c.inst, 4096, 2, 1};
        chan.ramfcMem = {c.inst, 512, 2, 1};
        chan.userdMem = {c.userd, 512, 2, 1};
        chan.mthdbufMem = {c.mthd, methodBufferBytes_ ? methodBufferBytes_ : 20480, 2, 0};
        UInt8 *p = alloc(kCcDevice, h | 0x6f, nvgsp::kAmpereChannelGpfifoA, sizeof(chan));
        __builtin_memcpy(p, &chan, sizeof(chan));
        ok = ok && call(103, 32 + sizeof(chan), 96, 6);
        if (ok) c.made |= kCcChan;
    }
    if (ok) {
        UInt8 *p = control(h | 0x6f, 0xa06f0104, 4);
        put32(p, nvgsp::kEngineTypeGraphics);
        ok = call(76, 28, 92, 7);
    }
    if (ok) {
        control(h | 0x6f, 0xa06f0103, 2)[0] = 1;
        ok = call(76, 26, 92, 8);
        if (ok) c.made |= kCcSched;
    }
    if (ok) {
        control(h | 0x6f, 0xc36f0108, 4);
        ok = call(76, 28, 92, 9) && rb >= 108;
        if (ok) __builtin_memcpy(&c.token, reply + 104, 4);
        if (ok && (c.token & 0xffff) != chid) { ok = false; status = 0xffff0009; }
    }
    // 10: the channel walks our directory
    if (ok) {
        stage = 10;
        lk(__LINE__);
        PraminPteResult r{};
        UInt64 e = 0;
        ok = praminPteAccess(pci_, c.inst + 0x200, &e, false, false, &r) &&
             ((e & 0xffffffff00000000ULL) | (e & 0xfffff000ULL)) == ring + kCcPd3Off;
        // stale translations of an earlier VAS that used this memory
        ok = ok && tlbInvalidate() == kIOReturnSuccess;
        ulk();
        if (!ok) status = 0xffff000a;
    }
    // 11: private MAIN/PATCH, shared global buffers
    if (ok) {
        UInt8 *p = control(kCcSubdev, 0x2080012b, nvgsp::kGrPromoteBytes);
        ok = nvgsp::buildGrPromoteCompact(kCcClient, h | 0x6f, c.ctx, kCcVramVa + c.ctx, kCcGlobalVa,
                                          p, nvgsp::kGrPromoteBytes) &&
             call(76, 24 + nvgsp::kGrPromoteBytes, 92, 11);
    }
    // 11b (0.178.11): compute preemption CTA. The promoted context reports WFI
    // for both; with several GR channels a long dispatch then holds the
    // engine until the context-switch timeout (RC 109). NVRAM nvgsp-cta=0 keeps WFI.
    if (ok && ctaPreemptByNvram()) {
        UInt8 *p = control(kCcSubdev, 0x20801210, 32);
        put32(p, 1); put32(p + 4, h | 0x6f); put32(p + 8, 0); put32(p + 12, 1);
        if (!call(76, 24 + 32, 92, 14)) { setProperty("NVGspControl-cchan-cta-fail", status, 32); status = 0; }
    }
    // 12: ADA_A, ADA_COMPUTE_A, FERMI_TWOD_A (freed with the channel)
    static const UInt32 kCcClasses[3] = {0xc997, 0xc9c0, 0x902d};
    for (UInt32 i = 0; ok && i < 3; ++i) {
        alloc(h | 0x6f, h | (kCcClasses[i] & 0xff), kCcClasses[i], 0);
        ok = call(103, 32, 96, 12);
        if (!ok) {   // 0.178.16: which class, raw RPC result and RM status, reply size
            UInt32 st = ~0U, res = ~0U, priv = ~0U;   // entry header 48 + RPC header {.., result +16, private +20}
            if (rb >= 100) __builtin_memcpy(&st, reply + 96, 4);
            if (rb >= 72) { __builtin_memcpy(&res, reply + 64, 4); __builtin_memcpy(&priv, reply + 68, 4); }
            UInt32 v[5] = {kCcClasses[i], res, priv, st, rb};
            setProperty("NVGspControl-cchan-obj-fail", v, sizeof(v));
        }
    }
    // 13: first work: bind subchannels 0/1 and fence (proves execution)
    lk(__LINE__);
    if (ok && c.gen != gpuResets_) { ok = false; status = 0xffff000c; }
    if (ok && c.closeAsked) { ok = false; status = 0xffff000f; }
    // 0.178.10: work this client sent to the shared channels before (a
    // retried open) must finish before its GR work moves here
    if (ok && !drainEnginesLocked()) { ok = false; status = 0xffff000e; }
    if (ok) {
        c.state = 2;
        Ring rg{};
        UInt64 ns = 0;
            // compute shared/local memory windows as NVMTL sets them: a batch
        // that starts with a draw never sets them before its first launch,
        // and a fresh context has them overlapping (SKEDCHECK17, RC 13)
        static const UInt32 kInit[10] = {0x20010000, 0xc997, 0x20012000, 0xc9c0,
                                         0x200220a8, 0, 0xfe000000, 0x200221ec, 0, 0xff000000};
        stage = 13;
        ok = clientRingLocked(&c, &rg) && submitRing(rg, kInit, 10, &ns) == kIOReturnSuccess;
        if (!ok) { c.state = 1; status = 0xffff000d; }
    }
    ulk();
    if (!ok) {
        // Undo in reverse; the channel takes its objects and context with
        // it. A free that does not answer leaves the slot (and its memory)
        // held until the next reset rather than reusing live backing.
        const UInt32 failStage = stage, failStatus = status;
        bool clean = true;
        if (c.made & kCcSched) {
            control(h | 0x6f, 0xa06f0103, 2)[0] = 0;
            clean = call(76, 26, 92, 20);
        }
        const UInt32 frees[6][2] = {{kCcChan, kCcDevice}, {kCcBack0 << 2, kCcSubdev},
                                    {kCcBack0 << 1, kCcSubdev}, {kCcBack0, kCcSubdev},
                                    {kCcVirt, kCcDevice}, {kCcVas, kCcDevice}};
        const UInt32 objs[6] = {0x6f, 0x45, 0x44, 0x43, 0x50, 0xf1};
        for (UInt32 i = 0; clean && i < 6; ++i) {
            if (!(c.made & frees[i][0])) continue;
            if (frees[i][0] == kCcVirt && (c.made & kCcPdSet)) {
                // RPC 79 UNSET_PAGE_DIRECTORY: RM takes the directory back
                // before the VAS goes (rpc_unset_page_directory_v1E_05)
                bzero(req, 24);
                put32(req, kCcClient); put32(req + 4, kCcDevice); put32(req + 8, h | 0xf1);
                UInt32 result = ~0U;
                rb = kReplyCap;
                clean = userRpc(79, req, 24, reply, &rb, &result) == kIOReturnSuccess && result == 0;
                if (!clean) break;
            }
            const UInt32 f[4] = {kCcClient, frees[i][1], h | objs[i], 0};
            bzero(req, 16);
            __builtin_memcpy(req, f, sizeof(f));
            clean = call(10, 16, 92, 21);
        }
        lk(__LINE__);
        if (c.gen == gpuResets_) {
            if (clean) {
                if (c.memHandle) releaseGpuMemLocked(c.memHandle - 1);
                c = ClientChannel{};
            } else {
                c.made |= kCcLeaked;
                c.owner = nullptr;
            }
        }
        ++cchanFails_;
        setProperty("NVGspControl-cchan-fails", cchanFails_, 32);
        setProperty("NVGspControl-cchan-last-fail", (UInt64(failStage) << 32) | failStatus, 64);
        ulk();
    } else {
        lk(__LINE__);
        ++cchanOpens_;
        setProperty("NVGspControl-cchan-opens", cchanOpens_, 32);
        ulk();
    }
    if (reply) IOFree(reply, kReplyCap + kReqCap);
    if (mode) { lk(__LINE__); cchanMutationBusy_ = cchanLifecycleBusy_ = false; ulk(); }
    *chidOut = chid;
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

// Frees a client channel's RM objects (channel with its context/objects,
// backings, virtual range, VAS) and puts its PD1 entries back. Runs without
// lock_. false: a free did not answer; the caller keeps the memory.
bool NVGspControl::clientChannelFreeRm(UInt32 slot, UInt32 made, UInt32 *failOut) {
    UInt8 req[32]{}, reply[256]{};
    const UInt32 h = kCcHandle | (slot << 8);
    UInt32 step = 0;
    auto rpcOk = [&](UInt32 fn, UInt32 bytes, UInt32 statusAt) {
        UInt32 rb = sizeof(reply), result = ~0U, st = ~0U;
        const IOReturn r = userRpc(fn, req, bytes, reply, &rb, &result);
        if (r == kIOReturnSuccess && rb >= statusAt + 4) __builtin_memcpy(&st, reply + statusAt, 4);
        const bool good = r == kIOReturnSuccess && result == 0 && st == 0;
        if (!good && failOut) *failOut = (step << 24) | ((r ? static_cast<UInt32>(r) : result ? result : st) & 0xffffff);
        return good;
    };
    bool clean = true;
    if (made & kCcSched) {
        const UInt32 hdr[6] = {kCcClient, h | 0x6f, 0xa06f0103, 0, 2, 0};
        __builtin_memcpy(req, hdr, sizeof(hdr));
        step = 1;
        clean = rpcOk(76, 26, 92);
    }
    const UInt32 objs[6] = {0x6f, 0x45, 0x44, 0x43, 0x50, 0xf1};
    const UInt32 need[6] = {kCcChan, kCcBack0 << 2, kCcBack0 << 1, kCcBack0, kCcVirt, kCcVas};
    for (UInt32 i = 0; clean && i < 6; ++i) {
        step = 2 + i;
        if (!(made & need[i])) continue;
        if (objs[i] == 0x50 && (made & kCcPdSet)) {   // 0.178.14: directory back to RM first
            const UInt32 u[3] = {kCcClient, kCcDevice, h | 0xf1};
            bzero(req, sizeof(req));
            __builtin_memcpy(req, u, sizeof(u));
            UInt32 rb = sizeof(reply), result = ~0U;
            const IOReturn r = userRpc(79, req, 24, reply, &rb, &result);
            clean = r == kIOReturnSuccess && result == 0;
            if (!clean) { if (failOut) *failOut = (0x79u << 24) | ((r ? static_cast<UInt32>(r) : result) & 0xffffff); break; }
        }
        const UInt32 f[4] = {kCcClient, objs[i] >= 0x43 && objs[i] <= 0x45 ? kCcSubdev : kCcDevice,
                             h | objs[i], 0};
        bzero(req, sizeof(req));
        __builtin_memcpy(req, f, sizeof(f));
        clean = rpcOk(10, 16, 92);
    }
    return clean;
}

void NVGspControl::clientChannelClose(const void *owner) {
    if (!lock_ || !owner) return;
    lk(__LINE__);
    if (!waitChannelLifecycleLocked()) { ulk(); return; }
    ClientChannel *c = clientChannelLocked(owner);
    if (!c) {
        // 0.178.16: an open still running for this owner frees the channel
        // itself when it finishes (it used to stay live with no owner)
        for (UInt32 i = 0; i < kMaxClientChannels; ++i)
            if (cchan_[i].state == 1 && cchan_[i].owner == owner && !(cchan_[i].made & kCcLeaked))
                cchan_[i].closeAsked = true;
        ulk();
        return;
    }
    const UInt32 mode = cchanQuiesceByNvram();
    const bool quiesce = mode == 1;
    if (quiesce) {
        const bool drained = drainClientChannelsLocked();
        setProperty("NVGspControl-cchan-quiesce-drained", drained);
    }
    if (mode) { cchanLifecycleBusy_ = true; cchanMutationBusy_ = quiesce; }
    Ring rg{};
    clientRingLocked(c, &rg);
    c->state = 1;   // no new submissions
    const UInt32 slot = static_cast<UInt32>(c - cchan_), gen = c->gen, want = c->seq;
    bool idle = false;
    for (UInt32 ms = 0; ms < 2000 && gen == gpuResets_; ++ms) {
        UInt32 v = 0;
        if (readRingSem(rg, &v) && static_cast<SInt32>(v - want) >= 0) { idle = true; break; }
        ulk();
        IOSleep(1);
        lk(__LINE__);
    }
    setProperty("NVGspControl-cchan-close-idle", idle);
    const ClientChannel snap = *c;
    ulk();
    if (gen != gpuResets_) {
        if (mode) { lk(__LINE__); cchanMutationBusy_ = cchanLifecycleBusy_ = false; ulk(); }
        return;   // the reset took it down
    }
    UInt32 fail = 0;
    const bool clean = clientChannelFreeRm(slot, snap.made, &fail);
    lk(__LINE__);
    if (gen == gpuResets_) {
        ClientChannel &cc = cchan_[slot];
        if (clean) {
            if (cc.memHandle) releaseGpuMemLocked(cc.memHandle - 1);
            cc = ClientChannel{};
        } else {
            cc.made |= kCcLeaked;
            cc.owner = nullptr;
            setProperty("NVGspControl-cchan-leaked-slot", slot, 32);
            setProperty("NVGspControl-cchan-close-fail", fail, 32);
        }
    }
    if (mode) cchanMutationBusy_ = cchanLifecycleBusy_ = false;
    ulk();
}

// 0.178.10: before a reset. FBSR suspend/resume keeps RM objects, so a channel
// left in RM came back after the reset pointing at memory we had released:
// its context restore timed out (RC 109) again and again and new channels
// collided with its handles (0x19). Free them in RM first; stop at the first
// free that does not answer (GSP hung) and keep those slots' memory.
void NVGspControl::clientChannelsTeardown() {
    if (!lock_) return;
    bool gspOk = true;
    UInt32 freed = 0;
    for (UInt32 slot = 0; slot < kMaxClientChannels; ++slot) {
        lk(__LINE__);
        ClientChannel &c = cchan_[slot];
        if (c.state != 2) { ulk(); continue; }
        c.state = 1;
        const ClientChannel snap = c;
        ulk();
        UInt32 fail = 0;
        const bool clean = gspOk &&
            clientChannelFreeRm(slot, snap.made, &fail);
        gspOk = clean;
        lk(__LINE__);
        if (clean) {
            if (c.memHandle) releaseGpuMemLocked(c.memHandle - 1);
            c = ClientChannel{};
            ++freed;
        } else {
            c.made |= kCcLeaked;
            c.owner = nullptr;
            setProperty("NVGspControl-cchan-reset-fail", fail, 32);
        }
        ulk();
    }
    setProperty("NVGspControl-cchan-reset-freed", freed, 32);
}
