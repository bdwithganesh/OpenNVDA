#include "../../NVGspCore/NVGspPackage.hpp"
#include <cassert>
#include <fstream>
#include <vector>
#include <cstdio>

int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), {});
    nvgsp::PackageView views[5]{};
    assert(nvgsp::parsePackage(data.data(), data.size(), views));
    assert(views[0].size == 63541248 && views[1].size == 4096);
    auto bad = data; bad[0] = 0; assert(!nvgsp::parsePackage(bad.data(), bad.size(), views));
    bad = data; bad.resize(data.size() - 1); assert(!nvgsp::parsePackage(bad.data(), bad.size(), views));
    std::printf("GSP package %zu bytes: five bounded components\n", data.size());
}
