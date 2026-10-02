#include "../drivers/NVGspCore/NVGspChannel.hpp"
#include <cassert>
#include <cstdio>

int main() {
    static_assert(sizeof(nvgsp::NvChannelAllocParams) == 368,
                  "channel params size");
    assert(nvgsp::kAmpereChannelGpfifoA == 0xc56f);
    assert(nvgsp::kEngineTypeGraphics == 0x00000001);
    assert(nvgsp::kChannelFlagsUserdPageSlot3 == 0x00200300);
    assert(nvgsp::kChannelInternalFlagsNotifierNone == 0x14);

    // Rejects bad inputs without touching hardware.
    nvgsp::NvChannelAllocParams bad{};
    assert(!nvgsp::buildChannelAllocParams(0, 0, 0, 256,
                                           nvgsp::kEngineTypeGraphics, &bad));
    assert(!nvgsp::buildChannelAllocParams(0xc0d090f1, 0, 0, 0,
                                           nvgsp::kEngineTypeGraphics, &bad));
    assert(!nvgsp::buildChannelAllocParams(0xc0d090f1, 0, 0, 255,
                                           nvgsp::kEngineTypeGraphics, &bad));
    assert(!nvgsp::buildChannelAllocParams(0xc0d090f1, 0, 0, 256, 0xdead,
                                           &bad));
    assert(!nvgsp::buildChannelAllocParams(0xc0d090f1, 0, 0, 256,
                                           nvgsp::kEngineTypeGraphics,
                                           nullptr));

    // Minimal valid graphics channel: VASpace + GPFIFO geometry, no USERD yet
    // (RM allocates USERD internally when hUserdMemory[*]==0).
    nvgsp::NvChannelAllocParams gfx{};
    assert(nvgsp::buildChannelAllocParams(0xc0d090f1, 0, 0x1000, 256,
                                          nvgsp::kEngineTypeGraphics, &gfx));
    assert(gfx.hVASpace == 0xc0d090f1);
    assert(gfx.gpFifoOffset == 0x1000);
    assert(gfx.gpFifoEntries == 256);
    assert(gfx.flags == 0);
    assert(gfx.engineType == nvgsp::kEngineTypeGraphics);
    assert(gfx.hUserdMemory[0] == 0);
    assert(gfx.instanceMem.base == 0 && gfx.ramfcMem.base == 0);
    assert(gfx.hPhysChannelGroup == 0 && gfx.tpcConfigId == 0);

    // Single-GPU client owns USERD only on subdevice 0.
    nvgsp::NvChannelAllocParams withUserd{};
    assert(nvgsp::buildChannelAllocParams(0xc0d090f1, 0xc0d00041, 0x2000, 512,
                                          nvgsp::kEngineTypeGraphics,
                                          &withUserd));
    assert(withUserd.hUserdMemory[0] == 0xc0d00041);
    assert(withUserd.userdOffset[0] == 0);
    for (uint32_t i = 1; i < nvgsp::kMaxSubdevices; ++i)
        assert(withUserd.hUserdMemory[i] == 0 &&
               withUserd.userdOffset[i] == 0);
    assert(withUserd.gpFifoEntries == 512);

    // CPU-RM's split-GSP payload: instance/RAMFC alias, per-channel USERD
    // descriptor and an independent method buffer descriptor.
    withUserd.flags = nvgsp::kChannelFlagsUserdPageSlot3;
    withUserd.internalFlags = nvgsp::kChannelInternalFlagsNotifierNone;
    withUserd.instanceMem = {0x3f0000000ULL, 4096, 2, 1};
    withUserd.ramfcMem = {0x3f0000000ULL, 512, 2, 1};
    withUserd.userdMem = {0x3f0001000ULL, 512, 2, 1};
    withUserd.mthdbufMem = {0x3f0002000ULL, 65536, 2, 0};
    assert(withUserd.ramfcMem.base == withUserd.instanceMem.base);
    assert(withUserd.ramfcMem.size == 512);
    assert(withUserd.userdMem.size == 512);
    assert(withUserd.mthdbufMem.cacheAttrib == 0);

    // Context-DMA allocation params: 32 bytes, exact offsets.
    static_assert(sizeof(nvgsp::NvCtxDmaAllocParams) == 32, "ctxdma size");
    nvgsp::NvCtxDmaAllocParams ctx{};
    ctx.hMemory = 0xc0d00044;
    ctx.flags = nvgsp::kCtxDmaFlagsKernelNoHash;
    ctx.limit = 4095;
    assert(ctx.hSubDevice == 0 && ctx.offset == 0);
    assert(ctx.flags == 0x20100000);

    // Split-RM display programming ABI: physical RM receives this control;
    // the host-owned C77D resource and its CPU mapping do not cross GSP-RPC.
    static_assert(sizeof(nvgsp::NvDispChannelPushbufferParams) == 56,
                  "display pushbuffer control size");
    nvgsp::NvDispChannelPushbufferParams dispPb{};
    dispPb.addressSpace = nvgsp::kAddressSpaceFbmem;
    dispPb.physicalAddr = 0x3f0003000ULL;
    dispPb.limit = 4095;
    dispPb.hclass = nvgsp::kDispCoreChannelDma;
    dispPb.valid = 1;
    dispPb.pbTargetAperture = nvgsp::kPbTargetPhysNvm;
    dispPb.channelPBSize = nvgsp::kChannelPbSize4K;
    assert(dispPb.addressSpace == 2 && dispPb.pbTargetAperture == 1);
    assert(dispPb.hclass == 0xc77d && dispPb.valid == 1);

    std::printf("channel ABI: %zu bytes; class 0x%x; gfx engine %u; "
                "gpFifo %u entries; userd %llu bytes\n",
                sizeof(nvgsp::NvChannelAllocParams),
                nvgsp::kAmpereChannelGpfifoA,
                nvgsp::kEngineTypeGraphics, withUserd.gpFifoEntries,
                (unsigned long long)nvgsp::kUserdBytes);
}
