#pragma once

// Minimal NVIDIA 570.144 bare-metal GSP-RM LibOS boot arguments and shared
// message queues. All addresses are GPU-visible IODMACommand addresses.
#include "NVGspDmaBuffer.hpp"
#include "NVGspInitAbi.hpp"

namespace nvgsp {

class GspInitStaging {
public:
    // 0.113.0: `registry` = a packed SET_REGISTRY table (NVGspRegistry.hpp);
    // null sends the empty table (8-byte header) as before.
    bool stage(const GspSystemInfoParameters &system, IOMapper *mapper = nullptr,
               const uint8_t *registry = nullptr, uint32_t registryBytes = 0) {
        if (ready_ || !queues_.allocate(kGspSharedBytes, false, mapper) ||
            !gspArgs_.allocate(4096, true, mapper) ||
            !libosArgs_.allocate(4096, true, mapper)) {
            reset(); return false;
        }
        static const char *const ids[5] = {
            "LOGINIT", "LOGINTR", "LOGRM", "LOGMNOC", "LOGKRNL"
        };
        for (unsigned i = 0; i < 5; ++i) {
            if (!logs_[i].allocate(0x10000, true, mapper)) {
                reset(); return false;
            }
            // NVIDIA's bare-metal LibOS log ABI stores the producer put
            // pointer in word 0 and the 16 GPU page addresses in words 1..16.
            // The init argument points at the first page (word 1's value).
            auto *logWords = static_cast<uint64_t *>(logs_[i].bytes());
            for (unsigned page = 0; page < 16; ++page) {
                uint64_t address = 0;
                if (!logs_[i].busPage(page, &address)) {
                    reset(); return false;
                }
                logWords[1 + page] = address;
            }
            if (!logs_[i].syncToDevice()) { reset(); return false; }
        }

        uint64_t *ptes = static_cast<uint64_t *>(queues_.bytes());
        for (uint32_t i = 0; i < kGspQueuePages; ++i)
            if (!queues_.busPage(i, &ptes[i])) { reset(); return false; }
        queueRootBus_ = ptes[0];

        MsgqTxHeader command{};
        command.size = kGspQueueBytes;
        command.msgSize = 4096;
        command.msgCount = (kGspQueueBytes - 4096) / 4096;
        command.flags = 1;       // MSGQ_FLAGS_SWAP_RX
        command.rxHdrOff = 32;   // 16-byte header alignment
        command.entryOff = 4096; // 4 KiB element alignment
        uint8_t *commandBase = static_cast<uint8_t *>(queues_.bytes()) +
            kGspQueuePageTableBytes;
        __builtin_memcpy(commandBase, &command, sizeof(command));

        // CPU-RM queues these two asynchronous RPCs before starting GSP-RM.
        // Early GSP initialization waits for both before it can emit INIT_DONE.
        uint8_t systemInfo[kGspSystemInfoBytes]{};
        put(systemInfo, 0, system.bar0);
        put(systemInfo, 8, system.bar1);
        put(systemInfo, 16, system.bar2);
        put(systemInfo, 32, system.domainBusDevice);
        put(systemInfo, 64, system.consoleMemSize);
        put(systemInfo, 72, system.maxUserVa);
        put(systemInfo, 88, system.pciDeviceId);
        put(systemInfo, 92, system.pciSubDeviceId);
        put(systemInfo, 96, system.pciRevisionId);
        systemInfo[124] = system.gpuBehindBridge;
        systemInfo[125] = system.flrSupported;
        systemInfo[126] = system.bar0Is64Bit;
        systemInfo[127] = system.mnocAvailable;
        systemInfo[896] = system.isPrimary;
        put(systemInfo, 900, system.pcieLinkCap);
        systemInfo[913] = system.routeDisplayInterruptsToCpu;
        const uint64_t hostPageSize = 4096;
        put(systemInfo, 920, hostPageSize);
        if (!queueRpc(commandBase + 4096, 4096, 0, 72, systemInfo,
                      sizeof(systemInfo))) { reset(); return false; }

        PackedRegistryTable emptyRegistry{sizeof(PackedRegistryTable), 0};
        const bool useRegistry = registry && registryBytes >= sizeof(PackedRegistryTable);
        if (!queueRpc(commandBase + 8192, 4096, 1, 73,
                      useRegistry ? static_cast<const void *>(registry) : &emptyRegistry,
                      useRegistry ? registryBytes : sizeof(emptyRegistry))) {
            reset(); return false;
        }
        command.writePtr = 2;
        __builtin_memcpy(commandBase, &command, sizeof(command));

        GspArgumentsCached args{};
        args.messageQueue.sharedMemPhysAddr = queueRootBus_;
        args.messageQueue.pageTableEntryCount = kGspQueuePages;
        args.messageQueue.cmdQueueOffset = kGspQueuePageTableBytes;
        args.messageQueue.statQueueOffset = kGspQueuePageTableBytes + kGspQueueBytes;
        args.dmemStack = 1;
        if (!gspArgs_.write(0, &args, sizeof(args))) { reset(); return false; }

        auto *regions = static_cast<LibosMemoryRegionInitArgument *>(libosArgs_.bytes());
        for (unsigned i = 0; i < 5; ++i) {
            regions[i].id8 = initArgId(ids[i]);
            regions[i].pa = logs_[i].busAddress();
            regions[i].size = 0x10000;
            regions[i].kind = 1; // LIBOS_MEMORY_REGION_CONTIGUOUS
            regions[i].loc = 1;  // LIBOS_MEMORY_REGION_LOC_SYSMEM
        }
        regions[5].id8 = initArgId("RMARGS");
        regions[5].pa = gspArgs_.busAddress();
        regions[5].size = 4096;
        regions[5].kind = 1;
        regions[5].loc = 1;
        if (!queues_.syncToDevice() || !libosArgs_.syncToDevice()) {
            reset(); return false;
        }
        ready_ = true;
        return true;
    }

    void reset() {
        ready_ = false; queueRootBus_ = 0;
        // 0.99.3: a fresh GSP expects command element seqNum 2 after the two
        // pre-queued RPCs; a stale counter (110 after one session) made the
        // S3 re-booted GSP-RM answer UNLOADING_GUEST_DRIVER (47) and halt.
        txSequence_ = 2;
        for (auto &log : logs_) log.release();
        libosArgs_.release(); gspArgs_.release(); queues_.release();
    }
    bool ready() const { return ready_; }
    // 0.100.0: GSP_ARGUMENTS_CACHED.srInitArguments (offset 32): resume =
    // {oldLevel 3, flags PRESERVING|PM_TRANSITION (5), bInPMTransition 1}
    // (nouveau r570_gsp_set_rmargs); cold = all zero.
    bool setSrArgs(bool resume) {
        if (!ready_) return false;
        GspArgumentsCached args{};
        __builtin_memcpy(&args, gspArgs_.bytes(), sizeof(args));
        args.sr.oldLevel = resume ? 3 : 0;
        args.sr.flags = resume ? 5 : 0;
        args.sr.inPmTransition = resume ? 1 : 0;
        return gspArgs_.write(0, &args, sizeof(args));
    }
    uint64_t libosArgsBus() const { return ready_ ? libosArgs_.busAddress() : 0; }
    uint64_t gspArgsBus() const { return ready_ ? gspArgs_.busAddress() : 0; }
    uint64_t queueRootBus() const { return ready_ ? queueRootBus_ : 0; }
    const void *queueBytes() const { return queues_.bytes(); }
    bool snapshotStatus(MsgqTxHeader *header, uint32_t *readPtr,
                        void *first, size_t firstBytes) const {
        if (!ready_ || !header || !readPtr || !first || firstBytes > 4096 ||
            !queues_.syncFromDevice()) return false;
        const uint8_t *command = static_cast<const uint8_t *>(queues_.bytes()) +
            kGspQueuePageTableBytes;
        const uint8_t *base = static_cast<const uint8_t *>(queues_.bytes()) +
            kGspQueuePageTableBytes + kGspQueueBytes;
        __builtin_memcpy(header, base, sizeof(*header));
        __builtin_memcpy(readPtr, command + 32, sizeof(*readPtr));
        __builtin_memcpy(first, base + 4096, firstBytes);
        return true;
    }
    const uint8_t *statusEntry(uint32_t index) const {
        if (!ready_ || index >= 63) return nullptr;
        return static_cast<const uint8_t *>(queues_.bytes()) +
            kGspQueuePageTableBytes + kGspQueueBytes + 4096 + index * 4096;
    }
    // 0.99.0: restore a post-stage queue snapshot (S3 re-boot on retained
    // staging): identical bytes incl. page table, headers and the 2
    // pre-queued init RPCs, then synced for the device.
    bool restoreQueues(const void *snapshot) {
        if (!ready_ || !snapshot) return false;
        txSequence_ = 2;   // snapshot = post-stage state (2 pre-queued RPCs)
        __builtin_memcpy(queues_.bytes(), snapshot, kGspSharedBytes);
        return queues_.syncToDevice();
    }
    bool publishStatusReadPtr(uint32_t readPtr) {
        if (!ready_ || readPtr >= 63) return false;
        uint8_t *command = static_cast<uint8_t *>(queues_.bytes()) +
            kGspQueuePageTableBytes;
        __builtin_memcpy(command + 32, &readPtr, sizeof(readPtr));
        return queues_.syncToDevice();
    }
    bool enqueueRpc(uint32_t function, const void *payload,
                    uint32_t payloadBytes, uint32_t rpcSequence = 0) {
        const uint32_t messageBytes = sizeof(GspQueueElementHeader) +
            sizeof(RpcMessageHeader) + payloadBytes;
        const uint32_t elements = (messageBytes + 4095) / 4096;
        if (!ready_ || !elements || elements > 16 ||
            !queues_.syncFromDevice()) return false;
        uint8_t *base = static_cast<uint8_t *>(queues_.bytes());
        uint8_t *command = base + kGspQueuePageTableBytes;
        uint8_t *status = command + kGspQueueBytes;
        MsgqTxHeader header{};
        uint32_t readPtr = 0;
        __builtin_memcpy(&header, command, sizeof(header));
        __builtin_memcpy(&readPtr, status + 32, sizeof(readPtr));
        if (header.msgCount != 63 || header.msgSize != 4096 ||
            header.entryOff != 4096 || header.writePtr >= header.msgCount ||
            readPtr >= header.msgCount) return false;
        uint32_t used = header.writePtr + header.msgCount - readPtr;
        if (used >= header.msgCount) used -= header.msgCount;
        if (elements > header.msgCount - used - 1) return false;
        const size_t recordBytes = static_cast<size_t>(elements) * 4096;
        uint8_t *record = static_cast<uint8_t *>(IOMalloc(recordBytes));
        if (!record) return false;
        const bool built = queueRpc(record, recordBytes, txSequence_, function,
                                    payload, payloadBytes, rpcSequence);
        if (built) {
            for (uint32_t i = 0; i < elements; ++i) {
                const uint32_t slot = (header.writePtr + i) % header.msgCount;
                __builtin_memcpy(command + header.entryOff + slot * 4096,
                                 record + i * 4096, 4096);
            }
            header.writePtr = (header.writePtr + elements) % header.msgCount;
        }
        IOFree(record, recordBytes);
        if (!built) return false;
        __builtin_memcpy(command, &header, sizeof(header));
        if (!queues_.syncToDevice()) return false;
        ++txSequence_;
        // 0.100.5: notify GSP (NV_PGSP_QUEUE_HEAD(0) doorbell, nouveau
        // r535_gsp_cmdq_push). Without it GSP-RM only noticed commands on its
        // own ~0.7 s timer: every chain RPC cost ~1 s.
        if (doorbell_) doorbell_(doorbellCtx_);
        return true;
    }
    void setDoorbell(void (*fn)(void *), void *ctx) { doorbell_ = fn; doorbellCtx_ = ctx; }
    uint32_t txSequence() const { return txSequence_; }
    // 0.99.2: debug read of the shared queue memory (page table + cmd + status).
    bool snapshotQueue(size_t offset, void *out, size_t bytes) const {
        if (!ready_ || !out || offset + bytes > kGspSharedBytes ||
            !queues_.syncFromDevice()) return false;
        __builtin_memcpy(out, static_cast<const uint8_t *>(queues_.bytes()) + offset, bytes);
        return true;
    }
    bool snapshotLog(unsigned index, void *out, size_t bytes) const {
        if (!ready_ || index >= 5 || !out || bytes > logs_[index].size() ||
            !logs_[index].syncFromDevice()) return false;
        __builtin_memcpy(out, logs_[index].bytes(), bytes);
        return true;
    }

private:
    template <typename T>
    static void put(uint8_t *bytes, size_t offset, const T &value) {
        __builtin_memcpy(bytes + offset, &value, sizeof(value));
    }
    static uint32_t checksum32(const void *bytes, size_t length) {
        const uint8_t *p = static_cast<const uint8_t *>(bytes);
        uint64_t value = 0;
        for (size_t i = 0; i < (length + 7) / 8; ++i) {
            uint64_t word = 0;
            const size_t offset = i * 8;
            const size_t take = length - offset < 8 ? length - offset : 8;
            __builtin_memcpy(&word, p + offset, take);
            value ^= word;
        }
        return static_cast<uint32_t>(value) ^ static_cast<uint32_t>(value >> 32);
    }
    static bool queueRpc(uint8_t *slot, size_t capacity, uint32_t queueSequence,
                         uint32_t function, const void *payload,
                         uint32_t payloadBytes, uint32_t rpcSequence = 0) {
        if (!slot || (!payload && payloadBytes) ||
            sizeof(GspQueueElementHeader) + sizeof(RpcMessageHeader) +
                payloadBytes > capacity) return false;
        bzero(slot, capacity);
        auto *element = reinterpret_cast<GspQueueElementHeader *>(slot);
        auto *rpc = reinterpret_cast<RpcMessageHeader *>(
            slot + sizeof(GspQueueElementHeader));
        element->sequence = queueSequence;
        const size_t messageBytes = sizeof(GspQueueElementHeader) +
            sizeof(RpcMessageHeader) + payloadBytes;
        element->elementCount = static_cast<uint32_t>((messageBytes + 4095) / 4096);
        rpc->headerVersion = 0x03000000;
        rpc->signature = 0x43505256;
        rpc->length = sizeof(RpcMessageHeader) + payloadBytes;
        rpc->function = function;
        rpc->result = 0xffffffff;
        rpc->privateResult = 0xffffffff;
        rpc->sequence = rpcSequence;   // echoed by GSP-RM: matches a reply to its request
        if (payloadBytes)
            __builtin_memcpy(reinterpret_cast<uint8_t *>(rpc) + sizeof(*rpc),
                             payload, payloadBytes);
        element->checksum = checksum32(slot, messageBytes);
        return checksum32(slot, messageBytes) == 0;
    }
    DmaBuffer queues_, gspArgs_, libosArgs_, logs_[5];
    uint64_t queueRootBus_ = 0;
    uint32_t txSequence_ = 2;
    void (*doorbell_)(void *) = nullptr;
    void *doorbellCtx_ = nullptr;
    bool ready_ = false;
};

} // namespace nvgsp
