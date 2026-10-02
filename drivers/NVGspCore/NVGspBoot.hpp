#pragma once

// NVIDIA 570.144 RM_RISCV_UCODE_DESC and normal AD103 WPR metadata inputs.
// This does not touch the device; all bus addresses must come from a DMA mapper.
#include "NVGspFbLayout.hpp"

namespace nvgsp {

struct BootUcodeDesc {
    uint32_t version, bootloaderOffset, bootloaderSize;
    uint32_t bootloaderParamOffset, bootloaderParamSize;
    uint32_t riscvElfOffset, riscvElfSize, appVersion;
    uint32_t manifestOffset, manifestSize;
    uint32_t monitorDataOffset, monitorDataSize;
    uint32_t monitorCodeOffset, monitorCodeSize, monitorEnabled;
    uint32_t swbromCodeOffset, swbromCodeSize;
    uint32_t swbromDataOffset, swbromDataSize;
    uint32_t fbReservedSize, signedAsCode;
};
static_assert(sizeof(BootUcodeDesc) == 84, "NVIDIA RISC-V descriptor ABI");
static_assert(offsetof(BootUcodeDesc, manifestOffset) == 32, "manifest ABI");
static_assert(offsetof(BootUcodeDesc, monitorCodeOffset) == 48, "monitor ABI");

struct BootDma {
    uint64_t radixRoot;
    uint64_t bootloader;
    uint64_t signature;
    uint64_t signatureBytes;
};

inline bool imageRange(uint64_t offset, uint64_t size, uint64_t imageBytes) {
    return size && offset <= imageBytes && size <= imageBytes - offset;
}

inline bool validBootUcodeDesc(const BootUcodeDesc &desc,
                               uint64_t imageBytes) {
    return desc.version == 5 && desc.monitorEnabled == 1 &&
           imageRange(desc.bootloaderOffset, desc.bootloaderSize, imageBytes) &&
           imageRange(desc.bootloaderParamOffset, desc.bootloaderParamSize, imageBytes) &&
           imageRange(desc.manifestOffset, desc.manifestSize, imageBytes) &&
           imageRange(desc.monitorDataOffset, desc.monitorDataSize, imageBytes) &&
           imageRange(desc.monitorCodeOffset, desc.monitorCodeSize, imageBytes) &&
           desc.fbReservedSize == imageBytes;
}

inline bool buildAd103WprMeta(const FbLayout &fb, const BootUcodeDesc &desc,
                              uint64_t bootImageBytes, uint64_t fwImageBytes,
                              const BootDma &dma, WprMeta *out) {
    if (!out || !validBootUcodeDesc(desc, bootImageBytes) ||
        !dma.radixRoot || !dma.bootloader || !dma.signature ||
        dma.signatureBytes != 4096 ||
        (dma.radixRoot & 4095) || (dma.bootloader & 4095) ||
        (dma.signature & 4095) ||
        fb.gspFwOffset < fb.gspFwWprStart ||
        fb.gspFwOffset > fb.bootBinOffset ||
        fwImageBytes > fb.bootBinOffset - fb.gspFwOffset ||
        fb.bootBinOffset > fb.frtsOffset ||
        bootImageBytes > fb.frtsOffset - fb.bootBinOffset)
        return false;

    WprMeta meta{};
    meta.magic = kWprMetaMagic;
    meta.revision = kWprMetaRevision;
    meta.sysmemAddrOfRadix3Elf = dma.radixRoot;
    meta.sizeOfRadix3Elf = fwImageBytes;
    meta.sysmemAddrOfBootloader = dma.bootloader;
    meta.sizeOfBootloader = bootImageBytes;
    meta.bootloaderCodeOffset = desc.monitorCodeOffset;
    meta.bootloaderDataOffset = desc.monitorDataOffset;
    meta.bootloaderManifestOffset = desc.manifestOffset;
    meta.sysmemAddrOfSignature = dma.signature;
    meta.sizeOfSignature = dma.signatureBytes;
    meta.gspFwRsvdStart = fb.gspFwRsvdStart;
    meta.nonWprHeapOffset = fb.nonWprHeapOffset;
    meta.nonWprHeapSize = fb.nonWprHeapSize;
    meta.gspFwWprStart = fb.gspFwWprStart;
    meta.gspFwHeapOffset = fb.gspFwHeapOffset;
    meta.gspFwHeapSize = fb.gspFwHeapSize;
    meta.gspFwOffset = fb.gspFwOffset;
    meta.bootBinOffset = fb.bootBinOffset;
    meta.frtsOffset = fb.frtsOffset;
    meta.frtsSize = fb.frtsSize;
    meta.gspFwWprEnd = fb.gspFwWprEnd;
    meta.fbSize = fb.fbSize;
    meta.vgaWorkspaceOffset = fb.vgaWorkspaceOffset;
    meta.vgaWorkspaceSize = fb.vgaWorkspaceSize;
    // NVIDIA initializes these to zero for a first normal boot.
    meta.bootCount = 0;
    meta.verified = 0;
    *out = meta;
    return true;
}

} // namespace nvgsp
