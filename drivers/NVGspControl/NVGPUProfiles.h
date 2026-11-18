#pragma once
// NVIDIA GPU family registry.
//
// This file is deliberately data-only: it recognizes public PCI device IDs and
// states whether a full bring-up profile exists in this tree. Recognition is
// not support. Only the AD103/RTX 4080 profile has a register, fuse, firmware
// and GR/CE/display path in this repository; the other entries are recorded so
// an unsupported card fails with a precise message instead of being mistaken
// for the 4080.
#include <stdint.h>

struct NVGPUProfile {
    uint16_t device;
    const char *family;      // architecture/family, e.g. AD103
    const char *marketing;   // public board name
    uint32_t flags;          // reserved for profile-specific capabilities
    bool supported;          // full native profile exists in this tree
};

static const NVGPUProfile kNVGPUProfiles[] = {
    // Verified target: AD103 / RTX 4080 (the current full profile).
    {0x2704, "AD103", "GeForce RTX 4080", 0, true},
    // Recognized public IDs; no AD102/GA10x/TU10x register/firmware profile
    // exists here. They must not be programmed with AD103 offsets.
    {0x2684, "AD102", "GeForce RTX 4090", 0, false},
    {0x2204, "GA102", "GeForce RTX 3090", 0, false},
    {0x2484, "GA104", "GeForce RTX 3070", 0, false},
    {0x1e04, "TU102", "GeForce RTX 2080 Ti", 0, false},
};

static inline const NVGPUProfile *nvGPUProfileFor(uint16_t device) {
    for (unsigned i = 0; i < sizeof(kNVGPUProfiles) / sizeof(kNVGPUProfiles[0]); ++i)
        if (kNVGPUProfiles[i].device == device) return &kNVGPUProfiles[i];
    return nullptr;
}

static inline const char *nvGPUFamilyName(uint16_t device) {
    const NVGPUProfile *p = nvGPUProfileFor(device);
    return p ? p->family : "unknown";
}

static inline const char *nvGPUDeviceName(uint16_t device) {
    const NVGPUProfile *p = nvGPUProfileFor(device);
    return p ? p->marketing : "unknown NVIDIA device";
}

static inline bool nvGPUProfileSupported(uint16_t device) {
    const NVGPUProfile *p = nvGPUProfileFor(device);
    return p && p->supported;
}
