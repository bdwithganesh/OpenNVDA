#include "../../NVGspCore/NVGspBoot.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>

int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    assert(input.good());
    unsigned char header[108]{};
    input.read(reinterpret_cast<char *>(header), sizeof(header));
    assert(input.gcount() == sizeof(header));
    nvgsp::BootUcodeDesc desc{};
    std::memcpy(&desc, header + 24, sizeof(desc));
    uint32_t wrapper[6]{};
    std::memcpy(wrapper, header, sizeof(wrapper));
    assert(wrapper[0] == 0x10de && wrapper[1] == 1);
    assert(wrapper[3] == 24 && wrapper[4] == 108 && wrapper[5] == 36864);
    assert(nvgsp::validBootUcodeDesc(desc, wrapper[5]));
    nvgsp::FbLayout fb{};
    assert(nvgsp::planAd103FbLayout(16376 * nvgsp::kMiB, false, 0,
                                    wrapper[5], 63541248, &fb));
    nvgsp::BootDma dma{0x100000, 0x200000, 0x300000, 4096};
    nvgsp::WprMeta meta{};
    assert(nvgsp::buildAd103WprMeta(fb, desc, wrapper[5], 63541248,
                                    dma, &meta));
    assert(meta.magic == nvgsp::kWprMetaMagic && meta.revision == 1);
    assert(meta.bootloaderCodeOffset == 18432);
    assert(meta.bootloaderDataOffset == 2048);
    assert(meta.bootloaderManifestOffset == 0);
    assert(meta.sizeOfSignature == 4096 && meta.verified == 0);
    assert(meta.gspFwWprStart == 0x3f3800000ULL);
    dma.signature = 0;
    assert(!nvgsp::buildAd103WprMeta(fb, desc, wrapper[5], 63541248,
                                     dma, &meta));
    std::printf("AD103 boot desc v%u, image %u bytes, WPR meta %zu bytes\n",
                desc.version, wrapper[5], sizeof(meta));
}
