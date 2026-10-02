#include "../drivers/NVGspCore/NVGspInitAbi.hpp"
#include <cassert>
#include <cstdio>
int main() {
    using namespace nvgsp;
    static_assert(kGspSharedBytes == 0x81000, "queue allocation");
    assert(initArgId("RMARGS") == 0x524d41524753ULL);
    assert(initArgId("LOGINIT") == 0x4c4f47494e4954ULL);
    MsgqTxHeader h{}; h.size=kGspQueueBytes; h.msgSize=4096;
    h.msgCount=(kGspQueueBytes-4096)/4096; h.flags=1; h.rxHdrOff=32; h.entryOff=4096;
    assert(h.msgCount==63 && sizeof(GspArgumentsCached)==72);
    std::printf("GSP init ABI: %u PTEs, queues 0x%x, command entries %u, args %zu\n",
                kGspQueuePages,kGspSharedBytes,h.msgCount,sizeof(GspArgumentsCached));
}
