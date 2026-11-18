#pragma once

// Single userspace-to-kernel payload for the version-paired GSP components.
#include <stddef.h>
#include <stdint.h>
#include "NVGspBooter.hpp"

namespace nvgsp {

constexpr uint8_t kPackageMagic[8] = {'N','V','G','S','P','K','G',0};
constexpr uint32_t kPackageVersion = 2;
constexpr uint32_t kPackageEntries = 6;
constexpr uint64_t kPackageCapacity = 128ULL << 20;
enum PackageType : uint32_t {
    kPackageFwImage = 1, kPackageSignature = 2, kPackageBootImage = 3,
    kPackageBootDescriptor = 4, kPackageBooterLoad = 5, kPackageUcodes = 6
};

struct PackageHeader {
    uint8_t magic[8];
    uint32_t version;
    uint32_t entryCount;
    uint64_t totalBytes;
    uint64_t reserved;
};
struct PackageEntry {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
    uint64_t reserved;
};
struct PackageView { const uint8_t *data; uint64_t size; };
static_assert(sizeof(PackageHeader) == 32, "package header ABI");
static_assert(sizeof(PackageEntry) == 32, "package entry ABI");

template <size_t N>
inline bool parsePackage(const void *bytes, uint64_t length,
                         PackageView (&views)[N]) {
    if (!bytes || length > kPackageCapacity ||
        length < sizeof(PackageHeader) + 5 * sizeof(PackageEntry))
        return false;
    const uint8_t *raw = static_cast<const uint8_t *>(bytes);
    PackageHeader header{};
    __builtin_memcpy(&header, raw, sizeof(header));
    for (size_t i = 0; i < sizeof(kPackageMagic); ++i)
        if (header.magic[i] != kPackageMagic[i]) return false;
    const unsigned count = header.version == 1 ? 5 :
                           header.version == kPackageVersion ? kPackageEntries : 0;
    if (!count || count > N || header.entryCount != count ||
        length < sizeof(PackageHeader) + count * sizeof(PackageEntry) ||
        header.totalBytes != length || header.reserved != 0)
        return false;
    for (size_t i = 0; i < N; ++i) views[i] = PackageView{nullptr, 0};
    const uint64_t payloadStart = sizeof(header) + count * sizeof(PackageEntry);
    uint64_t previousEnd = payloadStart;
    for (unsigned i = 0; i < count; ++i) {
        PackageEntry entry{};
        __builtin_memcpy(&entry, raw + sizeof(header) + i * sizeof(entry), sizeof(entry));
        if (entry.type < kPackageFwImage || entry.type > count ||
            entry.flags || entry.reserved || entry.offset < previousEnd ||
            (entry.offset & 7) || entry.offset > length ||
            entry.size > length - entry.offset || !entry.size)
            return false;
        PackageView &view = views[entry.type - 1];
        if (view.data) return false;
        view = PackageView{raw + entry.offset, entry.size};
        previousEnd = entry.offset + entry.size;
    }
    for (unsigned i = 0; i < count; ++i) if (!views[i].data) return false;
    if (count == kPackageEntries &&
        (views[count - 1].size < 4096 ||
         views[count - 1].size > (96ULL << 20))) return false;
    BooterView booter{};
    // Version-agnostic: the 570.144 package carries a 63.5 MiB .fwimage and
    // the 615.78.05 package a 27.0 MiB one.  The WPR planner validates that
    // the image actually fits the framebuffer layout.
    return views[kPackageFwImage - 1].size >= (1U << 20) &&
           views[kPackageFwImage - 1].size <= (256U << 20) &&
           views[kPackageSignature - 1].size == 4096 &&
           views[kPackageBootImage - 1].size >= 0x1000 &&
           views[kPackageBootImage - 1].size <= (1U << 20) &&
           views[kPackageBootDescriptor - 1].size == 84 &&
           views[kPackageBooterLoad - 1].size < (1U << 20) &&
           parseBooterLoad(views[kPackageBooterLoad - 1].data,
                           views[kPackageBooterLoad - 1].size, &booter);
}

} // namespace nvgsp
