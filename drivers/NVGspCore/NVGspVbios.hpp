#pragma once

#include "NVGspFwsec.hpp"

namespace nvgsp {

struct VbiosFwsecView {
    FwsecView fwsec;
    uint32_t biosSize, expansionOffset, bitOffset, falconTableOffset;
    uint32_t descriptorOffset, dmaImageSize;
    uint8_t targetId;
};

inline bool rangeWithin(uint64_t offset, uint64_t size, uint64_t total) {
    return offset <= total && size <= total - offset;
}
inline uint16_t readLe16(const uint8_t *p) {
    return static_cast<uint16_t>(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
}
inline uint32_t readLe32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Port of NVIDIA's PCI Data Extension/BIT/Falcon walk. The returned pointers
// alias the caller-owned 1 MiB PROM snapshot and remain valid only with it.
inline bool parseVbiosFwsec(const void *romBytes, uint64_t romSize,
                            VbiosFwsecView *out) {
    if (!romBytes || !out || romSize < 512 || romSize > UINT32_MAX) return false;
    const uint8_t *rom = static_cast<const uint8_t *>(romBytes);
    uint32_t current = 0, baseSize = 0, extensionImage = 0, biosSize = 0;
    bool last = false;
    for (unsigned image = 0; image < 32 && !last; ++image) {
        if (!rangeWithin(uint64_t(current) + 0x18, 2, romSize)) return false;
        const uint64_t pcir64 = uint64_t(current) + readLe16(rom + current + 0x18);
        if (!rangeWithin(pcir64, 24, romSize)) return false;
        const uint32_t pcir = static_cast<uint32_t>(pcir64);
        const bool signature =
            (rom[pcir] == 'P' && rom[pcir + 1] == 'C' && rom[pcir + 2] == 'I' && rom[pcir + 3] == 'R') ||
            (rom[pcir] == 'N' && rom[pcir + 1] == 'P' && rom[pcir + 2] == 'D' && rom[pcir + 3] == 'S');
        if (!signature) return false;
        const uint16_t imageBlocks = readLe16(rom + pcir + 16);
        uint16_t subBlocks = imageBlocks;
        const uint8_t codeType = rom[pcir + 20];
        last = (rom[pcir + 21] & 0x80) != 0;
        const uint64_t ext64 = (uint64_t(pcir) + readLe16(rom + pcir + 10) + 15) & ~uint64_t(15);
        if (rangeWithin(ext64, 11, romSize)) {
            const uint32_t ext = static_cast<uint32_t>(ext64);
            if (rom[ext] == 'N' && rom[ext + 1] == 'P' &&
                rom[ext + 2] == 'D' && rom[ext + 3] == 'E') {
                const uint16_t revision = readLe16(rom + ext + 4);
                const uint16_t length = readLe16(rom + ext + 6);
                if (revision == 0x100 || revision == 0x101) {
                    subBlocks = readLe16(rom + ext + 8);
                    if (length >= 11) last = (rom[ext + 10] & 0x80) != 0;
                    else if (subBlocks < imageBlocks) last = false;
                }
            }
        }
        const uint64_t size = uint64_t(subBlocks) * 512;
        if (!size || !rangeWithin(current, size, romSize) ||
            uint64_t(current) + size > UINT32_MAX)
            return false;
        if (!baseSize && codeType == 0) baseSize = static_cast<uint32_t>(size);
        if (!extensionImage && codeType == 0xe0) extensionImage = current;
        current += static_cast<uint32_t>(size);
        biosSize = current;
    }
    if (!last || !baseSize || !extensionImage || extensionImage < baseSize)
        return false;
    const uint32_t expansion = extensionImage - baseSize;

    uint32_t bit = UINT32_MAX;
    for (uint32_t offset = 0; uint64_t(offset) + 12 <= biosSize; ++offset) {
        if (rom[offset] != 0xff || rom[offset + 1] != 0xb8 ||
            rom[offset + 2] != 'B' || rom[offset + 3] != 'I' ||
            rom[offset + 4] != 'T' || rom[offset + 5] != 0)
            continue;
        const uint8_t headerSize = rom[offset + 8];
        if (!rangeWithin(offset, headerSize, biosSize)) continue;
        uint8_t sum = 0;
        for (uint32_t i = 0; i < headerSize; ++i) sum = uint8_t(sum + rom[offset + i]);
        if (!sum) { bit = offset; break; }
    }
    if (bit == UINT32_MAX) return false;
    const uint8_t headerSize = rom[bit + 8];
    const uint8_t tokenSize = rom[bit + 9];
    const uint8_t tokenCount = rom[bit + 10];
    if (tokenSize != 6 && tokenSize != 8) return false;
    uint32_t falconData = UINT32_MAX;
    for (uint32_t index = 0; index < tokenCount; ++index) {
        const uint64_t token64 = uint64_t(bit) + headerSize + uint64_t(index) * tokenSize;
        if (!rangeWithin(token64, tokenSize, biosSize)) return false;
        const uint32_t token = static_cast<uint32_t>(token64);
        const uint8_t id = rom[token], version = rom[token + 1];
        const uint16_t size = readLe16(rom + token + 2);
        const uint32_t pointer = tokenSize == 6 ? readLe16(rom + token + 4)
                                                : readLe32(rom + token + 4);
        if (id == 0x70 && version == 2 && size >= 4) { falconData = pointer; break; }
    }
    if (falconData == UINT32_MAX || !rangeWithin(falconData, 4, biosSize)) return false;
    const uint64_t table64 = uint64_t(expansion) + readLe32(rom + falconData);
    if (!rangeWithin(table64, 6, biosSize)) return false;
    const uint32_t table = static_cast<uint32_t>(table64);
    const uint8_t version = rom[table], tableHeaderSize = rom[table + 1];
    const uint8_t entrySize = rom[table + 2], entryCount = rom[table + 3];
    if (version != 1 || tableHeaderSize < 6 || entrySize < 6) return false;

    uint32_t descriptor = UINT32_MAX;
    uint8_t target = 0;
    for (uint32_t index = 0; index < entryCount; ++index) {
        const uint64_t entry64 = uint64_t(table) + tableHeaderSize + uint64_t(index) * entrySize;
        if (!rangeWithin(entry64, 6, biosSize)) return false;
        const uint32_t entry = static_cast<uint32_t>(entry64);
        if (rom[entry] == 0x85) {
            target = rom[entry + 1];
            const uint64_t desc64 = uint64_t(expansion) + readLe32(rom + entry + 2);
            if (desc64 > UINT32_MAX) return false;
            descriptor = static_cast<uint32_t>(desc64);
            break;
        }
    }
    if (descriptor == UINT32_MAX || !rangeWithin(descriptor, sizeof(FwsecDescV3), biosSize))
        return false;
    FwsecDescV3 desc{};
    __builtin_memcpy(&desc, rom + descriptor, sizeof(desc));
    const uint32_t descriptorSize = desc.vdesc >> 16;
    if (!rangeWithin(descriptor, descriptorSize, biosSize)) return false;
    const uint64_t imageOffset = uint64_t(descriptor) + descriptorSize;
    const uint32_t dmaImageSize = (desc.storedSize + 255) & ~uint32_t(255);
    if (dmaImageSize < desc.storedSize || !rangeWithin(imageOffset, dmaImageSize, biosSize))
        return false;
    FwsecView fwsec{};
    if (!parseFwsecV3(rom + descriptor, descriptorSize,
                      rom + imageOffset, desc.storedSize, &fwsec)) return false;
    *out = VbiosFwsecView{fwsec, biosSize, expansion, bit, table,
                          descriptor, dmaImageSize, target};
    return true;
}

} // namespace nvgsp
