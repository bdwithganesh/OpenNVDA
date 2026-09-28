#pragma once

// GA102/AD103 SEC2 Booter Load execution. The caller must reset SEC2 into
// Falcon mode first and retain both staged allocations for the whole call.
#include "NVGspBooter.hpp"

namespace nvgsp {

namespace sec2 {
constexpr uint32_t kBase = 0x00840000;
constexpr uint32_t kMailbox0 = kBase + 0x40;
constexpr uint32_t kMailbox1 = kBase + 0x44;
constexpr uint32_t kCpuCtl = kBase + 0x100;
constexpr uint32_t kBootVec = kBase + 0x104;
constexpr uint32_t kDmaCtl = kBase + 0x10c;
constexpr uint32_t kDmaBase = kBase + 0x110;
constexpr uint32_t kDmaMemOffset = kBase + 0x114;
constexpr uint32_t kDmaCommand = kBase + 0x118;
constexpr uint32_t kDmaFbOffset = kBase + 0x11c;
constexpr uint32_t kDmaBaseHi = kBase + 0x128;
constexpr uint32_t kCpuCtlAlias = kBase + 0x130;
constexpr uint32_t kFbifTranscfg0 = 0x00840600;
constexpr uint32_t kFbifCtl = 0x00840624;
constexpr uint32_t kBromModSel = 0x00841180;
constexpr uint32_t kBromUcodeId = 0x00841198;
constexpr uint32_t kBromEngineMask = 0x0084119c;
constexpr uint32_t kBromParaAddr0 = 0x00841210;
constexpr uint32_t kPriError = 0xbadf5620;
}

struct BooterExecutionResult {
    uint32_t dmaTransfers;
    uint32_t cpuCtl, mailbox0, mailbox1;
};

template <typename Io>
inline bool sec2Poll(Io &io, uint32_t reg, uint32_t mask, uint32_t expected,
                     uint32_t iterations, uint32_t delayUs) {
    for (uint32_t i = 0; i < iterations; ++i) {
        uint32_t value = 0;
        if (!io.read(reg, &value) || value == sec2::kPriError) return false;
        if ((value & mask) == expected) return true;
        io.delay(delayUs);
    }
    return false;
}

template <typename Io>
inline bool sec2DmaTransfer(Io &io, uint32_t destination, uint32_t memoryOffset,
                            uint64_t sourceBus, uint32_t bytes, uint32_t command,
                            uint32_t *count) {
    if (!bytes || (sourceBus & 255) || (bytes & 255) || destination > 0xffffff ||
        !sec2Poll(io, sec2::kDmaCommand, 1, 0, 200000, 10) ||
        !io.write(sec2::kDmaBase, static_cast<uint32_t>(sourceBus >> 8)) ||
        !io.write(sec2::kDmaBaseHi, static_cast<uint32_t>((sourceBus >> 40) & 0x1ff)))
        return false;
    for (uint32_t done = 0; done < bytes; done += 256) {
        if (!sec2Poll(io, sec2::kDmaCommand, 1, 0, 200000, 10) ||
            !io.write(sec2::kDmaMemOffset, destination + done) ||
            !io.write(sec2::kDmaFbOffset, memoryOffset + done) ||
            !io.write(sec2::kDmaCommand, command)) return false;
        if (count) ++*count;
    }
    return sec2Poll(io, sec2::kDmaCommand, 2, 2, 200000, 10);
}

template <typename Io>
inline bool executeBooterLoad(Io &io, const BooterView &view,
                              uint64_t stagedBus, uint32_t stagedBytes,
                              uint64_t metadataBus,
                              BooterExecutionResult *result) {
    if (!result || !stagedBus || (stagedBus & 255) || !metadataBus ||
        view.imageSize > stagedBytes || (view.layout.appCodeOffset & 255) ||
        (view.layout.appCodeSize & 255) || (view.layout.osDataOffset & 255) ||
        (view.layout.osDataSize & 255) ||
        view.patchLocation < view.layout.osDataOffset)
        return false;

    BooterExecutionResult value{};
    uint32_t reg = 0;
    if (!io.read(sec2::kFbifCtl, &reg) || reg == sec2::kPriError ||
        !io.write(sec2::kFbifCtl, reg | (1U << 7)) ||
        !io.write(sec2::kDmaCtl, 0) ||
        !io.read(sec2::kFbifTranscfg0, &reg) || reg == sec2::kPriError ||
        !io.write(sec2::kFbifTranscfg0, (reg & ~7U) | 5U)) return false;

    const uint32_t baseCommand = 6U << 8;
    const uint32_t imemCommand = baseCommand | (1U << 4) | (1U << 2);
    if (!sec2DmaTransfer(io, 0, view.layout.appCodeOffset,
                         stagedBus, view.layout.appCodeSize, imemCommand,
                         &value.dmaTransfers) ||
        !sec2DmaTransfer(io, 0, 0, stagedBus + view.layout.osDataOffset,
                         view.layout.osDataSize, baseCommand,
                         &value.dmaTransfers)) return false;

    if (!io.write(sec2::kBromParaAddr0,
                  view.patchLocation - view.layout.osDataOffset) ||
        !io.write(sec2::kBromEngineMask, view.engineId) ||
        !io.write(sec2::kBromUcodeId, view.ucodeId) ||
        !io.write(sec2::kBromModSel, 1) ||
        !io.write(sec2::kBootVec, view.layout.appCodeOffset) ||
        !io.write(sec2::kMailbox0, static_cast<uint32_t>(metadataBus)) ||
        !io.write(sec2::kMailbox1, static_cast<uint32_t>(metadataBus >> 32)) ||
        !io.read(sec2::kCpuCtl, &reg) || reg == sec2::kPriError)
        return false;
    const uint32_t startReg = (reg & (1U << 6)) ? sec2::kCpuCtlAlias : sec2::kCpuCtl;
    if (!io.write(startReg, 1U << 1) ||
        !sec2Poll(io, sec2::kCpuCtl, 1U << 4, 0, 10000, 10) ||
        !sec2Poll(io, sec2::kCpuCtl, 1U << 4, 1U << 4, 200000, 10) ||
        !io.read(sec2::kCpuCtl, &value.cpuCtl) ||
        !io.read(sec2::kMailbox0, &value.mailbox0) ||
        !io.read(sec2::kMailbox1, &value.mailbox1)) return false;
    *result = value;
    return value.mailbox0 == 0;
}

} // namespace nvgsp
