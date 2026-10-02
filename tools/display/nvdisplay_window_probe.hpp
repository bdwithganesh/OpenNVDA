// Fixed head0/window0 composition experiment. The production driver remembers
// its PUT; return GET/PUT to that exact offset and restore every PB byte.
// Window ARMED registers are not publicly mapped: report ASSY/fetch separately.
#pragma once
#include "nvdisplay_admin.hpp"

struct WindowProbeState {
    uint32_t getput[2], composition, factor, ilut[3], interlock[2], exception[3];
};
static inline bool window_snapshot(io_connect_t c, WindowProbeState *s) {
    return !peek(c, 0x690000, 2, s->getput) &&
        !peek(c, 0x6902ec, 1, &s->composition) && !peek(c, 0x6902f4, 1, &s->factor) &&
        !peek(c, 0x690440, 3, s->ilut) && !peek(c, 0x690370, 2, s->interlock) &&
        !peek(c, 0x61102c, 3, s->exception);
}
static inline void window_dump(const char *tag, const WindowProbeState &s) {
    printf("%s window PUT=%x GET=%x composition=%08x factor=%08x "
           "ILUT=%08x/%08x/%08x interlock=%08x/%08x "
           "exception=%08x/%08x/%08x method=%04x type=%u\n", tag,
           s.getput[0], s.getput[1], s.composition, s.factor,
           s.ilut[0], s.ilut[1], s.ilut[2], s.interlock[0], s.interlock[1],
           s.exception[0], s.exception[1], s.exception[2],
           (s.exception[0]&0xfff)<<2, (s.exception[0]>>12)&7);
    fflush(stdout);
}
static inline bool window_equal(const WindowProbeState &s, const WindowProbeState &want) {
    return s.getput[0] == s.getput[1] && s.composition == want.composition &&
        s.factor == want.factor && !memcmp(s.ilut, want.ilut, sizeof(s.ilut));
}
static inline bool window_guard(io_connect_t c, io_service_t service, uint64_t inst,
                               WindowProbeState *s, uint32_t pb[1024], bool normal=false) {
    uint64_t remembered = 0, flips = 0, presents = 0;
    uint32_t bounds[2], fmt[12], controls[6], owner[2], format = 0, coreFlags[2];
    const uint32_t identity[12] = {0x10000,0,0,0,0,0x10000,0,0,0,0,0x10000,0};
    const uint32_t addrs[] = {0x69045c,0x6904bc,0x69053c,0x69059c,0x6904a0,0x690580};
    if (!window_snapshot(c, s) || !property_u64(service, CFSTR("NVGspControl-wnd-put"), &remembered) ||
        (property_u64(service, CFSTR("NVGspControl-flip-count"), &flips) && flips) ||
        (property_u64(service, CFSTR("NVGspControl-present-count"), &presents) && presents) ||
        remembered != 88 || s->getput[0] != (normal ? 140U : 88U) || s->getput[1] != s->getput[0] ||
        s->composition != (normal ? 0x80U : 0x10000U) || s->factor != (normal ? 0x11U : 0U) ||
        (normal ? (s->ilut[0]!=0x40508 || s->ilut[1]!=0xc0d000e2 || s->ilut[2]) : s->ilut[1]) ||
        s->interlock[0] != (normal ? 1U : 0U) || s->interlock[1] ||
        peek(c, 0x681000, 1, owner) || peek(c, 0x689000, 1, owner+1) || owner[0] || owner[1] ||
        peek(c, 0x69022c, 1, &format) || format != 0xcf ||
        peek(c, 0x680218, 2, coreFlags) || coreFlags[0] || coreFlags[1] != (normal ? 1U : 0U) ||
        peek(c, 0x681010, 1, bounds) || peek(c, 0x689010, 1, bounds+1) ||
        bounds[0] != bounds[1] || !(bounds[0]&0x10000) ||
        peek(c, 0x690400, 12, fmt) || memcmp(fmt, identity, sizeof(fmt)) ||
        !vread(c, inst+0x8000, pb, 1024)) return false;
    for (unsigned i = 0; i < 6; ++i)
        if (peek(c, addrs[i], 1, controls+i) || controls[i]) return false;
    // Reserve enough empty PB tail for trial, restoration and a JUMP.
    for (unsigned i = s->getput[0]/4; i < (s->getput[0]+120)/4; ++i) if (pb[i]) return false;
    window_dump("window-before", *s);
    printf("window guard: remembered PUT88/currentPUT%u, no direct presenter, ILUT bounds, "
           "identity FMT, disabled CSC/LUT stages and vacant PB verified\n",s->getput[0]);
    return true;
}
static inline bool window_poke_put(io_connect_t c, uint32_t put) {
    const uint64_t in[] = {0x690000, put};
    const auto kr = IOConnectCallScalarMethod(c, 9, in, 2, nullptr, nullptr);
    if (kr) fprintf(stderr, "window PUT=%x transport=%08x\n", put, kr);
    return !kr;
}
static inline bool window_queue(io_connect_t c, uint64_t inst, uint32_t at,
                               const WindowProbeState &s, uint32_t *next) {
    uint32_t getput[2] = {};
    const uint32_t words[] = {0x000402ec,s.composition,0x000402f4,s.factor,
        0x000c0440,s.ilut[0],s.ilut[1],s.ilut[2],0x00080370,1,0,0x00040200,1};
    if ((at&3) || at > 4096-sizeof(words)-4 || peek(c, 0x690000, 2, getput) ||
        getput[0] != at || getput[1] != at ||
        !vwrite(c, inst+0x8000+at, words, sizeof(words)/4)) {
        fprintf(stderr, "window queue/write refused at=%x PUT=%x GET=%x\n", at, getput[0], getput[1]);
        return false;
    }
    uint32_t check[sizeof(words)/4];
    if (!vread(c, inst+0x8000+at, check, sizeof(check)/4) || memcmp(check, words, sizeof(words))) {
        fprintf(stderr, "window PB readback failed at=%x\n", at); return false;
    }
    *next = at+sizeof(words);
    return window_poke_put(c, *next);
}
static inline bool window_rewind(io_connect_t c, uint64_t inst, uint32_t at,
                                 const WindowProbeState &before, const uint32_t pb[1024]) {
    WindowProbeState s = {};
    if (!window_snapshot(c, &s) || !window_equal(s, before) || s.getput[0] != at) return false;
    // Restore one-shot interlock ASSY flags without another window UPDATE.
    const uint32_t flags[] = {0x00080370,before.interlock[0],before.interlock[1]};
    uint32_t flagCheck[3];
    if (!vwrite(c, inst+0x8000+at, flags, 3) ||
        !vread(c, inst+0x8000+at, flagCheck, 3) || memcmp(flagCheck, flags, sizeof(flags))) return false;
    at += sizeof(flags);
    if (!window_poke_put(c, at)) return false;
    const auto flagEnd = ns_now()+200000000;
    do {
        if (!window_snapshot(c, &s)) return false;
        if (s.getput[0] == at && s.getput[1] == at) break;
        usleep(1000);
    } while (ns_now() < flagEnd);
    if (s.getput[0] != at || s.getput[1] != at ||
        memcmp(s.interlock, before.interlock, sizeof(s.interlock))) return false;
    const uint32_t jump = 0x20000000U|before.getput[0]; // C67E DMA_JUMP_OFFSET bits11:2
    uint32_t check = 0;
    if (!vwrite(c, inst+0x8000+at, &jump, 1) ||
        !vread(c, inst+0x8000+at, &check, 1) || check != jump ||
        !window_poke_put(c, before.getput[0])) return false;
    const auto end = ns_now()+200000000;
    do {
        if (!window_snapshot(c, &s)) return false;
        if (s.getput[0] == before.getput[0] && s.getput[1] == before.getput[1]) break;
        usleep(1000);
    } while (ns_now() < end);
    window_dump("window-rewind", s);
    uint32_t restored[1024];
    return s.getput[0] == before.getput[0] && s.getput[1] == before.getput[1] &&
        window_equal(s, before) && vwrite(c, inst+0x8000, pb, 1024) &&
        vread(c, inst+0x8000, restored, 1024) && !memcmp(restored, pb, sizeof(restored));
}
