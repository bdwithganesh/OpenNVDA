#pragma once

#include <stdint.h>

namespace nvgsp {

// 0.126.0: VRAM range index for the kext allocator (memAllocLocked).
//
// Before 0.126.0 each VRAM allocation scanned all 4095 memory-object slots
// once per candidate move, O(n^2) under the device lock. VramHeap keeps the
// live VRAM ranges sorted by address in a fixed array (no allocation), so a
// first-fit search is one pass over the gaps and insert/remove are one
// memmove. Placement is unchanged: lowest free address in [start, end).
//
// Ranges never overlap and have non-zero size; the caller keeps the
// range <-> object mapping (the key is the start address).
template <uint32_t Cap>
struct VramHeap {
    struct Range { uint64_t phys, bytes; };
    Range r[Cap];
    uint32_t n = 0;

    void clear() { n = 0; }
    uint32_t count() const { return n; }

    // index of the first range with phys >= addr
    uint32_t lowerBound(uint64_t addr) const {
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            if (r[mid].phys < addr) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    // lowest address a >= start with [a, a + bytes) free and a + bytes <= end
    bool fit(uint64_t start, uint64_t end, uint64_t bytes, uint64_t *out) const {
        if (!bytes || !out || start >= end || bytes > end - start) return false;
        uint64_t cand = start;
        uint32_t i = lowerBound(start);
        // the range just below start may reach past it
        if (i > 0 && r[i - 1].phys + r[i - 1].bytes > cand) cand = r[i - 1].phys + r[i - 1].bytes;
        for (;; ++i) {
            if (cand > end || bytes > end - cand) return false;
            if (i == n || r[i].phys >= cand + bytes) { *out = cand; return true; }
            const uint64_t top = r[i].phys + r[i].bytes;
            if (top > cand) cand = top;
        }
    }

    bool insert(uint64_t phys, uint64_t bytes) {
        if (!bytes || n == Cap || phys + bytes < phys) return false;
        const uint32_t i = lowerBound(phys);
        if (i < n && r[i].phys < phys + bytes) return false;          // overlaps next
        if (i > 0 && r[i - 1].phys + r[i - 1].bytes > phys) return false;   // overlaps prev
        for (uint32_t k = n; k > i; --k) r[k] = r[k - 1];
        r[i] = Range{phys, bytes};
        ++n;
        return true;
    }

    bool remove(uint64_t phys) {
        const uint32_t i = lowerBound(phys);
        if (i == n || r[i].phys != phys) return false;
        for (uint32_t k = i; k + 1 < n; ++k) r[k] = r[k + 1];
        --n;
        return true;
    }
};

}  // namespace nvgsp
