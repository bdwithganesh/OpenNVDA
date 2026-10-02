// Fixed64px cursor comparison inside an already accepted normal pipeline.
// Caller owns a64KiB RM allocation: OLUT0, ILUT+4000, cursor+8000.
// The cursor descriptor and backing remain owned until the joint head/window
// restoration has verified the original disabled cursor and usage bounds.
#pragma once
#include "nvdisplay_admin.hpp"
#include <initializer_list>

enum class CursorObservation { Accepted, Refused, Uncertain };
using CursorObserver = CursorObservation (*)(io_connect_t,const Snapshot &,bool,void *);
static inline bool cursor_compare(io_connect_t c, uint64_t inst, uint64_t cursorOffset,
                                   const Snapshot &original, bool *accepted,
                                   CursorObserver observer=nullptr,void *observerContext=nullptr) {
    *accepted = false;
    Snapshot before = {}; uint32_t window = 0, lutHandle = 0;
    uint32_t hash[2048], objects[64];
    if ((cursorOffset&0xfff) || cursorOffset > vramBytes-0x8000 ||
        !snapshot(c, &before) || !armed_matches(&before, original.usage[1], original.armed) ||
        peek(c, 0x6902ec, 1, &window) || window != 0x80 ||
        peek(c, 0x68a288, 1, &lutHandle) || !lutHandle ||
        !vread(c, inst, hash, 2048) || !vread(c, inst+0x2000, objects, 64)) {
        fprintf(stderr, "cursor normal-pipeline guard refused\n"); return false;
    }
    for (unsigned slot : {56U,60U}) {
        const bool empty = !hash[slot*2] && !hash[slot*2+1];
        const bool cursor = hash[slot*2] == 0xc0d0d002 && hash[slot*2+1] == 0x00414001;
        if (!empty && !cursor) { fprintf(stderr, "cursor RAMHT occupied by unexpected object\n"); return false; }
    }
    uint32_t pixels[1024], readback[1024];
    for (auto &pixel : pixels) pixel = 0xffff0000; // same red image as backing-only comparison
    for (unsigned i = 0; i < 4; ++i) {
        if (!vwrite(c, cursorOffset+i*4096, pixels, 1024) ||
            !vread(c, cursorOffset+i*4096, readback, 1024) ||
            memcmp(pixels, readback, sizeof(pixels))) return false;
    }
    const uint32_t descriptor[] = {5, (uint32_t)(cursorOffset>>8), (uint32_t)(cursorOffset>>40),
        (uint32_t)((cursorOffset+0x7fff)>>8), (uint32_t)((cursorOffset+0x7fff)>>40)};
    if (!vwrite(c, inst+0x20a0, descriptor, 5)) return false;
    const uint32_t entry[] = {0xc0d0d002,0x00414001};
    if (!vwrite(c, inst+56*8, entry, 2) || !vwrite(c, inst+60*8, entry, 2)) return false;
    uint32_t afterHash[2048], afterObjects[64];
    if (!vread(c, inst, afterHash, 2048) || !vread(c, inst+0x2000, afterObjects, 64) ||
        memcmp(afterHash+112, entry, sizeof(entry)) || memcmp(afterHash+120, entry, sizeof(entry)) ||
        memcmp(afterObjects+40, descriptor, sizeof(descriptor))) return false;
    for (unsigned i = 0; i < 2048; ++i)
        if (i/2 != 56 && i/2 != 60 && hash[i] != afterHash[i]) return false;
    for (unsigned i = 0; i < 64; ++i)
        if ((i < 40 || i >= 45) && objects[i] != afterObjects[i]) return false;
    printf("cursor: same RM backing/manual DMA/red64px/relative0/control; normal composition is comparison variable\n");
    const uint32_t cursor[] = {0xc0d0d002,0xc0d0d002,0,0,0,0x800001cf,0x75ff};
    const uint32_t usage = (original.usage[1]&~7U)|2;
    const auto kr = core(c, usage, cursor);
    printf("normal-cursor submit=%08x (fetch result only)\n", kr);
    *accepted = wait_armed(c, "normal-cursor", &before, usage, cursor) && !kr;
    Snapshot changed = {}; if (!snapshot(c, &changed)) return false;
    if (!*accepted) {
        const auto restore = core(c, original.usage[1], original.armed);
        const bool safe = wait_armed(c, "normal-cursor-rejected-restore", &changed,
                                    original.usage[1], original.armed) && !restore;
        printf("normal-cursor actual-ARM=no cursor-restored=%s visual-proof=unverified\n", safe ? "yes" : "no");
        return safe;
    }
    if(observer && observer(c,changed,true,observerContext)==CursorObservation::Uncertain)return false;
    uint32_t disabled[7]; memcpy(disabled, cursor, sizeof(disabled)); disabled[5] &= ~0x80000000U;
    const auto disable = core(c, usage, disabled);
    if (!wait_armed(c, "normal-cursor-disable", &changed, usage, disabled) || disable) return false;
    if (!snapshot(c, &changed)) return false;
    if(observer && observer(c,changed,false,observerContext)==CursorObservation::Uncertain)return false;
    // Retain prepared capacity through cleanup. Even joint shrinking stalled;
    // the caller verifies NULL/disabled references and reboots for old bounds.
    const auto prepare = core(c, usage, original.armed);
    const bool safe = wait_armed(c, "normal-cursor-prepare-original", &changed,
                                usage, original.armed) && !prepare;
    printf("normal-cursor actual-ARM=yes disabled=%s original-bounds-restoration=pending visual-proof=unverified\n",
           safe ? "yes" : "no");
    return safe;
}
