#include "../drivers/NVGspCore/NVGspSysmemFlush.hpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

struct FlushIo {
    std::map<uint32_t, uint32_t> registers;
    std::vector<uint32_t> writes;
    bool dropLo = false;
    unsigned reads = 0, failRead = 0;
    bool read(uint32_t offset, uint32_t *value) {
        if (++reads == failRead) return false;
        *value = registers[offset]; return true;
    }
    bool write(uint32_t offset, uint32_t value) {
        writes.push_back(offset);
        if (!(dropLo && offset == 0x100c10)) registers[offset] = value;
        return true;
    }
};

int main() {
    uint32_t lo = 0, hi = 0;
    FlushIo io;
    io.registers[0x100c40] = 0xa5000000;
    const uint64_t address = 0x712345678000ULL;
    assert(nvgsp::programSysmemFlush(io, address, &lo, &hi));
    assert(lo == 0x23456780 && hi == 0xa5000071);
    assert((((uint64_t(hi) & 0x7f) << 40) | (uint64_t(lo) << 8)) == address);
    assert((io.writes == std::vector<uint32_t>{0x100c40, 0x100c10}));
    // The old, absent programming leaves both fields zero. Lost writes or
    // failed reads must never be promoted to a successful firmware boot.
    FlushIo lost; lost.dropLo = true;
    assert(!nvgsp::programSysmemFlush(lost, address, &lo, &hi));
    for (unsigned read = 1; read <= 3; ++read) {
        FlushIo failed; failed.failRead = read;
        assert(!nvgsp::programSysmemFlush(failed, address, &lo, &hi));
    }
    for (uint64_t bad : {0ULL, 1ULL, 0x1001ULL, 1ULL << 47, ~0ULL - 4095}) {
        FlushIo invalid;
        assert(!nvgsp::programSysmemFlush(invalid, bad, &lo, &hi));
        assert(invalid.writes.empty());
    }
    FlushIo boundary;
    assert(nvgsp::programSysmemFlush(boundary, (1ULL << 47) - 4096, &lo, &hi));
    assert(lo == 0xfffffff0 && hi == 0x7f);
    assert(!nvgsp::programSysmemFlush(boundary, address, nullptr, &hi));
    std::puts("sysmem flush: 47-bit address, reserved bits, ordering, readback and invalid inputs pass");
}
