#pragma once
// SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: MIT
// The full permission notice is reproduced in NVGspGmcInit.hpp below.
// Port of pinned615 rmgspseq.h / kgspLoadAndExecuteHsBinary_GA102 and
// kgspExecuteCoreResume_TU102. Only signed firmware's GSP HS request is accepted.
#include "NVGspGmcInit.hpp"
#include "NVGspFalcon.hpp"

namespace nvgsp {
constexpr uint32_t kGmcExecHs = 0x10003;
template<class Io>
bool selectGspCore(Io &io, bool riscv) {
    // BCR VALID bit0 is a read-only hardware acknowledgement, not CORE_SELECT.
    // CORE_SELECT is bit4; BRFETCH is bit8 (ga102/dev_riscv_pri.h).
    return io.write(0x111668, riscv ? 0x111 : 0) &&
        falconPoll(io, 0x111668, 0x11, riscv ? 0x11 : 1, 200000, 10);
}
struct GmcHsParams {
    uint64_t imemPhysAddr, dmemPhysAddr, reserved64[2];
    uint32_t imemVa, imemPa, imemSize, dmemVa, dmemPa, dmemSize;
    uint32_t signatureDmem, engineMask, ucodeId, reserved32[3];
};
static_assert(sizeof(GmcHsParams) == 80, "GspLoadExecHsBinaryParams ABI");

inline bool parseGmcHs(const void *packet, size_t bytes, GmcHsParams *out) {
    constexpr size_t prefix = sizeof(GspQueueElementHeader) + sizeof(GmcHeader);
    if (!packet || !out || bytes != prefix + sizeof(*out)) return false;
    GspQueueElementHeader q{}; GmcHeader g{}; GmcHsParams p{};
    __builtin_memcpy(&q, packet, sizeof(q));
    __builtin_memcpy(&g, static_cast<const uint8_t *>(packet) + sizeof(q), sizeof(g));
    __builtin_memcpy(&p, static_cast<const uint8_t *>(packet) + prefix, sizeof(p));
    if (q.mctpMagic != kMctpMagic || q.mctpHeader != kMctpHeader ||
        q.nvdmHeader != kGmcNvdmHeader || q.reserved || q.mctpPayloadSize != bytes ||
        q.nvdmPayloadSize != bytes - sizeof(q) || g.command != kGmcExecHs ||
        g.size != sizeof(p) || g.statusOrMaxResponse) return false;
    for (auto v : g.reserved) if (v) return false;
    for (auto v : p.reserved64) if (v) return false;
    for (auto v : p.reserved32) if (v) return false;
    *out = p; return true;
}

inline bool validGmcHs(const GmcHsParams &p, uint64_t fbBytes) {
    // Falcon DMA offsets have 24 bits. Bound each transfer to the TCM size
    // budget, require full256-byte blocks, and keep all reads inside real FB.
    const uint32_t sizes[2] = {p.imemSize, p.dmemSize};
    const uint32_t pa[2] = {p.imemPa, p.dmemPa};
    const uint32_t va[2] = {p.imemVa, p.dmemVa == UINT32_MAX ? 0 : p.dmemVa};
    const uint64_t src[2] = {p.imemPhysAddr, p.dmemPhysAddr};
    if (!fbBytes || p.engineMask != 0x400 || !p.ucodeId ||
        (p.signatureDmem & 3) || p.signatureDmem >= p.dmemSize) return false;
    for (unsigned i = 0; i < 2; ++i) {
        if (!sizes[i] || sizes[i] > 0x40000 ||
            ((sizes[i] | pa[i] | va[i] | src[i]) & 255) ||
            pa[i] > 0x1000000U - sizes[i] || va[i] > 0x1000000U - sizes[i] ||
            src[i] >= fbBytes || va[i] > fbBytes - src[i] ||
            sizes[i] > fbBytes - src[i] - va[i]) return false;
    }
    return true;
}

struct GmcHsResult {
    uint32_t stage = 0, dmaTransfers = 0, cpuCtl = 0;
    uint32_t mailbox0 = 0, sec2Mailbox0 = 0, handoff = 0, riscvCpuCtl = 0;
};

template<class Io, class Reset>
bool executeGmcHs(Io &io, const GmcHsParams &p, uint64_t fbBytes,
                  uint64_t argsBus, uint32_t appVersion, Reset reset,
                  GmcHsResult *out) {
    if (!out) return false;
    *out = GmcHsResult{};
    out->stage = 1;
    if (!validGmcHs(p, fbBytes) || !argsBus || (argsBus & 4095) ||
        !falconPoll(io, falcon::kMailbox0, 0x80000000, 0x80000000, 200000, 10))
        return false;
    out->stage = 2;
    uint32_t reg = 0, chipId = 0;
    if (!reset(false) || !io.read(0, &chipId) ||
        !io.write(falcon::kBase + 0x84, chipId)) return false;
    out->stage = 21;
    if (!io.read(falcon::kFbifCtl, &reg) ||
        !io.write(falcon::kFbifCtl, reg | 0x80) || !io.write(falcon::kDmaCtl, 0) ||
        !io.read(falcon::kFbifTranscfg0, &reg) ||
        !io.write(falcon::kFbifTranscfg0, (reg & ~0x10007U) | 4)) return false;
    out->stage = 3;
    if (!falconDmaTransfer(io, p.imemPa, p.imemVa, p.imemPhysAddr,
                           p.imemSize, 0x614, &out->dmaTransfers)) return false;
    out->stage = 4;
    const uint32_t dmemOff = p.dmemVa == UINT32_MAX ? 0 : p.dmemVa;
    const uint32_t dmemCmd = 0x600 | (p.dmemVa == UINT32_MAX ? 0 : 0x10000);
    if (!falconDmaTransfer(io, p.dmemPa, dmemOff, p.dmemPhysAddr,
                           p.dmemSize, dmemCmd, &out->dmaTransfers) ||
        !io.write(falcon::kBromParaAddr0, p.signatureDmem) ||
        !io.write(falcon::kBromEngineMask, p.engineMask) ||
        !io.write(falcon::kBromUcodeId, p.ucodeId) ||
        !io.write(falcon::kBromModSel, 1) ||
        !io.write(falcon::kMailbox0, 0xfe) ||
        !io.write(falcon::kBootVec, p.imemVa) ||
        !io.read(falcon::kCpuCtl, &reg)) return false;
    out->stage = 5;
    if (!io.write((reg & 0x40) ? falcon::kCpuCtlAlias : falcon::kCpuCtl, 2) ||
        !falconPoll(io, falcon::kCpuCtl, 0x10, 0, 10000, 10) ||
        !falconPoll(io, falcon::kCpuCtl, 0x10, 0x10, 200000, 10) ||
        !io.read(falcon::kCpuCtl, &out->cpuCtl) ||
        !io.read(falcon::kMailbox0, &out->mailbox0) || out->mailbox0 != 0)
        return false;
    out->stage = 6;
    // SEC2 retains the authenticated boot/resume program. Do not replay
    // Booter Load from sysmem or send a response to this fire-and-forget API.
    if (!reset(true) ||
        !io.write(falcon::kMailbox0, static_cast<uint32_t>(argsBus)) ||
        !io.write(falcon::kMailbox1, static_cast<uint32_t>(argsBus >> 32)) ||
        !io.read(0x840100, &reg) ||
        !io.write((reg & 0x40) ? 0x840130 : 0x840100, 2) ||
        !falconPoll(io, 0x1180f8, 1U << 26, 1U << 26, 200000, 10) ||
        !io.read(0x840040, &out->sec2Mailbox0) || out->sec2Mailbox0 != 0 ||
        !io.write(falcon::kBase + 0x80, appVersion) ||
        !io.read(0x1180f8, &out->handoff) ||
        !io.read(0x111388, &out->riscvCpuCtl) || !(out->riscvCpuCtl & 0x80))
        return false;
    out->stage = 100;
    return true;
}
} // namespace nvgsp
