#pragma once

// Ada/AD103 bare-metal GSP framebuffer layout from NVIDIA 570.144
// kgspCalculateFbLayout_TU102 and AD103 HAL constants. Offline planner only.
#include "NVGspAbi.hpp"

namespace nvgsp {

constexpr uint64_t kMiB = 1ULL << 20;
constexpr uint64_t kGiB = 1ULL << 30;

struct FbLayout {
    uint64_t fbSize;
    uint64_t vgaWorkspaceOffset;
    uint64_t vgaWorkspaceSize;
    uint64_t gspFwWprEnd;
    uint64_t frtsOffset;
    uint64_t frtsSize;
    uint64_t bootBinOffset;
    uint64_t gspFwOffset;
    uint64_t gspFwHeapOffset;
    uint64_t gspFwHeapSize;
    uint64_t gspFwWprStart;
    uint64_t nonWprHeapOffset;
    uint64_t nonWprHeapSize;
    uint64_t gspFwRsvdStart;
};

inline uint64_t alignDown(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

inline uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// The AD103 HAL uses a 22 MiB OS carveout, 8 MiB base, 96 KiB per GiB of
// usable FB rounded to MiB, and 96 MiB for 2048 client allocations. Its
// bare-metal min/max are 88/280 MiB. AD103 supports scrubber ucode, so the
// pre-scrubbed-top-FB cap is not applied by kgspGetFwHeapSize_IMPL.
inline uint64_t ad103HeapSize(uint64_t fbSize) {
    if (!fbSize || fbSize > UINT64_MAX - (kGiB - 1)) return 0;
    const uint64_t fbGiB = (fbSize + kGiB - 1) / kGiB;
    if (fbGiB > UINT64_MAX / (96 * 1024)) return 0;
    const uint64_t size = 22 * kMiB + 8 * kMiB +
                          alignUp(fbGiB * 96 * 1024, kMiB) + 96 * kMiB;
    if (size < 88 * kMiB) return 88 * kMiB;
    return size > 280 * kMiB ? 280 * kMiB : size;
}

// workspaceValid/Offset are decoded from NV_PDISP_VGA_WORKSPACE_BASE. The
// AD103 path has no valid MMU lock region in NVIDIA 570.144. Normal boot uses
// zero WPR end margin and a 1 MiB FRTS allocation.
inline bool planAd103FbLayout(uint64_t fbSize, bool workspaceValid,
                              uint64_t workspaceOffset, uint64_t bootloaderBytes,
                              uint64_t fwImageBytes, FbLayout *out) {
    if (!out || fbSize < 256 * kMiB || !bootloaderBytes || !fwImageBytes ||
        fbSize & (kMiB - 1)) return false;
    FbLayout layout{};
    layout.fbSize = fbSize;
    if (workspaceValid) {
        if (workspaceOffset > fbSize) return false;
        layout.vgaWorkspaceOffset = workspaceOffset < fbSize - kMiB
            ? fbSize - 0x20000 : workspaceOffset;
    } else {
        layout.vgaWorkspaceOffset = fbSize - kMiB;
    }
    layout.vgaWorkspaceSize = fbSize - layout.vgaWorkspaceOffset;
    layout.gspFwWprEnd = alignDown(layout.vgaWorkspaceOffset, 0x20000);
    layout.frtsSize = kMiB;
    if (layout.gspFwWprEnd < layout.frtsSize + bootloaderBytes) return false;
    layout.frtsOffset = layout.gspFwWprEnd - layout.frtsSize;
    layout.bootBinOffset = alignDown(layout.frtsOffset - bootloaderBytes, 4096);
    if (layout.bootBinOffset < fwImageBytes) return false;
    layout.gspFwOffset = alignDown(layout.bootBinOffset - fwImageBytes, 0x10000);
    const uint64_t heapSize = ad103HeapSize(fbSize);
    if (!heapSize || layout.gspFwOffset < heapSize + 2 * kMiB) return false;
    layout.gspFwHeapOffset = alignDown(layout.gspFwOffset - heapSize, kMiB);
    layout.gspFwHeapSize = alignDown(layout.gspFwOffset - layout.gspFwHeapOffset, kMiB);
    layout.gspFwWprStart = layout.gspFwHeapOffset - kMiB;  // metadata region
    layout.nonWprHeapSize = kMiB;
    layout.nonWprHeapOffset = layout.gspFwWprStart - layout.nonWprHeapSize;
    layout.gspFwRsvdStart = layout.nonWprHeapOffset;
    *out = layout;
    return true;
}

}  // namespace nvgsp
