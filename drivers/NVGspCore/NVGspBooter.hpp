#pragma once

// Nouveau-compatible HS Falcon wrapper emitted by NVIDIA's 570.144 extractor.
#include <stddef.h>
#include <stdint.h>

namespace nvgsp {

struct FirmwareWrapperHeader {
    uint32_t vendor, version, totalSize, descriptorOffset, imageOffset, imageSize;
};
struct HsHeaderV2 {
    uint32_t signaturesOffset, signaturesSize;
    uint32_t patchLocationOffset, patchSignatureOffset;
    uint32_t metadataOffset, metadataSize;
    uint32_t signatureCountOffset, headerOffset, headerSize;
};
struct FalconLayout {
    uint32_t osCodeOffset, osCodeSize, osDataOffset, osDataSize, appCount;
    uint32_t appCodeOffset, appCodeSize, appDataOffset, appDataSize;
};
struct BooterView {
    const uint8_t *image;
    uint32_t imageSize;
    const uint8_t *signatures;
    uint32_t signatureSize;
    uint32_t signatureCount;
    uint32_t patchLocation;
    uint32_t fuseVersion;
    uint32_t engineId;
    uint32_t ucodeId;
    FalconLayout layout;
};
static_assert(sizeof(FirmwareWrapperHeader) == 24, "wrapper ABI");
static_assert(sizeof(HsHeaderV2) == 36, "HS header ABI");
static_assert(sizeof(FalconLayout) == 36, "Falcon layout ABI");

inline bool contained(uint32_t offset, uint32_t size, uint64_t bytes) {
    return offset <= bytes && size <= bytes - offset;
}

inline bool parseBooterLoad(const void *data, uint64_t bytes, BooterView *out) {
    if (!data || !out || bytes < sizeof(FirmwareWrapperHeader) + sizeof(HsHeaderV2))
        return false;
    const uint8_t *raw = static_cast<const uint8_t *>(data);
    FirmwareWrapperHeader wrapper{};
    HsHeaderV2 hs{};
    __builtin_memcpy(&wrapper, raw, sizeof(wrapper));
    __builtin_memcpy(&hs, raw + sizeof(wrapper), sizeof(hs));
    if (wrapper.vendor != 0x10de || wrapper.version != 1 ||
        wrapper.descriptorOffset != sizeof(wrapper) ||
        wrapper.totalSize < bytes ||
        !contained(wrapper.imageOffset, wrapper.imageSize, bytes) ||
        hs.headerSize != sizeof(FalconLayout) || hs.metadataSize != 12 ||
        !contained(hs.signaturesOffset, hs.signaturesSize, wrapper.imageOffset) ||
        !contained(hs.patchLocationOffset, 4, wrapper.imageOffset) ||
        !contained(hs.patchSignatureOffset, 4, wrapper.imageOffset) ||
        !contained(hs.metadataOffset, 12, wrapper.imageOffset) ||
        !contained(hs.signatureCountOffset, 4, wrapper.imageOffset) ||
        !contained(hs.headerOffset, hs.headerSize, wrapper.imageOffset))
        return false;
    uint32_t patchLocation = 0, patchSignature = 0, meta[3]{}, signatureCount = 0;
    FalconLayout layout{};
    __builtin_memcpy(&patchLocation, raw + hs.patchLocationOffset, 4);
    __builtin_memcpy(&patchSignature, raw + hs.patchSignatureOffset, 4);
    __builtin_memcpy(meta, raw + hs.metadataOffset, sizeof(meta));
    __builtin_memcpy(&signatureCount, raw + hs.signatureCountOffset, 4);
    __builtin_memcpy(&layout, raw + hs.headerOffset, sizeof(layout));
    if (patchSignature != 0 || !signatureCount ||
        hs.signaturesSize % signatureCount || layout.appCount != 1 ||
        patchLocation < layout.osDataOffset || patchLocation >= wrapper.imageSize ||
        !contained(layout.osCodeOffset, layout.osCodeSize, wrapper.imageSize) ||
        !contained(layout.osDataOffset, layout.osDataSize, wrapper.imageSize) ||
        !contained(layout.appCodeOffset, layout.appCodeSize, wrapper.imageSize) ||
        !contained(layout.appDataOffset, layout.appDataSize, wrapper.imageSize))
        return false;
    *out = BooterView{raw + wrapper.imageOffset, wrapper.imageSize,
                      raw + hs.signaturesOffset,
                      hs.signaturesSize / signatureCount, signatureCount,
                      patchLocation, meta[0], meta[1], meta[2], layout};
    return true;
}

inline const uint8_t *signatureForFuse(const BooterView &view,
                                       uint32_t fuseVersion) {
    if (!view.signatures || fuseVersion >= view.signatureCount) return nullptr;
    const uint32_t index = view.signatureCount - 1 - fuseVersion;
    return view.signatures + static_cast<uint64_t>(index) * view.signatureSize;
}

} // namespace nvgsp
