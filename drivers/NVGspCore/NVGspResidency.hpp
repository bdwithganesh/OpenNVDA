#pragma once

#include <stdint.h>

namespace nvgsp {

// Runtime residency support. The kext must supply the drained-engine,
// allocation, CE-copy and TLB-flush parts before using these helpers.
constexpr uint64_t kResidencyChunkBytes = 0x200000;
constexpr uint64_t kResidencyAddressEnd = 1ULL << 49;

// Either contiguous VRAM or a list of distinct, aligned 2 MiB SYS chunks.
// The owner keeps the list alive and unchanged for the whole transaction.
struct ResidencyBacking {
    uint64_t base, bytes;
    const uint64_t *chunks;
    uint32_t count;

    bool valid() const {
        if (!bytes || (bytes & (kResidencyChunkBytes - 1))) return false;
        if (!chunks)
            return !count && !(base & (kResidencyChunkBytes - 1)) &&
                   base < kResidencyAddressEnd && bytes <= kResidencyAddressEnd - base;
        if (bytes / kResidencyChunkBytes != count) return false;
        for (uint32_t i = 0; i < count; ++i) {
            if ((chunks[i] & (kResidencyChunkBytes - 1)) ||
                chunks[i] > kResidencyAddressEnd - kResidencyChunkBytes) return false;
            for (uint32_t j = 0; j < i; ++j)
                if (chunks[j] == chunks[i]) return false;
        }
        return true;
    }

    bool offset(uint64_t phys, uint64_t pageBytes, uint64_t *out) const {
        if (!out || !pageBytes || pageBytes > kResidencyChunkBytes) return false;
        if (!chunks) {
            if (phys < base || phys - base >= bytes || pageBytes > bytes - (phys - base)) return false;
            *out = phys - base;
            return true;
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (phys < chunks[i] || phys - chunks[i] >= kResidencyChunkBytes) continue;
            if (pageBytes > kResidencyChunkBytes - (phys - chunks[i])) return false;
            *out = uint64_t(i) * kResidencyChunkBytes + phys - chunks[i];
            return true;
        }
        return false;
    }

    bool address(uint64_t offset, uint64_t pageBytes, uint64_t *out) const {
        if (!out || !pageBytes || offset >= bytes || pageBytes > bytes - offset) return false;
        if (!chunks) { *out = base + offset; return true; }
        const uint64_t index = offset / kResidencyChunkBytes, in = offset % kResidencyChunkBytes;
        if (index >= count || pageBytes > kResidencyChunkBytes - in) return false;
        *out = chunks[index] + in;
        return true;
    }
};

struct ResidencyTranslation {
    ResidencyBacking from, to;
    bool valid() const { return from.bytes == to.bytes && from.valid() && to.valid(); }
    bool translate(uint64_t oldPhys, uint64_t pageBytes, uint64_t *newPhys) const {
        uint64_t offset = 0;
        return from.offset(oldPhys, pageBytes, &offset) && to.address(offset, pageBytes, newPhys);
    }
};

struct ArenaRemapEntry { uint64_t address, before, after; };
enum class ArenaRemapResult { Success, Stale, RolledBack, RollbackFailed };

// No allocation or ownership mutation here. The caller serialises all page
// table writers and drains every engine first. Even a failed write may have
// reached hardware, so rollback includes that entry. After any attempted
// write the caller must flush the TLB before allowing GPU execution. Both
// backings remain owned until the update AND flush succeed. RollbackFailed
// requires recovery with both backings retained; it is never safe to free one.
template <class B>
ArenaRemapResult applyArenaRemap(B &b, const ArenaRemapEntry *entries, uint32_t count) {
    if (!entries && count) return ArenaRemapResult::Stale;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t value = 0;
        if (!b.read64(entries[i].address, &value) || value != entries[i].before)
            return ArenaRemapResult::Stale;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (b.write64(entries[i].address, entries[i].after)) continue;
        bool restored = true;
        for (uint32_t j = i + 1; j; --j)
            restored = b.write64(entries[j - 1].address, entries[j - 1].before) && restored;
        return restored ? ArenaRemapResult::RolledBack : ArenaRemapResult::RollbackFailed;
    }
    return ArenaRemapResult::Success;
}

// A caller-maintained monotonic use stamp implements LRU. Fence completion
// must include GR, CE and video, because they can all reference an object.
// Raw bindings and persistent BAR1 mappings cannot move with this protocol.
struct ResidencyCandidate {
    uint64_t lastUse;
    bool live, resident, idle, cpuMapped, rawBound, internal, presented, migrating;
};

inline bool residencyCanEvict(const ResidencyCandidate &c) {
    return c.live && c.resident && c.idle && !c.cpuMapped && !c.rawBound &&
           !c.internal && !c.presented && !c.migrating;
}

inline uint32_t residencyVictim(const ResidencyCandidate *items, uint32_t count) {
    uint32_t best = count;
    if (!items) return best;
    for (uint32_t i = 0; i < count; ++i)
        if (residencyCanEvict(items[i]) && (best == count || items[i].lastUse < items[best].lastUse))
            best = i;
    return best;
}

}  // namespace nvgsp
