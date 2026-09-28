#include "../../NVGspCore/NVGspAbi.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

static bool tablePage(void *, uint64_t index, uint64_t *address) {
    *address = 0x10000000 + index * 4096;
    return true;
}

static bool imagePage(void *, uint64_t index, uint64_t *address) {
    *address = 0x20000000 + index * 4096;
    return true;
}

int main() {
    nvgsp::RadixLayout layout{};
    assert(nvgsp::radixLayout(63541248, &layout));
    assert(layout.pages[0] == 1);
    assert(layout.pages[1] == 1);
    assert(layout.pages[2] == 31);
    assert(layout.pages[3] == 15513);
    assert(layout.offset[1] == 4096);
    assert(layout.offset[2] == 8192);
    assert(layout.offset[3] == 135168);
    assert(layout.tableBytes == 135168);
    std::vector<uint64_t> table(layout.tableBytes / 8);
    assert(nvgsp::fillRadixTables(layout, table.data(), layout.tableBytes,
                                  tablePage, imagePage, nullptr));
    assert(table[0] == 0x10001000);  // root -> level 1
    assert(table[layout.offset[1] / 8] == 0x10002000);  // level 1 -> level 2
    assert(table[layout.offset[1] / 8 + 30] == 0x10020000);
    assert(table[layout.offset[2] / 8] == 0x20000000);  // leaf -> firmware
    assert(table[layout.offset[2] / 8 + 15512] == 0x20000000 + 15512 * 4096ULL);
    assert(!nvgsp::fillRadixTables(layout, table.data(), layout.tableBytes - 1,
                                   tablePage, imagePage, nullptr));
    assert(!nvgsp::radixLayout(0, &layout));
    assert(!nvgsp::radixLayout(UINT64_MAX, &layout));
    std::printf("WPR ABI: %zu bytes; radix: %llu/%llu/%llu/%llu pages, %llu table bytes\n",
                sizeof(nvgsp::WprMeta),
                (unsigned long long)layout.pages[0],
                (unsigned long long)layout.pages[1],
                (unsigned long long)layout.pages[2],
                (unsigned long long)layout.pages[3],
                (unsigned long long)layout.tableBytes);
}
