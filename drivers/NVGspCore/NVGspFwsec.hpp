#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nvgsp {

struct FwsecDescV3 {
    uint32_t vdesc, storedSize, pkcDataOffset, interfaceOffset;
    uint32_t imemPhysBase, imemLoadSize, imemVirtBase;
    uint32_t dmemPhysBase, dmemLoadSize;
    uint16_t engineIdMask;
    uint8_t ucodeId, signatureCount;
    uint16_t signatureVersions, reserved;
};
struct FwsecView {
    FwsecDescV3 desc;
    const uint8_t *image;
    const uint8_t *signatures;
    uint32_t signatureSize;
};
struct FalconAppInterfaceHeaderV1 {
    uint8_t version, headerSize, entrySize, entryCount;
};
struct FalconAppInterfaceEntryV1 {
    uint32_t id, dmemOffset;
};
struct FalconDmemMapperV3 {
    uint32_t signature;
    uint16_t version, size;
    uint32_t cmdInBufferOffset, cmdInBufferSize;
    uint32_t cmdOutBufferOffset, cmdOutBufferSize;
    uint32_t nvfImageDataBufferOffset, nvfImageDataBufferSize;
    uint32_t printfBufferHeader, ucodeBuildTimestamp, ucodeSignature;
    uint32_t initCommand, ucodeFeature, ucodeCommandMask0, ucodeCommandMask1;
    uint32_t multiTargetTable;
};
struct FwsecReadVbiosDesc {
    uint32_t version, size;
    uint64_t gfwImageOffset;
    uint32_t gfwImageSize, flags;
};
struct FwsecFrtsRegionDesc {
    uint32_t version, size, offset4K, size4K, mediaType;
};
struct FwsecFrtsCommand {
    FwsecReadVbiosDesc readVbios;
    FwsecFrtsRegionDesc region;
};
struct FwsecPatchInfo {
    uint32_t selectedSignature, signatureOffset, interfaceOffset;
    uint32_t mapperOffset, commandOffset;
};
static_assert(sizeof(FwsecDescV3) == 44, "FWSEC v3 descriptor ABI");
static_assert(sizeof(FalconAppInterfaceHeaderV1) == 4, "FWSEC app header ABI");
static_assert(sizeof(FalconAppInterfaceEntryV1) == 8, "FWSEC app entry ABI");
static_assert(sizeof(FalconDmemMapperV3) == 64, "FWSEC DMEM mapper ABI");
static_assert(sizeof(FwsecReadVbiosDesc) == 24, "FWSEC VBIOS descriptor ABI");
static_assert(sizeof(FwsecFrtsRegionDesc) == 20, "FWSEC FRTS descriptor ABI");
static_assert(sizeof(FwsecFrtsCommand) == 48, "FWSEC FRTS command ABI");

inline bool parseFwsecV3(const void *descriptor, uint64_t descriptorBytes,
                         const void *image, uint64_t imageBytes,
                         FwsecView *out) {
    if (!descriptor || !image || !out || descriptorBytes < sizeof(FwsecDescV3))
        return false;
    FwsecDescV3 desc{};
    __builtin_memcpy(&desc, descriptor, sizeof(desc));
    const uint32_t version = (desc.vdesc >> 8) & 0xff;
    const uint32_t declaredBytes = desc.vdesc >> 16;
    if (version != 3 || declaredBytes != descriptorBytes ||
        desc.storedSize != imageBytes || !desc.signatureCount ||
        descriptorBytes != sizeof(desc) + desc.signatureCount * 384ULL ||
        desc.imemLoadSize > imageBytes || desc.dmemLoadSize > imageBytes - desc.imemLoadSize ||
        desc.pkcDataOffset > desc.dmemLoadSize ||
        384 > desc.dmemLoadSize - desc.pkcDataOffset ||
        desc.interfaceOffset >= desc.dmemLoadSize || desc.ucodeId == 0)
        return false;
    *out = FwsecView{desc, static_cast<const uint8_t *>(image),
                     static_cast<const uint8_t *>(descriptor) + sizeof(desc), 384};
    return true;
}

inline const uint8_t *fwsecSignatureForFuse(const FwsecView &view,
                                             uint32_t fuseVersion) {
    if (fuseVersion >= 16 || !(view.desc.signatureVersions & (1U << fuseVersion)))
        return nullptr;
    uint32_t index = 0;
    for (uint32_t bit = 0; bit < fuseVersion; ++bit)
        if (view.desc.signatureVersions & (1U << bit)) ++index;
    if (index >= view.desc.signatureCount) return nullptr;
    return view.signatures + index * view.signatureSize;
}

// Build the exact host-side image that NVIDIA's FWSEC path submits to the
// Falcon: copy IMEM+DMEM, patch the fuse-selected RSA-3K signature into DMEM,
// select command 0x15, and place the 1 MiB FRTS command in cmd_in_buffer.
inline bool patchFwsecFrts(const FwsecView &view, uint32_t fuseVersion,
                           uint64_t frtsOffset, void *output,
                           uint64_t outputBytes, FwsecPatchInfo *info = nullptr,
                           uint32_t initCommand = 0x15) {
    if (!output || outputBytes < view.desc.storedSize || (frtsOffset & 4095) ||
        (frtsOffset >> 12) > UINT32_MAX || view.desc.imemLoadSize > view.desc.storedSize ||
        view.desc.dmemLoadSize > view.desc.storedSize - view.desc.imemLoadSize)
        return false;
    const uint8_t *signature = fwsecSignatureForFuse(view, fuseVersion);
    if (!signature) return false;

    uint8_t *image = static_cast<uint8_t *>(output);
    __builtin_memcpy(image, view.image, view.desc.storedSize);
    uint8_t *dmem = image + view.desc.imemLoadSize;
    const uint32_t dmemBytes = view.desc.dmemLoadSize;
    if (view.desc.pkcDataOffset > dmemBytes ||
        view.signatureSize > dmemBytes - view.desc.pkcDataOffset ||
        view.desc.interfaceOffset > dmemBytes ||
        sizeof(FalconAppInterfaceHeaderV1) > dmemBytes - view.desc.interfaceOffset)
        return false;

    FalconAppInterfaceHeaderV1 header{};
    __builtin_memcpy(&header, dmem + view.desc.interfaceOffset, sizeof(header));
    if (header.version != 1 || header.headerSize != sizeof(header) ||
        header.entrySize != sizeof(FalconAppInterfaceEntryV1) || header.entryCount < 2)
        return false;
    const uint64_t entriesOffset = uint64_t(view.desc.interfaceOffset) + header.headerSize;
    const uint64_t entriesBytes = uint64_t(header.entryCount) * header.entrySize;
    if (entriesOffset > dmemBytes || entriesBytes > dmemBytes - entriesOffset)
        return false;

    uint32_t mapperOffset = UINT32_MAX;
    for (uint32_t index = 0; index < header.entryCount; ++index) {
        FalconAppInterfaceEntryV1 entry{};
        __builtin_memcpy(&entry, dmem + entriesOffset + uint64_t(index) * header.entrySize,
                         sizeof(entry));
        if (entry.id == 4) mapperOffset = entry.dmemOffset;
    }
    if (mapperOffset == UINT32_MAX || mapperOffset > dmemBytes ||
        sizeof(FalconDmemMapperV3) > dmemBytes - mapperOffset)
        return false;

    FalconDmemMapperV3 mapper{};
    __builtin_memcpy(&mapper, dmem + mapperOffset, sizeof(mapper));
    if (mapper.signature != 0x50414d44 || mapper.version != 3 ||
        mapper.size < sizeof(mapper) || mapper.cmdInBufferSize < sizeof(FwsecFrtsCommand) ||
        mapper.cmdInBufferOffset > dmemBytes ||
        sizeof(FwsecFrtsCommand) > dmemBytes - mapper.cmdInBufferOffset)
        return false;

    FwsecFrtsCommand command{};
    command.readVbios.version = 1;
    command.readVbios.size = sizeof(FwsecReadVbiosDesc);
    command.readVbios.flags = 2;
    command.region.version = 1;
    command.region.size = sizeof(FwsecFrtsRegionDesc);
    command.region.offset4K = static_cast<uint32_t>(frtsOffset >> 12);
    command.region.size4K = 0x100;
    command.region.mediaType = 2;
    // 0x15 = FRTS (boot), 0x19 = SB (unload: restore pre-OS apps, nouveau
    // NVFW_FALCON_APPIF_DMEMMAPPER_CMD_SB), SB has no command body.
    mapper.initCommand = initCommand;

    __builtin_memcpy(dmem + view.desc.pkcDataOffset, signature, view.signatureSize);
    __builtin_memcpy(dmem + mapperOffset, &mapper, sizeof(mapper));
    if (initCommand == 0x15)
        __builtin_memcpy(dmem + mapper.cmdInBufferOffset, &command, sizeof(command));
    if (info) {
        uint32_t selected = 0;
        while (view.signatures + selected * view.signatureSize != signature) ++selected;
        *info = FwsecPatchInfo{selected, view.desc.pkcDataOffset,
                               view.desc.interfaceOffset, mapperOffset,
                               mapper.cmdInBufferOffset};
    }
    return true;
}

} // namespace nvgsp
