#include "../drivers/NVGspCore/NVGspInitAbi.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
int main() {
    using namespace nvgsp;
    static_assert(kGspSharedBytes == 0x81000, "queue allocation");
    assert(initArgId("RMARGS") == 0x524d41524753ULL);
    assert(initArgId("LOGINIT") == 0x4c4f47494e4954ULL);
    MsgqTxHeader h{}; h.size=kGspQueueBytes; h.msgSize=4096;
    h.msgCount=(kGspQueueBytes-4096)/4096; h.flags=1; h.rxHdrOff=32; h.entryOff=4096;
    assert(h.msgCount==63 && sizeof(GspArgumentsCached)==184);
    const auto args = buildGspArguments(0x731234000ULL);
    assert(std::memcmp(&args.magic, "GSP ", 4) == 0);
    assert(args.size == 184 && args.flags == 2);
    assert(args.messageQueue.sharedMemPhysAddr == 0x731234000ULL);
    assert(args.messageQueue.cmdQueueOffset == 0x1000);
    assert(args.messageQueue.statQueueOffset == 0x41000);
    assert(args.messageQueue.pageTableEntryCount == 129);
    assert(args.messageQueue.queueElementHdrSize == 24);
    assert(args.messageQueue.queueElementSizeMin == 4096);
    assert(args.messageQueue.queueElementSizeMax == 65536);
    assert(args.bindataArgs.radix3 == 0 && args.bindataArgs.size == 0);
    const auto external = buildGspArguments(0x731234000ULL, 0x731456000ULL, 58215936);
    assert(std::memcmp(&external.magic, "GSP ", 4) == 0);
    assert(external.bindataArgs.radix3 == 0x731456000ULL);
    assert(external.bindataArgs.size == 58215936);
    assert(external.messageQueue.sharedMemPhysAddr == args.messageQueue.sharedMemPhysAddr);
    assert(kGspLogCount == 8);
    assert(initArgId(kGspLogs[0].id) == 0x4c4f47494e4954ULL);
    assert(initArgId(kGspLogs[5].id) == 0x4c4f47444247ULL);
    assert(initArgId(kGspLogs[6].id) == 0x4c4f47524f4f54ULL);
    assert(initArgId(kGspLogs[7].id) == 0x4c4f47524d4f4eULL);
    assert(kGspLogs[5].bytes == 65536 && kGspLogs[6].bytes == 4096 && kGspLogs[7].bytes == 4096);
    const auto monitored = buildGspArguments(0x731234000ULL, 0x731456000ULL,
                                           58215936, 0x731789000ULL);
    assert(monitored.rmStateMonitorBufferArgs.pa == 0x731789000ULL);
    assert(monitored.rmStateMonitorBufferArgs.size == 4096);
    assert(monitored.bindataArgs.radix3 == external.bindataArgs.radix3);
    std::printf("GSP init ABI: %u PTEs, queues 0x%x, command entries %u, args %zu\n",
                kGspQueuePages,kGspSharedBytes,h.msgCount,sizeof(GspArgumentsCached));
}
