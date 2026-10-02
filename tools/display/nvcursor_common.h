// Shared bounded cursor diagnostics; no NVDisplay routing changes.
#pragma once
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t exception[3], pending, error, core, getput[2];
    uint32_t assy[7], armed[7], usage[2];
} Snapshot;

static inline kern_return_t peek(io_connect_t c, uint32_t addr, uint32_t count,
                          uint32_t *out) {
    uint64_t in[2] = {addr, count};
    size_t bytes = count * sizeof(*out);
    kern_return_t kr = IOConnectCallMethod(c, 8, in, 2, NULL, 0, NULL,
                                          NULL, out, &bytes);
    return kr ? kr : (bytes == count * sizeof(*out) ? KERN_SUCCESS : KERN_FAILURE);
}

static inline bool snapshot(io_connect_t c, Snapshot *s) {
    return !peek(c, 0x611020, 3, s->exception) &&
           !peek(c, 0x611854, 1, &s->pending) &&
           !peek(c, 0x611848, 1, &s->error) &&
           !peek(c, 0x610630, 1, &s->core) &&
           !peek(c, 0x680000, 2, s->getput) &&
           !peek(c, 0x682088, 7, s->assy) &&
           !peek(c, 0x68a088, 7, s->armed) &&
           !peek(c, 0x682030, 1, &s->usage[0]) &&
           !peek(c, 0x68a030, 1, &s->usage[1]);
}

static inline void dump(const char *tag, const Snapshot *s) {
    printf("%s exception=%08x data=%08x code=%08x method=%04x type=%u "
           "pending=%08x error=%08x core=%08x PUT=%x GET=%x\n", tag,
           s->exception[0], s->exception[1], s->exception[2],
           (s->exception[0] & 0xfff) << 2, (s->exception[0] >> 12) & 7,
           s->pending, s->error, s->core, s->getput[0], s->getput[1]);
    printf("%s usage ASSY=%08x ARMED=%08x\n", tag, s->usage[0], s->usage[1]);
    for (unsigned i = 0; i < 7; ++i)
        printf("%s cursor+%02x ASSY=%08x ARMED=%08x\n", tag, i * 4,
               s->assy[i], s->armed[i]);
    fflush(stdout);
}

static inline kern_return_t core(io_connect_t c, uint32_t usage, const uint32_t state[7]) {
    const uint32_t words[] = {
        0x00042030, usage, 0x00042098, state[4],
        0x00082088, state[0], state[1], 0x00082090, state[2], state[3],
        0x0004209c, state[5], 0x000420a0, state[6],
        0x00080218, 0, 0, 0x00040200, 1
    };
    return IOConnectCallStructMethod(c, 12, words, sizeof(words), NULL, NULL);
}

static inline uint64_t ns_now(void) {
    static mach_timebase_info_data_t t;
    if (!t.denom) mach_timebase_info(&t);
    // Convert in floating point to avoid overflowing the tick product.
    return (uint64_t)((double)mach_absolute_time() * t.numer / t.denom);
}

static inline bool armed_matches(const Snapshot *s, uint32_t usage,
                          const uint32_t state[7]) {
    return s->getput[0] == s->getput[1] && ((s->core >> 16) & 0x1f) == 0xb &&
           s->usage[0] == usage && s->usage[1] == usage &&
           !memcmp(s->assy, state, sizeof(s->assy)) &&
           !memcmp(s->armed, state, sizeof(s->armed));
}

static inline bool wait_armed(io_connect_t c, const char *tag, const Snapshot *before,
                       uint32_t usage, const uint32_t state[7]) {
    const uint64_t end = ns_now() + 200000000;
    Snapshot s;
    memset(&s, 0, sizeof(s));
    bool fault_recorded = false, armed = false;
    do {
        if (!snapshot(c, &s)) { fprintf(stderr, "readback failed\n"); return false; }
        // FE_EXCEPT retains an ACKed snapshot. A valid bit alone does not
        // establish a new exception; retain the first change/pending reason.
        if (!fault_recorded &&
            ((s.pending & 1) || memcmp(s.exception, before->exception,
                                     sizeof(s.exception)))) {
            dump("first-observed-exception-change", &s);
            fault_recorded = true;
        }
        armed = armed_matches(&s, usage, state);
        if (armed) break;
        usleep(1000);
    } while (ns_now() < end);
    dump(tag, &s);
    printf("%s armed-match=%s first-exception-change=%s\n", tag,
           armed ? "yes" : "no", fault_recorded ? "yes" : "no");
    return armed;
}

static inline bool property_u64(io_service_t service, CFStringRef key, uint64_t *out) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(service, key,
                                                kCFAllocatorDefault, 0);
    bool ok = v && CFGetTypeID(v) == CFNumberGetTypeID() &&
              CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, out);
    if (v) CFRelease(v);
    return ok;
}
