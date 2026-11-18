#include "../drivers/NVGspCore/NVGspPackage.hpp"
#include <cassert>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream f(argv[1], std::ios::binary);
    assert(f.good());
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), {});
    nvgsp::PackageView views[nvgsp::kPackageEntries]{};
    assert(nvgsp::parsePackage(data.data(), data.size(), views));
    nvgsp::PackageHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    assert(views[0].size >= 1u<<20 && views[1].size == 4096);
    auto bad = data; bad[0] = 0; assert(!nvgsp::parsePackage(bad.data(), bad.size(), views));
    bad = data; bad.resize(data.size() - 1); assert(!nvgsp::parsePackage(bad.data(), bad.size(), views));
    // Reject forged component counts and overlapping or overflowing ranges.
    bad = data; reinterpret_cast<nvgsp::PackageHeader *>(bad.data())->entryCount++;
    assert(!nvgsp::parsePackage(bad.data(), bad.size(), views));
    bad = data; reinterpret_cast<nvgsp::PackageEntry *>(bad.data()+32)[1].offset = 0;
    assert(!nvgsp::parsePackage(bad.data(), bad.size(), views));
    bad = data; reinterpret_cast<nvgsp::PackageEntry *>(bad.data()+32)[0].size = UINT64_MAX;
    assert(!nvgsp::parsePackage(bad.data(), bad.size(), views));
    bad = data; reinterpret_cast<nvgsp::PackageEntry *>(bad.data()+32)[1].type = 1;
    assert(!nvgsp::parsePackage(bad.data(), bad.size(), views));
    if (header.version == 2) {
        assert(nvgsp::parsePackage(data.data(), data.size(), views));
        assert(views[nvgsp::kPackageUcodes-1].size == 58215936);
        nvgsp::PackageView legacyViews[5]{};
        assert(!nvgsp::parsePackage(data.data(), data.size(), legacyViews));
        bad = data; reinterpret_cast<nvgsp::PackageEntry *>(bad.data()+32)[5].size = 0;
        assert(!nvgsp::parsePackage(bad.data(), bad.size(), views));
    }
    std::printf("GSP package %zu bytes: %u bounded components and malformed-input checks pass\n", data.size(),header.entryCount);
}
