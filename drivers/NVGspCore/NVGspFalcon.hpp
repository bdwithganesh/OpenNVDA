#pragma once

#include "NVGspFwsec.hpp"

namespace nvgsp {

namespace falcon {
constexpr uint32_t kBase = 0x00110000;
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
constexpr uint32_t kFbifTranscfg0 = 0x00110600;
constexpr uint32_t kFbifCtl = 0x00110624;
constexpr uint32_t kBromModSel = 0x00111180;
constexpr uint32_t kBromUcodeId = 0x00111198;
constexpr uint32_t kBromEngineMask = 0x0011119c;
constexpr uint32_t kBromParaAddr0 = 0x00111210;
constexpr uint32_t kFrtsScratch = 0x00001438;
constexpr uint32_t kWpr2Lo = 0x001fa824;
constexpr uint32_t kWpr2Hi = 0x001fa828;
constexpr uint32_t kPriError = 0xbadf5620;
}

struct FwsecExecutionResult {
    uint32_t dmaTransfers;
    uint32_t cpuCtl, mailbox0, mailbox1, frtsScratch, wpr2Lo, wpr2Hi;
};

template <typename Io>
inline bool falconPoll(Io &io, uint32_t reg, uint32_t mask, uint32_t expected,
                       uint32_t iterations, uint32_t delayUs) {
    for (uint32_t i = 0; i < iterations; ++i) {
        uint32_t value = 0;
        if (!io.read(reg, &value) || value == falcon::kPriError) return false;
        if ((value & mask) == expected) return true;
        io.delay(delayUs);
    }
    return false;
}

template <typename Io>
inline bool falconDmaTransfer(Io &io, uint32_t destination, uint32_t memoryOffset,
                              uint64_t sourceBus, uint32_t bytes, uint32_t command,
                              uint32_t *transferCount) {
    if (!bytes || (sourceBus & 255) || destination > 0xffffff ||
        !falconPoll(io, falcon::kDmaCommand, 1, 0, 200000, 10) ||
        !io.write(falcon::kDmaBase, static_cast<uint32_t>(sourceBus >> 8)) ||
        !io.write(falcon::kDmaBaseHi, static_cast<uint32_t>((sourceBus >> 40) & 0x1ff)))
        return false;
    uint32_t transferred = 0;
    while (transferred < bytes) {
        if (destination > 0xffffff ||
            !falconPoll(io, falcon::kDmaCommand, 1, 0, 200000, 10) ||
            !io.write(falcon::kDmaMemOffset, destination) ||
            !io.write(falcon::kDmaFbOffset, memoryOffset) ||
            !io.write(falcon::kDmaCommand, command))
            return false;
        transferred += 256;
        destination += 256;
        memoryOffset += 256;
        if (transferCount) ++*transferCount;
    }
    return falconPoll(io, falcon::kDmaCommand, 2, 2, 200000, 10);
}

// Execute an already reset and Falcon-selected GSP core. The staged buffer
// must contain the VBIOS image rounded up to 256 bytes, because Falcon DMA
// always moves full 256-byte blocks (including the final DMEM tail).
template <typename Io>
inline bool executeFwsecFrts(Io &io, const FwsecView &view,
                             uint64_t stagedBus, uint32_t stagedBytes,
                             uint64_t frtsOffset, FwsecExecutionResult *result) {
    if (!result || (stagedBus & 255) || (frtsOffset & 4095) ||
        !view.desc.imemLoadSize || !view.desc.dmemLoadSize ||
        (view.desc.imemLoadSize & 255)) return false;
    const uint64_t logicalBytes = uint64_t(view.desc.imemLoadSize) + view.desc.dmemLoadSize;
    const uint64_t dmaBytes = (logicalBytes + 255) & ~uint64_t(255);
    if (logicalBytes > view.desc.storedSize || dmaBytes > stagedBytes) return false;

    FwsecExecutionResult value{};
    uint32_t reg = 0;
    if (!io.read(falcon::kFbifCtl, &reg) || reg == falcon::kPriError ||
        !io.write(falcon::kFbifCtl, reg | (1U << 7)) ||
        !io.write(falcon::kDmaCtl, 0) ||
        !io.read(falcon::kFbifTranscfg0, &reg) || reg == falcon::kPriError ||
        !io.write(falcon::kFbifTranscfg0, (reg & ~7U) | 5U))
        return false;

    // READ, 256B, CTXDMA 0. IMEM is secure level 1; DMEM is non-secure.
    const uint32_t baseCommand = 6U << 8;
    const uint32_t imemCommand = baseCommand | (1U << 4) | (1U << 2);
    if (stagedBus < view.desc.imemVirtBase ||
        !falconDmaTransfer(io, view.desc.imemPhysBase, view.desc.imemVirtBase,
                           stagedBus - view.desc.imemVirtBase,
                           view.desc.imemLoadSize, imemCommand, &value.dmaTransfers) ||
        !falconDmaTransfer(io, view.desc.dmemPhysBase, 0,
                           stagedBus + view.desc.imemLoadSize,
                           view.desc.dmemLoadSize, baseCommand, &value.dmaTransfers))
        return false;

    if (!io.write(falcon::kBromParaAddr0, view.desc.pkcDataOffset) ||
        !io.write(falcon::kBromEngineMask, view.desc.engineIdMask) ||
        !io.write(falcon::kBromUcodeId, view.desc.ucodeId) ||
        !io.write(falcon::kBromModSel, 1) ||
        !io.write(falcon::kBootVec, view.desc.imemVirtBase) ||
        !io.read(falcon::kCpuCtl, &reg) || reg == falcon::kPriError)
        return false;
    const uint32_t startReg = (reg & (1U << 6)) ? falcon::kCpuCtlAlias : falcon::kCpuCtl;
    if (!io.write(startReg, 1U << 1)) return false;

    // Require evidence that STARTCPU cleared HALTED before accepting the later halt.
    if (!falconPoll(io, falcon::kCpuCtl, 1U << 4, 0, 10000, 10) ||
        !falconPoll(io, falcon::kCpuCtl, 1U << 4, 1U << 4, 200000, 10))
        return false;
    if (!io.read(falcon::kCpuCtl, &value.cpuCtl) ||
        !io.read(falcon::kMailbox0, &value.mailbox0) ||
        !io.read(falcon::kMailbox1, &value.mailbox1) ||
        !io.read(falcon::kFrtsScratch, &value.frtsScratch) ||
        !io.read(falcon::kWpr2Lo, &value.wpr2Lo) ||
        !io.read(falcon::kWpr2Hi, &value.wpr2Hi))
        return false;
    *result = value;
    return (value.frtsScratch >> 16) == 0 && (value.wpr2Hi >> 4) != 0 &&
           (value.wpr2Lo >> 4) == static_cast<uint32_t>(frtsOffset >> 12);
}

} // namespace nvgsp
