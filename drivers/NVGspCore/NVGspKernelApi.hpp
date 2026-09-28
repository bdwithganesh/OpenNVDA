// In-kernel API between NVAccelerator (Aux KC) and NVGspControl (Boot KC),
// reached with IOService::callPlatformFunction so neither links the other.
// 
//   "nvgsp-stamp-region"   p1 = UInt64[3] out {VRAM/BAR1 offset, GPU VA, bytes},
//                          p2 = IOMemoryDescriptor ** out (retained BAR1 view) or null
//   "nvgsp-stamp-register" p1 = NVGspStampFn, p2 = ref (p1 null: unregister)
//   "nvgsp-submit-stamp"   p1 = NVGspKernelSubmit *
// 
//   "nvgsp-flip-copy"      p1 = NVGspFlipCopy *: a flipped surface to scan-out
//                          on the kernel copy engine, after pending GR work
#pragma once
#include <libkern/OSTypes.h>
class IOMemoryDescriptor;

// engineMask bit 0 = GR fence interrupt, bit 1 = CE. Called on the MSI
// workloop with NVGspControl's lock dropped; may take the caller's locks.
typedef void (*NVGspStampFn)(void *ref, UInt32 engineMask);

struct NVGspKernelSubmit {
    UInt32 version;          // 1
    UInt32 engine;           // 0 GR, 1 CE (execSegments numbering)
    const void *owner;       // the process's NVGspControl client (its VA arena)
    UInt32 n;                // segments, <= kExecMaxSegments
    const UInt64 *va;        // pushbuffer segments (GPU VA in the owner's arena)
    const UInt32 *dwords;
    const UInt32 *flags;
    UInt64 stampVa;          // GPU VA inside the stamp region, 0 = none
    UInt32 stampValue;       // released there before the ring's own fence
    UInt32 seqOut;           // ring fence sequence
};

struct NVGspFlipCopy {
    UInt32 version;          // 1
    UInt32 rows;
    IOMemoryDescriptor *src; // wired (prepared) surface memory, system RAM
    UInt64 srcOffset;
    UInt32 srcRowBytes;
    UInt32 widthBytes;       // bytes copied per row
    UInt64 dstBus;           // scan-out: physical (bus) address inside BAR1
    UInt32 dstRowBytes;
    UInt32 copiesOut;        // copy-engine lines used (diagnostics)
};
