#pragma once
// NVMTLFamilyGate.h - the one place that decides which MTLGPUFamily values
// the NVIDIA Metal driver may advertise.
//
// Pure C (no Metal/IOKit headers) so tests/test_nvmtl_family_gate.py can
// compile and execute the exact policy used by NVMTLDevice.m.
//
// Rules:
//   * Mac1/Mac2/Common1/2/3 are the native baseline the driver has shipped and
//     tested; they are always advertised.
//   * Metal3 (mesh/tessellation family) is opt-in only. The conformance
//     runner sets nvram nvmtl-metal3=1 after the Metal 3 matrix passes.
//   * Metal4 requires BOTH the MTL4 object graph (NVMTL_METAL4=1 or nvram
//     nvmtl-metal4=1) and the family receipt (NVMTL_METAL4_FAMILY=1 or
//     nvram nvmtl-metal4=1). A per-process NVMTL_METAL4=1 run must not make
//     the family visible to other applications.
//   * Apple* families are never advertised; this is not Apple silicon.

#ifndef NVMTL_FAMILY_GATE_H
#define NVMTL_FAMILY_GATE_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NVMTLFamilyApple1  = 1001,
    NVMTLFamilyApple9  = 1009,
    NVMTLFamilyMac1    = 2001,
    NVMTLFamilyMac2    = 2002,
    NVMTLFamilyCommon1 = 3001,
    NVMTLFamilyCommon2 = 3002,
    NVMTLFamilyCommon3 = 3003,
    NVMTLFamilyMetal3  = 5001,
    NVMTLFamilyMetal4  = 5002
};

typedef struct NVMTLFamilyGates {
    unsigned metal3OptIn;    // NVMTL_METAL3=1 or nvram nvmtl-metal3=1
    unsigned metal4OptIn;    // MTL4 objects enabled (NVMTL_METAL4/nvmtl-metal4)
    unsigned metal4Family;   // conformance receipt (NVMTL_METAL4_FAMILY/nvmtl-metal4)
} NVMTLFamilyGates;

// Families backed by the native Metal 2/Common3 path and the long-running
// WindowServer/MPS/app test history. Deliberately no Apple* and no MetalN.
// 0.9.56: Mac1 too - every Mac2 GPU is a Mac1 GPU, Intel/AMD Macs answer YES
// for both, and Metal's API validation checks Mac1 for non-uniform dispatch.
static inline int nvMTLFamilyBaseline(unsigned family)
{
    return family == NVMTLFamilyMac1 ||
           family == NVMTLFamilyMac2 ||
           family == NVMTLFamilyCommon1 ||
           family == NVMTLFamilyCommon2 ||
           family == NVMTLFamilyCommon3;
}

static inline int nvMTLFamilyAllowed(unsigned family, const NVMTLFamilyGates *g)
{
    if (nvMTLFamilyBaseline(family))
        return 1;
    if (!g)
        return 0;
    if (family == NVMTLFamilyMetal3)
        return g->metal3OptIn ? 1 : 0;
    if (family == NVMTLFamilyMetal4)
        return (g->metal4OptIn && g->metal4Family) ? 1 : 0;
    return 0;
}

static inline const char *nvMTLFamilyName(unsigned family)
{
    switch (family) {
    case NVMTLFamilyApple1:  return "Apple1";
    case NVMTLFamilyApple9:  return "Apple9";
    case NVMTLFamilyMac1:    return "Mac1";
    case NVMTLFamilyMac2:    return "Mac2";
    case NVMTLFamilyCommon1: return "Common1";
    case NVMTLFamilyCommon2: return "Common2";
    case NVMTLFamilyCommon3: return "Common3";
    case NVMTLFamilyMetal3:  return "Metal3";
    case NVMTLFamilyMetal4:  return "Metal4";
    default:                 return "unknown";
    }
}

#ifdef __OBJC__
// Implemented once in NVMTL4.m: reads the process environment and the
// IODeviceTree:/options NVRAM node. Kept out of the pure-C policy above so
// the host test can execute the same decision table without IOKit.
int nvMTLGateObjectsEnabled(void);
NVMTLFamilyGates nvMTLGateFamilyInputs(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // NVMTL_FAMILY_GATE_H
