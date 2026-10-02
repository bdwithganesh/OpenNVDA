#include "../drivers/NVGspCore/NVGspBooter.hpp"
#include <cassert>
#include <fstream>
#include <vector>
#include <cstdio>

int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), {});
    nvgsp::BooterView view{};
    assert(nvgsp::parseBooterLoad(data.data(), data.size(), &view));
    assert(view.imageSize == 56832 && view.signatureCount == 2);
    assert(nvgsp::signatureForFuse(view, 1) == view.signatures);
    assert(nvgsp::signatureForFuse(view, 0) == view.signatures + view.signatureSize);
    assert(nvgsp::signatureForFuse(view, 2) == nullptr);
    auto bad = data; bad[24 + 32] = 0; // corrupt 36-byte Falcon header size
    assert(!nvgsp::parseBooterLoad(bad.data(), bad.size(), &view));
    std::printf("Booter Load: image %u, %u signatures x %u, ucode %u engine 0x%x\n",
                view.imageSize, view.signatureCount, view.signatureSize,
                view.ucodeId, view.engineId);
}
