#pragma once

// NVIDIA 570.144 GSP WPR ABI. Field order and constants follow the MIT-licensed
// upstream gsp_fw_wpr_meta.h and gsp_abi_check.c (see upstream/nvidia-open-570.144).
#include <stddef.h>
#include <stdint.h>

namespace nvgsp {

constexpr uint64_t kWprMetaMagic = 0xdc3aae21371a60b3ULL;
constexpr uint64_t kWprMetaRevision = 1;
constexpr uint64_t kWprMetaVerified = 0xa0a0a0a0a0a0a0a0ULL;
constexpr uint64_t kRadixPageSize = 4096;
constexpr uint64_t kRadixEntriesPerPage = 512;

struct WprMeta {
    uint64_t magic;
    uint64_t revision;
    uint64_t sysmemAddrOfRadix3Elf;
    uint64_t sizeOfRadix3Elf;
    uint64_t sysmemAddrOfBootloader;
    uint64_t sizeOfBootloader;
    uint64_t bootloaderCodeOffset;
    uint64_t bootloaderDataOffset;
    uint64_t bootloaderManifestOffset;
    uint64_t sysmemAddrOfSignature;
    uint64_t sizeOfSignature;
    uint64_t gspFwRsvdStart;
    uint64_t nonWprHeapOffset;
    uint64_t nonWprHeapSize;
    uint64_t gspFwWprStart;
    uint64_t gspFwHeapOffset;
    uint64_t gspFwHeapSize;
    uint64_t gspFwOffset;
    uint64_t bootBinOffset;
    uint64_t frtsOffset;
    uint64_t frtsSize;
    uint64_t gspFwWprEnd;
    uint64_t fbSize;
    uint64_t vgaWorkspaceOffset;
    uint64_t vgaWorkspaceSize;
    uint64_t bootCount;
    uint64_t partitionRpcAddr;
    uint16_t partitionRpcRequestOffset;
    uint16_t partitionRpcReplyOffset;
    uint32_t elfCodeOffset;
    uint32_t elfDataOffset;
    uint32_t elfCodeSize;
    uint32_t elfDataSize;
    uint32_t lsUcodeVersion;
    uint8_t gspFwHeapVfPartitionCount;
    uint8_t flags;
    uint8_t padding[2];
    uint32_t pmuReservedSize;
    uint64_t verified;
};

static_assert(sizeof(WprMeta) == 256, "GSP WPR metadata must be 256 bytes");
static_assert(offsetof(WprMeta, sysmemAddrOfRadix3Elf) == 16, "radix address ABI");
static_assert(offsetof(WprMeta, sysmemAddrOfBootloader) == 32, "bootloader address ABI");
static_assert(offsetof(WprMeta, gspFwWprStart) == 112, "WPR start ABI");
static_assert(offsetof(WprMeta, gspFwHeapSize) == 128, "heap size ABI");
static_assert(offsetof(WprMeta, bootCount) == 200, "boot count ABI");
static_assert(offsetof(WprMeta, flags) == 241, "flags ABI");
static_assert(offsetof(WprMeta, verified) == 248, "verified ABI");

struct RadixLayout {
    uint64_t pages[4];
    uint64_t offset[4];
    uint64_t tableBytes;
    uint64_t paddedDataBytes;
};

// Resolve a page index to its page-aligned bus address. Table pages are indexed
// from the start of the table allocation; image pages from the firmware buffer.
using PageAddress = bool (*)(void *context, uint64_t pageIndex, uint64_t *address);

// Compute the 4-level page layout from NVIDIA kernel_gsp.c. The caller must
// allocate the tables and fill each entry with the physical page address.
inline bool radixLayout(uint64_t imageBytes, RadixLayout *layout) {
    if (!layout || !imageBytes || imageBytes > UINT64_MAX - (kRadixPageSize - 1))
        return false;
    layout->pages[3] = (imageBytes + kRadixPageSize - 1) / kRadixPageSize;
    for (int i = 3; i > 0; --i)
        layout->pages[i - 1] = (layout->pages[i] + kRadixEntriesPerPage - 1) /
                               kRadixEntriesPerPage;
    if (layout->pages[0] != 1) return false;
    layout->offset[0] = 0;
    uint64_t tablePages = 0;
    for (int i = 1; i < 4; ++i) {
        tablePages += layout->pages[i - 1];
        layout->offset[i] = tablePages * kRadixPageSize;
    }
    layout->tableBytes = tablePages * kRadixPageSize;
    layout->paddedDataBytes = layout->pages[3] * kRadixPageSize;
    return true;
}

inline bool fillRadixTables(const RadixLayout &layout, void *tableMemory,
                            uint64_t capacity, PageAddress tablePage,
                            PageAddress imagePage, void *context) {
    if (!tableMemory || !tablePage || !imagePage || layout.pages[0] != 1 ||
        layout.tableBytes > capacity || (layout.tableBytes & 7)) return false;
    uint64_t *entries = static_cast<uint64_t *>(tableMemory);
    for (uint64_t i = 0; i < layout.tableBytes / 8; ++i) entries[i] = 0;
    for (int level = 0; level < 3; ++level) {
        const uint64_t count = layout.pages[level + 1];
        if (count > layout.pages[level] * kRadixEntriesPerPage) return false;
        const uint64_t first = layout.offset[level] / 8;
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t address = 0;
            const bool ok = level == 2
                ? imagePage(context, i, &address)
                : tablePage(context, layout.offset[level + 1] / kRadixPageSize + i,
                            &address);
            if (!ok || !address || (address & (kRadixPageSize - 1))) return false;
            entries[first + i] = address;
        }
    }
    return true;
}

}  // namespace nvgsp
