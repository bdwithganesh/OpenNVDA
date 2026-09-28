#include "../../NVGspCore/NVGspRegistry.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

static uint32_t rd(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

int main() {
    uint8_t buf[4096];
    uint32_t bytes = 0, n = 0;
    // empty spec: just the 8-byte header (what the kext used to send before
    // registry support)
    assert(nvgsp::buildRegistry("", 0, buf, sizeof(buf), &bytes, &n));
    assert(bytes == 8 && n == 0 && rd(buf) == 8 && rd(buf + 4) == 0);

    const char *spec = " RMSecBusResetEnable = 1 ; bad; =7; k=;RMFoo=0x1F;RMStr=hello ";
    assert(nvgsp::buildRegistry(spec, (uint32_t)std::strlen(spec), buf, sizeof(buf), &bytes, &n));
    assert(n == 3 && rd(buf + 4) == 3 && rd(buf) == bytes);
    // entry 0: DWORD 1
    const uint8_t *e0 = buf + 8, *e1 = buf + 24, *e2 = buf + 40;
    assert(e0[4] == nvgsp::kRegistryTypeDword && rd(e0 + 8) == 1 && rd(e0 + 12) == 4);
    assert(!std::strcmp((const char *)buf + rd(e0), "RMSecBusResetEnable"));
    assert(e1[4] == nvgsp::kRegistryTypeDword && rd(e1 + 8) == 0x1f);
    assert(!std::strcmp((const char *)buf + rd(e1), "RMFoo"));
    // entry 2: STRING, data = offset of the value
    assert(e2[4] == nvgsp::kRegistryTypeString && rd(e2 + 12) == 6);
    assert(!std::strcmp((const char *)buf + rd(e2), "RMStr"));
    assert(!std::strcmp((const char *)buf + rd(e2 + 8), "hello"));
    // name of entry 0 directly follows the entry array
    assert(rd(e0) == 8 + 3 * 16);
    const uint32_t expect = 8 + 3 * 16 + 20 + 6 + 6 + 6;
    assert(bytes == expect);

    // Too small a buffer / too many entries / overflowing numbers
    assert(!nvgsp::buildRegistry("A=1;B=2", 7, buf, 20, &bytes, &n));
    char many[1024] = {0};
    for (int i = 0; i < 33; ++i) std::sprintf(many + std::strlen(many), "K%d=%d;", i, i);
    assert(!nvgsp::buildRegistry(many, (uint32_t)std::strlen(many), buf, sizeof(buf), &bytes, &n));
    uint32_t v;
    assert(!nvgsp::registryParseNumber("4294967296", 10, &v));
    assert(nvgsp::registryParseNumber("4294967295", 10, &v) && v == 0xffffffffu);
    assert(!nvgsp::registryParseNumber("0x", 2, &v));
    // "0x" alone is a string value, not a number
    assert(nvgsp::buildRegistry("K=0x", 4, buf, sizeof(buf), &bytes, &n) && n == 1 &&
           buf[8 + 4] == nvgsp::kRegistryTypeString);
    std::puts("nvgsp_registry_check: PASS");
    return 0;
}
