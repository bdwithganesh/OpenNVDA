#pragma once

#include "NVGspArenaPages.hpp"
#include "NVGspResidency.hpp"

namespace nvgsp {

// Page tables of one client's user VA arena with 2 MiB and 64 KiB
// pages (Turing+ MMU v2, nouveau vmmgp100.c layout):
//   PD0 table = 256 dual entries of 16 bytes, one per 2 MiB region.
//     qword 0 = a 2 MiB PTE (VALID bit 0), or the PDE of a 64 KiB "LPT"
//               (address >> 4 | aperture VRAM 1 << 1), or 0;
//     qword 1 = small (4 KiB) page table PDE, always 0 here.
//   LPT = 32 x 8-byte PTEs (256 B, 256 B aligned). An unmapped LPT entry
//   holds VALID=0 + PRIV (bit 5) so the MMU does not look for small pages.
//   PTE = (phys >> 4) | flags: VALID 0, APERTURE 2:1, VOL 3, KIND 63:56.
// A region is either one 2 MiB PTE or an LPT; a 64 KiB-granular bind or
// unbind in a 2 MiB-mapped region first splits it into an LPT with the
// same translation. Ownership (object handle / raw) is tracked per 2 MiB
// region or per LPT entry, so a freed object can be unmapped exactly.
//
// Backend B (static dispatch, kernel- and host-safe):
//   bool read64(uint64_t vram, uint64_t *v); bool write64(uint64_t vram, uint64_t v);
//   bool zero(uint64_t vram, uint64_t bytes);
//   bool allocChunk(uint64_t *phys);            // 2 MiB of VRAM for LPTs
//   void *allocOwners(uint32_t bytes); void freeOwners(void *p, uint32_t bytes);
constexpr uint64_t kBigPageBytes = 0x10000;
constexpr uint32_t kLptEntries = 32;
constexpr uint32_t kLptBytes = kLptEntries * 8;
constexpr uint64_t kLptInvalid = 1ULL << 5;                 // PRIV, VALID=0
constexpr uint32_t kLptPerChunk = 0x200000 / kLptBytes;     // 8192
constexpr uint32_t kLptChunks = kArenaPages / kLptPerChunk; // 2: one LPT per region
constexpr uint64_t kPteAddrMask = 0x00FFFFFFFFFFFF00ULL;
constexpr uint64_t kArenaTablesBytes = 64 * 4096;
// 27 Sep: small (4 KiB) page tables for binding scattered host pages (user
// memory wired by the kext). A region in SPT mode has PD0 qword 0 = 0 (no big
// page table) and qword 1 = PDE of a 4 KiB table of 512 x 8-byte PTEs; it is
// never mixed with 2 MiB / LPT mappings. SPTs live in their own 2 MiB VRAM
// chunks (512 tables each).
constexpr uint64_t kSmallPageBytes = 0x1000;
constexpr uint32_t kSptEntries = 512;
constexpr uint32_t kSptBytes = kSptEntries * 8;
constexpr uint32_t kSptPerChunk = 0x200000 / kSptBytes;     // 512
constexpr uint32_t kSptChunks = 8;                          // up to 4096 regions (8 GiB)

inline uint64_t pteAddress(uint64_t pte) { return (pte & kPteAddrMask) << 4; }
inline uint64_t pdeVram(uint64_t table) { return (table >> 4) | (1ULL << 1); }

template <class B>
// Zero-initialise the object (bzero / value-init) before the first init().
class ArenaMap {
public:
    uint64_t tables = 0;                     // 64 contiguous PD0 tables
    uint16_t page[kArenaPages];              // 2 MiB-mode region owner
    uint16_t lpt[kArenaPages];               // 0 = 2 MiB mode, else LPT slot + 1
    uint64_t chunk[kLptChunks];
    uint16_t *owners[kLptChunks];            // per LPT entry
    uint32_t used[kArenaPages / 32];         // LPT slot bitmap
    uint32_t lptCount = 0;
    uint16_t spt[kArenaPages];               // 0 = none, else SPT slot + 1
    uint64_t sptChunk[kSptChunks];
    uint16_t *sptOwners[kSptChunks];         // per SPT entry
    uint32_t sptUsed[kSptChunks * kSptPerChunk / 32];
    uint32_t sptCount = 0;

    // Fresh (zeroed) tables for a new client or a rebuilt VAS.
    bool init(B &b, uint64_t tablesPhys) {
        release(b);
        tables = tablesPhys;
        return b.zero(tables, kArenaTablesBytes);
    }

    void release(B &b) {
        for (uint32_t c = 0; c < kLptChunks; ++c) {
            if (owners[c]) b.freeOwners(owners[c], kLptPerChunk * kLptEntries * 2);
            owners[c] = nullptr;
            chunk[c] = 0;
        }
        for (uint32_t c = 0; c < kSptChunks; ++c) {
            if (sptOwners[c]) b.freeOwners(sptOwners[c], kSptPerChunk * kSptEntries * 2);
            sptOwners[c] = nullptr;
            sptChunk[c] = 0;
        }
        for (uint32_t i = 0; i < kArenaPages; ++i) page[i] = lpt[i] = spt[i] = 0;
        for (uint32_t i = 0; i < kArenaPages / 32; ++i) used[i] = 0;
        for (uint32_t i = 0; i < kSptChunks * kSptPerChunk / 32; ++i) sptUsed[i] = 0;
        lptCount = sptCount = 0;
    }

    // Map [va, va + bytes) to phys (contiguous). All 64 KiB aligned; whole
    // aligned 2 MiB regions of a 2 MiB-mode region get one 2 MiB PTE.
    bool bind(B &b, uint64_t va, uint64_t bytes, uint64_t phys, uint64_t flags,
              uint16_t owner) {
        if (!rangeOk(va, bytes) || (phys & (kBigPageBytes - 1)) || !owner) return false;
        const uint64_t f = (flags & ~kPteAddrMask) | 1;
        for (uint64_t a = va; a < va + bytes; a += kBigPageBytes)
            if (spt[region(a)]) return false;           // small-page region: use bindPages
        for (uint64_t a = va; a < va + bytes;) {
            const uint32_t r = region(a);
            const uint64_t rs = arenaPageVa(r), re = rs + kArenaPageBytes;
            const uint64_t e = re < va + bytes ? re : va + bytes;
            const uint64_t p = phys + (a - va);
            if (!lpt[r] && a == rs && e == re && !(p & (kArenaPageBytes - 1))) {
                if (!writePd0(b, r, (p >> 4) | f)) return false;
                page[r] = owner;
            } else {
                if (!ensureLpt(b, r)) return false;
                for (uint64_t q = a; q < e; q += kBigPageBytes) {
                    const uint32_t i = entry(q);
                    if (!b.write64(lptAddr(r) + i * 8ULL, ((p + (q - a)) >> 4) | f)) return false;
                    ownerOf(r, i) = owner;
                }
            }
            a = e;
        }
        return true;
    }

    // Map n scattered 4 KiB pages at va (4 KiB aligned) through small page
    // tables. The regions touched must not hold 2 MiB / LPT mappings.
    bool bindPages(B &b, uint64_t va, uint32_t n, const uint64_t *phys, uint64_t flags,
                   uint16_t owner) {
        const uint64_t bytes = uint64_t(n) * kSmallPageBytes;
        if (!n || !owner || (va & (kSmallPageBytes - 1)) || va < kArenaBase || va >= kArenaEnd ||
            bytes > kArenaEnd - va)
            return false;
        for (uint64_t a = va & ~(kArenaPageBytes - 1); a < va + bytes; a += kArenaPageBytes)
            if (page[region(a)] || lpt[region(a)]) return false;
        const uint64_t f = (flags & ~kPteAddrMask) | 1;
        for (uint32_t i = 0; i < n; ++i) {
            if (phys[i] & (kSmallPageBytes - 1)) return false;
            const uint64_t a = va + uint64_t(i) * kSmallPageBytes;
            const uint32_t r = region(a);
            if (!ensureSpt(b, r)) return false;
            const uint32_t e = sptEntry(a);
            if (!b.write64(sptAddr(r) + e * 8ULL, (phys[i] >> 4) | f)) return false;
            sptOwnerOf(r, e) = owner;
        }
        return true;
    }

    // Unmap [va, va + bytes) (64 KiB aligned) whatever maps it.
    bool unbind(B &b, uint64_t va, uint64_t bytes) {
        if (!rangeOk(va, bytes)) return false;
        for (uint64_t a = va; a < va + bytes;) {
            const uint32_t r = region(a);
            const uint64_t rs = arenaPageVa(r), re = rs + kArenaPageBytes;
            const uint64_t e = re < va + bytes ? re : va + bytes;
            if (spt[r]) {
                for (uint64_t q = a; q < e; q += kSmallPageBytes) {
                    if (!b.write64(sptAddr(r) + sptEntry(q) * 8ULL, 0)) return false;
                    sptOwnerOf(r, sptEntry(q)) = 0;
                }
                if (!maybeFreeSpt(b, r)) return false;
            } else if (!lpt[r] && a == rs && e == re) {
                if (page[r] && !writePd0(b, r, 0)) return false;
                page[r] = 0;
            } else if (lpt[r] || page[r]) {
                if (!ensureLpt(b, r)) return false;
                for (uint64_t q = a; q < e; q += kBigPageBytes) {
                    const uint32_t i = entry(q);
                    if (!b.write64(lptAddr(r) + i * 8ULL, kLptInvalid)) return false;
                    ownerOf(r, i) = 0;
                }
                if (!maybeFreeLpt(b, r)) return false;
            }
            a = e;
        }
        return true;
    }

    // Unmap everything owned by `owner`; *any = something was unmapped.
    bool unbindOwner(B &b, uint16_t owner, bool *any) {
        bool ok = true;
        *any = false;
        if (!owner) return true;
        for (uint32_t r = 0; r < kArenaPages; ++r) {
            if (spt[r]) {
                bool hit = false;
                for (uint32_t i = 0; i < kSptEntries; ++i) {
                    if (sptOwnerOf(r, i) != owner) continue;
                    ok = b.write64(sptAddr(r) + i * 8ULL, 0) && ok;
                    sptOwnerOf(r, i) = 0;
                    hit = true;
                }
                if (hit) { *any = true; ok = maybeFreeSpt(b, r) && ok; }
            } else if (lpt[r]) {
                bool hit = false;
                for (uint32_t i = 0; i < kLptEntries; ++i) {
                    if (ownerOf(r, i) != owner) continue;
                    ok = b.write64(lptAddr(r) + i * 8ULL, kLptInvalid) && ok;
                    ownerOf(r, i) = 0;
                    hit = true;
                }
                if (hit) { *any = true; ok = maybeFreeLpt(b, r) && ok; }
            } else if (page[r] == owner) {
                ok = writePd0(b, r, 0) && ok;
                page[r] = 0;
                *any = true;
            }
        }
        return ok;
    }

    // Owner of the page containing va (0 = unmapped), for diagnostics/tests.
    uint16_t ownerAt(uint64_t va) const {
        if (va < kArenaBase || va >= kArenaEnd) return 0;
        const uint32_t r = region(va);
        if (spt[r]) {
            const uint32_t s = spt[r] - 1;
            return sptOwners[s / kSptPerChunk][(s % kSptPerChunk) * kSptEntries + sptEntry(va)];
        }
        return lpt[r] ? owners[(lpt[r] - 1) / kLptPerChunk]
                              [((lpt[r] - 1) % kLptPerChunk) * kLptEntries + entry(va)]
                      : page[r];
    }

    // Build a journal without touching live PTEs. All aliases and partial
    // bindings are translated by physical offset; holes and other owners
    // stay untouched. 2 MiB chunk alignment avoids splitting page tables.
    // A null journal counts entries only. The caller freezes the object's
    // bindings across count/allocation/plan/apply; apply detects stale PTEs.
    // memoryFlags is only APERTURE/VOL (0xC for coherent SYS, 0 for VRAM).
    template <class T>
    bool planOwnerRemap(B &b, uint16_t owner, const T &translation, uint64_t memoryFlags,
                        ArenaRemapEntry *journal, uint32_t capacity, uint32_t *count) const {
        if (!count) return false;
        *count = 0;
        if (!owner || owner == kArenaRawOwner || (memoryFlags & ~0xEULL)) return false;
        auto add = [&](uint64_t address, uint64_t pageBytes) -> bool {
            uint64_t old = 0, phys = 0;
            if (!b.read64(address, &old) || !(old & 1) ||
                !translation.translate(pteAddress(old), pageBytes, &phys) ||
                (phys & (pageBytes - 1)) || phys >= kResidencyAddressEnd ||
                pageBytes > kResidencyAddressEnd - phys) return false;
            if (journal) {
                if (*count >= capacity) return false;
                journal[*count] = {address, old,
                    (phys >> 4) | (old & ~(kPteAddrMask | 0xEULL)) | memoryFlags};
            }
            ++*count;
            return true;
        };
        for (uint32_t r = 0; r < kArenaPages; ++r) {
            if (spt[r]) {
                const uint32_t s = spt[r] - 1;
                const uint16_t *o = sptOwners[s / kSptPerChunk] + (s % kSptPerChunk) * kSptEntries;
                for (uint32_t i = 0; i < kSptEntries; ++i)
                    if (o[i] == owner && !add(sptAddr(r) + i * 8ULL, kSmallPageBytes)) return false;
            } else if (lpt[r]) {
                const uint32_t s = lpt[r] - 1;
                const uint16_t *o = owners[s / kLptPerChunk] + (s % kLptPerChunk) * kLptEntries;
                for (uint32_t i = 0; i < kLptEntries; ++i)
                    if (o[i] == owner && !add(lptAddr(r) + i * 8ULL, kBigPageBytes)) return false;
            } else if (page[r] == owner && !add(pd0Addr(r), kArenaPageBytes)) return false;
        }
        return true;
    }

    // the owner tag mapping va (0 = unmapped), and how: 1 = 2 MiB
    // PTE, 2 = 64 KiB LPT entry, 3 = 4 KiB SPT entry. For naming an MMU fault.
    uint16_t ownerAt(uint64_t va, uint32_t *how) {
        *how = 0;
        if (va < kArenaBase || va >= kArenaEnd) return 0;
        const uint32_t r = region(va);
        if (spt[r]) { *how = 3; return sptOwnerOf(r, sptEntry(va)); }
        if (lpt[r]) { *how = 2; return ownerOf(r, entry(va)); }
        if (page[r]) *how = 1;
        return page[r];
    }

private:
    static uint32_t region(uint64_t va) {
        return static_cast<uint32_t>((va - kArenaBase) / kArenaPageBytes);
    }
    static uint32_t entry(uint64_t va) {
        return static_cast<uint32_t>((va >> 16) & (kLptEntries - 1));
    }
    static uint32_t sptEntry(uint64_t va) {
        return static_cast<uint32_t>((va >> 12) & (kSptEntries - 1));
    }
    uint64_t sptAddr(uint32_t r) const {
        const uint32_t s = spt[r] - 1;
        return sptChunk[s / kSptPerChunk] + (s % kSptPerChunk) * uint64_t(kSptBytes);
    }
    uint16_t &sptOwnerOf(uint32_t r, uint32_t i) {
        const uint32_t s = spt[r] - 1;
        return sptOwners[s / kSptPerChunk][(s % kSptPerChunk) * kSptEntries + i];
    }
    bool ensureSpt(B &b, uint32_t r) {
        if (spt[r]) return true;
        uint32_t s = 0;
        while (s < kSptChunks * kSptPerChunk && (sptUsed[s / 32] & (1u << (s % 32)))) ++s;
        if (s == kSptChunks * kSptPerChunk) return false;
        const uint32_t c = s / kSptPerChunk;
        if (!sptChunk[c]) {
            uint64_t phys = 0;
            if (!b.allocChunk(&phys)) return false;
            uint16_t *o = static_cast<uint16_t *>(b.allocOwners(kSptPerChunk * kSptEntries * 2));
            if (!o) return false;
            for (uint32_t i = 0; i < kSptPerChunk * kSptEntries; ++i) o[i] = 0;
            sptChunk[c] = phys;
            sptOwners[c] = o;
        }
        const uint64_t addr = sptChunk[c] + (s % kSptPerChunk) * uint64_t(kSptBytes);
        if (!b.zero(addr, kSptBytes)) return false;
        sptUsed[s / 32] |= 1u << (s % 32);
        spt[r] = static_cast<uint16_t>(s + 1);
        ++sptCount;
        return b.write64(pd0Addr(r), 0) && b.write64(pd0Addr(r) + 8, pdeVram(addr));
    }
    bool maybeFreeSpt(B &b, uint32_t r) {
        for (uint32_t i = 0; i < kSptEntries; ++i)
            if (sptOwnerOf(r, i)) return true;
        const uint32_t s = spt[r] - 1;
        if (!writePd0(b, r, 0)) return false;
        sptUsed[s / 32] &= ~(1u << (s % 32));
        spt[r] = 0;
        --sptCount;
        return true;
    }
    static bool rangeOk(uint64_t va, uint64_t bytes) {
        return bytes && !((va | bytes) & (kBigPageBytes - 1)) && va >= kArenaBase &&
               va < kArenaEnd && bytes <= kArenaEnd - va;
    }
    uint64_t pd0Addr(uint32_t r) const {
        // One 4 KiB PD0 table per 512 MiB; 16-byte dual entry per 2 MiB
        return tables + (r / 256) * 4096ULL + (r % 256) * 16ULL;
    }
    uint64_t lptAddr(uint32_t r) const {
        const uint32_t s = lpt[r] - 1;
        return chunk[s / kLptPerChunk] + (s % kLptPerChunk) * uint64_t(kLptBytes);
    }
    uint16_t &ownerOf(uint32_t r, uint32_t i) {
        const uint32_t s = lpt[r] - 1;
        return owners[s / kLptPerChunk][(s % kLptPerChunk) * kLptEntries + i];
    }
    bool writePd0(B &b, uint32_t r, uint64_t q0) {
        return b.write64(pd0Addr(r), q0) && b.write64(pd0Addr(r) + 8, 0);
    }

    // Give region r an LPT, preserving a 2 MiB mapping as 32 big pages.
    bool ensureLpt(B &b, uint32_t r) {
        if (lpt[r]) return true;
        uint32_t s = 0;
        while (s < kArenaPages && (used[s / 32] & (1u << (s % 32)))) ++s;
        if (s == kArenaPages) return false;
        const uint32_t c = s / kLptPerChunk;
        if (!chunk[c]) {
            uint64_t phys = 0;
            if (!b.allocChunk(&phys)) return false;
            uint16_t *o = static_cast<uint16_t *>(b.allocOwners(kLptPerChunk * kLptEntries * 2));
            if (!o) return false;
            for (uint32_t i = 0; i < kLptPerChunk * kLptEntries; ++i) o[i] = 0;
            chunk[c] = phys;
            owners[c] = o;
        }
        const uint64_t addr = chunk[c] + (s % kLptPerChunk) * uint64_t(kLptBytes);
        uint64_t old = 0;
        if (page[r] && !b.read64(pd0Addr(r), &old)) return false;
        const bool split = page[r] && (old & 1);
        const uint64_t base = pteAddress(old), flags = old & ~kPteAddrMask;
        for (uint32_t i = 0; i < kLptEntries; ++i) {
            const uint64_t v = split ? ((base + i * kBigPageBytes) >> 4) | flags : kLptInvalid;
            if (!b.write64(addr + i * 8ULL, v)) return false;
            owners[c][(s % kLptPerChunk) * kLptEntries + i] = split ? page[r] : 0;
        }
        used[s / 32] |= 1u << (s % 32);
        lpt[r] = static_cast<uint16_t>(s + 1);
        page[r] = 0;
        ++lptCount;
        return writePd0(b, r, pdeVram(addr));
    }

    // Region r's LPT has no mapping left: back to an empty 2 MiB region.
    bool maybeFreeLpt(B &b, uint32_t r) {
        for (uint32_t i = 0; i < kLptEntries; ++i)
            if (ownerOf(r, i)) return true;
        const uint32_t s = lpt[r] - 1;
        if (!writePd0(b, r, 0)) return false;
        used[s / 32] &= ~(1u << (s % 32));
        lpt[r] = 0;
        --lptCount;
        return true;
    }
};

}  // namespace nvgsp
