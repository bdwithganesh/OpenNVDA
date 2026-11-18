#pragma once

// NVIDIA 615.78.05 bare-metal GSP-RM LibOS boot arguments and shared message
// queues (msgq v2).  All addresses are GPU-visible IODMACommand addresses.
#include "NVGspDmaBuffer.hpp"
#include "NVGspAbi.hpp"
#include "NVGspInitAbi.hpp"
#include "NVGspGmcInit.hpp"

namespace nvgsp {

class GspInitStaging {
public:
    // 0.113.0: `registry` = a packed SET_REGISTRY table (NVGspRegistry.hpp);
    // null sends the empty table (8-byte header) as before.
    bool stage(const GspSystemInfoParameters &system, IOMapper *mapper = nullptr,
               const uint8_t *registry = nullptr, uint32_t registryBytes = 0,
               const void *ucodes = nullptr, size_t ucodesBytes = 0) {
        if (ready_) return false;
        if ((ucodes ? (ucodesBytes < 4096 || ucodesBytes > (96ULL << 20) ||
                       externalUcodesBytes_ != 0) :
                      (ucodesBytes != 0 || externalUcodesBytes_ == 0)) ||
            !queues_.allocate(kGspSharedBytes, false, mapper) ||
            !gspArgs_.allocate(4096, true, mapper) ||
            !stateMonitor_.allocate(kGspRmStateMonitorBytes, true, mapper) ||
            !libosArgs_.allocate(4096, true, mapper)) {
            reset(); return false;
        }
        for (unsigned i = 0; i < kGspLogCount; ++i) {
            if (!logs_[i].allocate(kGspLogs[i].bytes, true, mapper)) {
                reset(); return false;
            }
            // NVIDIA's bare-metal LibOS log ABI stores the producer put
            // pointer in word 0 and the contiguous buffer GPA in word 1.
            // The init argument points at that same first page.
            auto *logWords = static_cast<uint64_t *>(logs_[i].bytes());
            logWords[1] = logs_[i].busAddress();
            if (!logs_[i].syncToDevice()) { reset(); return false; }
        }

        uint64_t *ptes = static_cast<uint64_t *>(queues_.bytes());
        for (uint32_t i = 0; i < kGspQueuePages; ++i)
            if (!queues_.busPage(i, &ptes[i])) { reset(); return false; }
        queueRootBus_ = ptes[0];

        // msgq v2 header.  The ring write/read pointers live in the
        // NV_PGSP_QUEUE_HEAD/TAIL(0) registers, not in this header.
        uint8_t *commandBase = static_cast<uint8_t *>(queues_.bytes()) +
            kGspQueuePageTableBytes;
        MsgqTxHeaderV2 command{};
        command.versionMajor = kMsgqVersionMajor;
        command.versionMinor = kMsgqVersionMinor;
        command.size = kGspQueueBytes;
        command.msgSize = kMsgqMsgSize;
        command.msgCount = kMsgqMsgCount;
        command.entryOff = kMsgqEntryOff;
        __builtin_memcpy(commandBase, &command, sizeof(command));

        // 615 replaced the two legacy init RPCs with one GMCAPI/NVKV request.
        // A registry-heavy packet can span multiple queue elements; avoid a
        // 64 KiB kernel stack frame and preserve every zero-padded fragment.
        constexpr size_t initCapacity = 16 * kMsgqMsgSize;
        auto *initRecord = static_cast<uint8_t *>(IOMalloc(initCapacity));
        uint32_t initBytes = 0;
        const bool initBuilt = initRecord && buildGmcInit(
            initRecord, initCapacity, system, registry, registryBytes, &initBytes);
        const uint32_t initElements = (initBytes + kMsgqMsgSize - 1) / kMsgqMsgSize;
        if (initBuilt && initElements && initElements <= 16)
            __builtin_memcpy(commandBase + kMsgqEntryOff, initRecord,
                             initElements * kMsgqMsgSize);
        if (initRecord) IOFree(initRecord, initCapacity);
        if (!initBuilt || !initElements || initElements > 16) { reset(); return false; }
        txWritePtr_ = initialTxWritePtr_ = initElements;
        statusReadPtr_ = 0;

        // 615 splits engine firmware from GSP-RM into ucodes_ga10x.bin.
        // NVIDIA kgspPrepareFirmwareImages creates a separate radix tree
        // and kgspPopulateGspRmInitArgs passes its root and exact byte size.
        if (ucodes) {
            RadixLayout externalLayout{};
            if (!radixLayout(ucodesBytes, &externalLayout) ||
                !externalRadix_.allocate(externalLayout.tableBytes, true, mapper) ||
                !externalUcodes_.allocate(externalLayout.paddedDataBytes, false, mapper) ||
                !externalUcodes_.write(0, ucodes, ucodesBytes) ||
                !fillRadixTables(externalLayout, externalRadix_.bytes(),
                                 externalRadix_.size(), externalTablePage,
                                 externalImagePage, this) ||
                !externalRadix_.syncToDevice()) { reset(); return false; }
            externalUcodesBytes_ = ucodesBytes;
        }
        const GspArgumentsCached args = buildGspArguments(
            queueRootBus_, externalRadix_.busAddress(), externalUcodesBytes_,
            stateMonitor_.busAddress());
        if (!stateMonitor_.syncToDevice() ||
            !gspArgs_.write(0, &args, sizeof(args))) { reset(); return false; }

        auto *regions = static_cast<LibosMemoryRegionInitArgument *>(libosArgs_.bytes());
        for (unsigned i = 0; i < kGspLogCount; ++i) {
            regions[i].id8 = initArgId(kGspLogs[i].id);
            regions[i].pa = logs_[i].busAddress();
            regions[i].size = logs_[i].size();
            regions[i].kind = 1; // LIBOS_MEMORY_REGION_CONTIGUOUS
            regions[i].loc = 1;  // LIBOS_MEMORY_REGION_LOC_SYSMEM
        }
        regions[kGspLogCount].id8 = initArgId("RMARGS");
        regions[kGspLogCount].pa = gspArgs_.busAddress();
        regions[kGspLogCount].size = 4096;
        regions[kGspLogCount].kind = 1;
        regions[kGspLogCount].loc = 1;
        if (!queues_.syncToDevice() || !libosArgs_.syncToDevice()) {
            reset(); return false;
        }
        ready_ = true;
        // Publish the command queue write pointer (GMCAPI init) before
        // the GSP starts.  The callback maps to NV_PGSP_QUEUE_HEAD(0).
        if (doorbell_) doorbell_(doorbellCtx_);
        return true;
    }

    // A hardware-reset resume rebuilds queues/args/logs, retaining the exact
    // external archive and its DMA tree until the service is stopped.
    bool restage(const GspSystemInfoParameters &system) {
        if (!externalUcodesBytes_) return false;
        reset(true);
        return stage(system);
    }
    void reset(bool preserveExternal = false) {
        ready_ = false; queueRootBus_ = 0;
        txWritePtr_ = initialTxWritePtr_ = 0;
        statusReadPtr_ = 0;
        for (auto &log : logs_) log.release();
        if (!preserveExternal) {
            externalRadix_.release(); externalUcodes_.release();
            externalUcodesBytes_ = 0;
        }
        stateMonitor_.release(); libosArgs_.release(); gspArgs_.release(); queues_.release();
    }
    bool ready() const { return ready_; }
    // 615 resume args: GSP_ARGUMENTS_FLAG_IN_PM_TRANSITION lives in
    // GSP_ARGUMENTS_CACHED.flags; sr = {oldLevel 3, flags 5}.
    bool setSrArgs(bool resume) {
        if (!ready_) return false;
        GspArgumentsCached args{};
        __builtin_memcpy(&args, gspArgs_.bytes(), sizeof(args));
        args.flags = (args.flags & ~uint64_t(0x01)) | (resume ? 0x01 : 0);
        args.sr.oldLevel = resume ? 3 : 0;
        args.sr.flags = resume ? 5 : 0;
        return gspArgs_.write(0, &args, sizeof(args));
    }
    uint64_t libosArgsBus() const { return ready_ ? libosArgs_.busAddress() : 0; }
    uint64_t gspArgsBus() const { return ready_ ? gspArgs_.busAddress() : 0; }
    uint64_t queueRootBus() const { return ready_ ? queueRootBus_ : 0; }
    uint64_t externalUcodesRootBus() const {
        return ready_ ? externalRadix_.busAddress() : 0;
    }
    uint64_t stateMonitorBus() const { return ready_ ? stateMonitor_.busAddress() : 0; }
    const void *queueBytes() const { return queues_.bytes(); }
    bool snapshotStatus(MsgqTxHeader *header, uint32_t *readPtr,
                        void *first, size_t firstBytes) const {
        if (!ready_ || !header || !readPtr || !first ||
            firstBytes > kMsgqMsgSize || !queues_.syncFromDevice())
            return false;
        const uint8_t *base = static_cast<const uint8_t *>(queues_.bytes()) +
            kGspQueuePageTableBytes + kGspQueueBytes;
        MsgqTxHeaderV2 v2{};
        __builtin_memcpy(&v2, base, sizeof(v2));
        // Compatibility view used by the existing phase machine: the old
        // driver expected version 0 / rxHdrOff 32 and slot-based pointers.
        header->version = 0;
        header->size = v2.size;
        header->msgSize = kMsgqMsgSize;
        header->msgCount = kMsgqMsgCount;
        header->entryOff = kMsgqEntryOff;
        header->flags = 0;
        header->rxHdrOff = sizeof(MsgqTxHeader);
        if (v2.versionMajor != kMsgqVersionMajor ||
            v2.versionMinor != kMsgqVersionMinor ||
            v2.size != kGspQueueBytes || v2.msgSize != kMsgqMsgSize ||
            v2.msgCount != kMsgqMsgCount || v2.entryOff != kMsgqEntryOff) {
            // The GSP has not published its status queue header yet.
            header->writePtr = 0;
            *readPtr = statusReadPtr_ % kMsgqMsgCount;
            bzero(first, firstBytes);
            return true;
        }
        const uint32_t head = rxHead_ ? rxHead_(rxHeadCtx_) : 0;
        header->writePtr = head % kMsgqMsgCount;
        *readPtr = statusReadPtr_ % kMsgqMsgCount;
        __builtin_memcpy(first, base + kMsgqEntryOff, firstBytes);
        return true;
    }
    const uint8_t *statusEntry(uint32_t index) const {
        if (!ready_ || index >= kMsgqMsgCount) return nullptr;
        return static_cast<const uint8_t *>(queues_.bytes()) +
            kGspQueuePageTableBytes + kGspQueueBytes + kMsgqEntryOff +
            index * kMsgqMsgSize;
    }
    // 0.99.0: restore a post-stage queue snapshot (S3 re-boot on retained
    // staging): identical bytes incl. page table, headers and the
    // pre-queued init request, then synced for the device.
    bool restoreQueues(const void *snapshot) {
        if (!ready_ || !snapshot) return false;
        txWritePtr_ = initialTxWritePtr_;
        statusReadPtr_ = 0;
        __builtin_memcpy(queues_.bytes(), snapshot, kGspSharedBytes);
        if (rxTail_) rxTail_(rxTailCtx_, 0);
        return queues_.syncToDevice();
    }
    bool publishStatusReadPtr(uint32_t readPtr) {
        if (!ready_ || readPtr >= kMsgqMsgCount || !rxTail_) return false;
        const uint32_t current = statusReadPtr_ % kMsgqMsgCount;
        const uint32_t delta = (readPtr + kMsgqMsgCount - current) % kMsgqMsgCount;
        if (!delta) return true;
        statusReadPtr_ += delta;
        rxTail_(rxTailCtx_, statusReadPtr_);
        return true;
    }
    bool enqueueRpc(uint32_t function, const void *payload,
                    uint32_t payloadBytes, uint32_t rpcSequence = 0) {
        // 0.180.12: reject before the uint32 additions can wrap. The old
        // expression could build a tiny record and then memcpy payloadBytes.
        if (payloadBytes >
            UINT32_MAX - sizeof(RpcMessageHeader) - sizeof(GspQueueElementHeader))
            return false;
        const uint32_t rpcBytes = sizeof(RpcMessageHeader) + payloadBytes;
        const uint32_t elementBytes = sizeof(GspQueueElementHeader) + rpcBytes;
        const uint32_t elements = (elementBytes + kMsgqMsgSize - 1) / kMsgqMsgSize;
        if (!ready_ || !elements || elements > 16 ||
            elementBytes > kMsgqMsgSize * 16) return false;
        const uint32_t tail = txTail_ ? txTail_(txTailCtx_) : 0;
        const uint32_t used = txWritePtr_ - tail;
        if (used >= kMsgqMsgCount || elements > kMsgqMsgCount - used - 1)
            return false;
        uint8_t *base = static_cast<uint8_t *>(queues_.bytes()) +
            kGspQueuePageTableBytes;
        const size_t recordBytes = static_cast<size_t>(elements) * kMsgqMsgSize;
        uint8_t *record = static_cast<uint8_t *>(IOMalloc(recordBytes));
        if (!record) return false;
        const bool built = queueRpc(record, recordBytes, function,
                                    payload, payloadBytes, rpcSequence);
        if (built) {
            for (uint32_t i = 0; i < elements; ++i) {
                const uint32_t slot = (txWritePtr_ + i) % kMsgqMsgCount;
                __builtin_memcpy(base + kMsgqEntryOff + slot * kMsgqMsgSize,
                                 record + i * kMsgqMsgSize, kMsgqMsgSize);
            }
            txWritePtr_ += elements;
        }
        IOFree(record, recordBytes);
        if (!built) return false;
        if (!queues_.syncToDevice()) return false;
        if (doorbell_) doorbell_(doorbellCtx_);
        return true;
    }
    void setDoorbell(void (*fn)(void *), void *ctx) { doorbell_ = fn; doorbellCtx_ = ctx; }
    // msgq v2 hardware registers: NV_PGSP_QUEUE_TAIL(0),
    // NV_PGSP_MSGQ_HEAD(0), NV_PGSP_MSGQ_TAIL(0).  The doorbell callback above
    // owns NV_PGSP_QUEUE_HEAD(0).
    void setQueueCallbacks(uint32_t (*txTail)(void *), void *txTailCtx,
                           uint32_t (*rxHead)(void *), void *rxHeadCtx,
                           void (*rxTail)(void *, uint32_t), void *rxTailCtx) {
        txTail_ = txTail; txTailCtx_ = txTailCtx;
        rxHead_ = rxHead; rxHeadCtx_ = rxHeadCtx;
        rxTail_ = rxTail; rxTailCtx_ = rxTailCtx;
    }
    void publishTxHead() { if (doorbell_) doorbell_(doorbellCtx_); }
    uint32_t txSequence() const { return txWritePtr_; }
    // 0.99.2: debug read of the shared queue memory (page table + cmd + status).
    bool snapshotQueue(size_t offset, void *out, size_t bytes) const {
        if (!ready_ || !out || offset > kGspSharedBytes ||
            bytes > kGspSharedBytes - offset ||
            !queues_.syncFromDevice()) return false;
        __builtin_memcpy(out, static_cast<const uint8_t *>(queues_.bytes()) + offset, bytes);
        return true;
    }
    bool snapshotLog(unsigned index, void *out, size_t bytes) const {
        if (!ready_ || index >= kGspLogCount || !out || bytes > logs_[index].size() ||
            !logs_[index].syncFromDevice()) return false;
        __builtin_memcpy(out, logs_[index].bytes(), bytes);
        return true;
    }

private:
    static bool externalTablePage(void *context, uint64_t index, uint64_t *address) {
        return static_cast<GspInitStaging *>(context)->externalRadix_.busPage(index, address);
    }
    static bool externalImagePage(void *context, uint64_t index, uint64_t *address) {
        return static_cast<GspInitStaging *>(context)->externalUcodes_.busPage(index, address);
    }
    template <typename T>
    static void put(uint8_t *bytes, size_t offset, const T &value) {
        __builtin_memcpy(bytes + offset, &value, sizeof(value));
    }
    static bool queueRpc(uint8_t *slot, size_t capacity, uint32_t function,
                         const void *payload, uint32_t payloadBytes,
                         uint32_t rpcSequence = 0) {
        if (payloadBytes >
            UINT32_MAX - sizeof(RpcMessageHeader) - sizeof(GspQueueElementHeader))
            return false;
        const uint32_t rpcBytes = sizeof(RpcMessageHeader) + payloadBytes;
        const uint32_t elementBytes = sizeof(GspQueueElementHeader) + rpcBytes;
        if (!slot || (!payload && payloadBytes) || elementBytes > capacity)
            return false;
        bzero(slot, capacity);
        auto *element = reinterpret_cast<GspQueueElementHeader *>(slot);
        element->mctpMagic = kMctpMagic;
        element->mctpPayloadSize = elementBytes;
        element->mctpHeader = kMctpHeader;
        element->nvdmHeader = kNvdmHeader;
        element->nvdmPayloadSize = rpcBytes;
        element->reserved = 0;
        auto *rpc = reinterpret_cast<RpcMessageHeader *>(
            slot + sizeof(GspQueueElementHeader));
        rpc->headerVersion = 0x03000000;
        rpc->signature = 0x43505256;
        rpc->length = rpcBytes;
        rpc->function = function;
        rpc->result = 0xffffffff;
        rpc->privateResult = 0xffffffff;
        rpc->sequence = rpcSequence;
        rpc->spare = 0;
        if (payloadBytes)
            __builtin_memcpy(reinterpret_cast<uint8_t *>(rpc) + sizeof(*rpc),
                             payload, payloadBytes);
        return true;
    }
    DmaBuffer queues_, gspArgs_, libosArgs_, stateMonitor_, logs_[kGspLogCount];
    DmaBuffer externalRadix_, externalUcodes_;
    uint64_t externalUcodesBytes_ = 0;
    uint64_t queueRootBus_ = 0;
    uint32_t txWritePtr_ = 0, initialTxWritePtr_ = 0;
    uint32_t statusReadPtr_ = 0;
    void (*doorbell_)(void *) = nullptr;
    void *doorbellCtx_ = nullptr;
    uint32_t (*txTail_)(void *) = nullptr;
    void *txTailCtx_ = nullptr;
    uint32_t (*rxHead_)(void *) = nullptr;
    void *rxHeadCtx_ = nullptr;
    void (*rxTail_)(void *, uint32_t) = nullptr;
    void *rxTailCtx_ = nullptr;
    bool ready_ = false;
};

} // namespace nvgsp
