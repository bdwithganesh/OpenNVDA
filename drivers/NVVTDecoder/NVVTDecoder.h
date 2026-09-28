#pragma once
#include <VideoToolbox/VideoToolbox.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Opt-in registration for this process. Requires the Sonoma private decoder
 * ABI and an administrator connection to NVGspControl. No software fallback. */
OSStatus NVVTRegisterH264Decoder(void);
typedef struct {
    uint64_t sessions, submitted, emitted, errors, gpu_nanoseconds;
} NVVTStats;
void NVVTGetStats(NVVTStats *stats);
#ifdef __cplusplus
}
#endif
