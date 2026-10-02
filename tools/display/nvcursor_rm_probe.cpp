// Fixed head-0 RM-owned cursor comparison for GSP 570.144.
// --bind-only validates allocation/bind/readback/unbind/free without UPDATE.
// --show also submits one 64px cursor and restores the exact initial state.
// --manual-owned-show compares RM VRAM backing with the existing manual DMA.
// --manual-owned-maxbounds uses NVIDIA's normal maximum usage bound (256),
// while the cursor image/control remain 64px. It is a diagnostic comparison.
// Build: clang++ -std=c++17 -Wall -Wextra -Werror -framework IOKit
//        -framework CoreFoundation tools/display/nvcursor_rm_probe.cpp -o nvcursor_rm_probe
#include "nvcursor_common.h"
#include "nvdisplay_admin.hpp"

static constexpr uint32_t memory = 0xc0d000d0, dma = 0xc0d000d1;
static constexpr uint32_t channel = 0xc0d0c77d, hashSlot = 219;

struct Instance {
    uint32_t hash[2048];
    uint32_t objects[64]; // byte offsets 0x2000..0x20ff
};

static bool instance(io_connect_t c, uint64_t address, Instance *s, const char *tag) {
    if (!vread(c, address, s->hash, 2048) ||
        !vread(c, address + 0x2000, s->objects, 64)) return false;
    for (unsigned i = 0; i < 1024; ++i)
        if (s->hash[2*i] || s->hash[2*i+1])
            printf("%s hash[%u]=%08x %08x\n", tag, i,
                   s->hash[2*i], s->hash[2*i+1]);
    for (unsigned i = 0; i < 64; i += 8)
        printf("%s object[%04x]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
               tag, 0x2000 + i*4, s->objects[i], s->objects[i+1],
               s->objects[i+2], s->objects[i+3], s->objects[i+4],
               s->objects[i+5], s->objects[i+6], s->objects[i+7]);
    fflush(stdout);
    return true;
}

static nvgsp::AdminRpcResult bind(io_connect_t c, bool attach) {
    const uint32_t p[] = {client, dma, attach ? 0x20102U : 0x20103U,
                          0, 4, 0, channel};
    uint8_t reply[4096]; size_t bytes;
    return rpc(c, 76, p, sizeof(p), reply, &bytes);
}

static bool verified_binding(const Instance &before, const Instance &after,
                             uint64_t offset) {
    // RM client ID 1, core chid 0, first non-null 32B-aligned descriptor 0x2020.
    if (after.hash[hashSlot*2] != dma || after.hash[hashSlot*2+1] != 0x00404001)
        return false;
    for (unsigned i = 0; i < 2048; ++i)
        if (i/2 != hashSlot && before.hash[i] != after.hash[i]) return false;
    for (unsigned i = 0; i < 64; ++i)
        if ((i < 8 || i >= 16) && before.objects[i] != after.objects[i]) return false;
    const uint32_t expected[] = {5, (uint32_t)(offset >> 8),
        (uint32_t)(offset >> 40), (uint32_t)((offset+0x7fff) >> 8),
        (uint32_t)((offset+0x7fff) >> 40)};
    return !memcmp(after.objects+8, expected, sizeof(expected));
}

static int trial(io_connect_t c, io_service_t service, bool show, bool manual,
                 bool maxBounds, bool bypass) {
    Snapshot before = {}, setup = {};
    Instance initial = {}, prebind = {}, bound = {}, unbound = {};
    uint64_t phase = 0, inst = 0;
    if (!snapshot(c, &before)) return 1;
    dump("before", &before);
    if (bypass) {
        uint32_t composition = 0;
        if (peek(c, 0x6902ec, 1, &composition) || !(composition&0x10000)) {
            fprintf(stderr, "refused: bypass comparison requires live bypass window\n"); return 1;
        }
        printf("window composition=%08x; cursor bypass comparison\n", composition);
    }
    if (!property_u64(service, CFSTR("NVGspControl-post-init-phase"), &phase) ||
        !property_u64(service, CFSTR("NVGspControl-dispinst-offset"), &inst) ||
        phase != 33 || inst > vramBytes-0x10000 || !inst ||
        !armed_matches(&before, before.usage[1], before.armed) ||
        (before.armed[5] & 0x80000000) ||
        (manual && (before.armed[0] || before.armed[1])) ||
        !instance(c, inst, &initial, "initial")) {
        fprintf(stderr, "refused: requires phase33, idle matching disabled cursor and instance\n");
        return 1;
    }
    for (unsigned i = 8; i < 16; ++i) if (initial.objects[i]) {
        fprintf(stderr, "refused: candidate descriptor 0x2020 is occupied\n"); return 1;
    }
    if (initial.hash[hashSlot*2] || initial.hash[hashSlot*2+1]) {
        fprintf(stderr, "refused: candidate RAMHT slot is occupied\n"); return 1;
    }
    uint64_t op = 0;
    if (IOConnectCallScalarMethod(c, 35, &op, 1, nullptr, nullptr) ||
        !wait_armed(c, "setup", &before, before.usage[1], before.armed) ||
        !snapshot(c, &setup) ||
        !armed_matches(&setup, before.usage[1], before.armed) ||
        !instance(c, inst, &prebind, "prebind")) {
        fprintf(stderr, "setup unverified; no RM allocation submitted\n"); return 1;
    }
    for (unsigned i = 8; i < 16; ++i) if (prebind.objects[i]) return 1;
    if (prebind.hash[hashSlot*2] || prebind.hash[hashSlot*2+1]) return 1;
    bool memoryLive = false, dmaLive = false, isBound = false, uncertain = false;
    bool manualWritten = false;
    bool restoreSafe = true, accepted = !show;
    uint64_t offset = 0;
    uint8_t mem[128] = {}, reply[4096]; size_t replyBytes;
    const uint32_t type = 6, attr = 0x10800000;
    const uint64_t size = 0x8000;
    memcpy(mem, &client, 4); memcpy(mem+4, &type, 4);
    memcpy(mem+24, &attr, 4); memcpy(mem+64, &size, 8);
    auto status = alloc(c, memory, 0x40, mem, sizeof(mem), reply, &replyBytes);
    memoryLive = status == nvgsp::AdminRpcResult::Success;
    uncertain = status == nvgsp::AdminRpcResult::Uncertain;
    if (memoryLive) {
        if (replyBytes < 244) uncertain = true;
        else {
            uint32_t returnedAttr = 0; uint64_t returnedSize = 0;
            memcpy(&offset, reply+196, 8);
            memcpy(&returnedAttr, reply+116+24, 4);
            memcpy(&returnedSize, reply+116+64, 8);
            printf("memory returned attr=%08x size=%llx\n", returnedAttr,
                   (unsigned long long)returnedSize);
            if ((returnedAttr&0x19800000) != attr || returnedSize < size) uncertain = true;
        }
    }
    printf("memory offset=%llx size=8000 contiguous-4k\n", (unsigned long long)offset);
    if (memoryLive && !uncertain && offset && offset <= vramBytes-size && !(offset&0xfff)) {
        if (manual) {
            const uint32_t descriptor[] = {5, (uint32_t)(offset>>8),
                (uint32_t)(offset>>40), (uint32_t)((offset+size-1)>>8),
                (uint32_t)((offset+size-1)>>40)};
            // Existing setup owns this fixed manual slot; save and restore it.
            manualWritten = true;
            if (!vwrite(c, inst+0x20a0, descriptor, 5) ||
                !instance(c, inst, &bound, "manual-owned") ||
                memcmp(bound.objects+40, descriptor, sizeof(descriptor))) uncertain = true;
            else {
                isBound = true;
                for (unsigned i = 0; i < 2048; ++i)
                    if (bound.hash[i] != prebind.hash[i]) uncertain = true;
                for (unsigned i = 0; i < 64; ++i)
                    if ((i < 40 || i >= 45) && bound.objects[i] != prebind.objects[i]) uncertain = true;
            }
        } else {
            nvgsp::NvCtxDmaAllocParams ctx = {};
            ctx.flags = nvgsp::kCtxDmaFlagsRwNoHash | 0x20000;
            ctx.hMemory = memory; ctx.limit = size-1;
            status = alloc(c, dma, nvgsp::kContextDma, &ctx, sizeof(ctx), reply, &replyBytes);
            dmaLive = status == nvgsp::AdminRpcResult::Success;
            uncertain = status == nvgsp::AdminRpcResult::Uncertain;
            if (dmaLive && !uncertain) {
                status = bind(c, true);
                isBound = status == nvgsp::AdminRpcResult::Success;
                uncertain = status == nvgsp::AdminRpcResult::Uncertain;
                if (isBound && (!instance(c, inst, &bound, "bound") ||
                    !verified_binding(prebind, bound, offset))) {
                    fprintf(stderr, "binding layout/window preservation unverified; reboot required\n");
                    uncertain = true;
                }
            }
        }
        if (isBound && !uncertain) {
            printf("verified %s descriptor=%04x other entries/objects unchanged\n",
                   manual ? "manual DMA with RM backing" : "RM binding",
                   manual ? 0x20a0 : 0x2020);
            if (show) {
                uint32_t image[4096];
                for (unsigned i = 0; i < 4096; ++i) image[i] = 0xffff0000;
                bool uploaded = true;
                for (unsigned i = 0; i < 4; ++i) {
                    if (!vwrite(c, offset+i*4096, image+i*1024, 1024)) {
                        uploaded = false; break;
                    }
                }
                if (uploaded) {
                    const uint32_t h = manual ? 0xc0d0d002 : dma;
                    const uint32_t state[] = {h, h, 0, 0, 0, 0x800001cf,
                                             bypass ? 0x1075ffU : 0x75ffU};
                    const uint32_t usage = (before.usage[1]&~7U)|(maxBounds ? 4 : 2);
                    const auto kr = core(c, usage, state);
                    printf("show submit=%08x (fetch result only)\n", kr);
                    accepted = wait_armed(c, "owned-show", &setup, usage, state) && !kr;
                    Snapshot changed = {};
                    if (!snapshot(c, &changed)) changed = setup;
                    const auto restore = core(c, before.usage[1], before.armed);
                    restoreSafe = wait_armed(c, "restore", &changed,
                        before.usage[1], before.armed) && !restore;
                }
            }
        }
    }
    // Never free a possibly bound/ARMED object after uncertain RPC or restore.
    if (uncertain || !restoreSafe) {
        fprintf(stderr, "resources retained: reboot required before further trials\n"); return 1;
    }
    if (manualWritten) {
        if (!vwrite(c, inst+0x20a0, prebind.objects+40, 5) ||
            !instance(c, inst, &unbound, "manual-restored") ||
            memcmp(&prebind, &unbound, sizeof(prebind))) {
            fprintf(stderr, "manual descriptor restore unverified; resources retained, reboot required\n"); return 1;
        }
    } else if (isBound) {
        if (bind(c, false) != nvgsp::AdminRpcResult::Success ||
            !instance(c, inst, &unbound, "unbound") ||
            memcmp(prebind.hash, unbound.hash, sizeof(prebind.hash)) ||
            memcmp(prebind.objects, unbound.objects, 8*4) ||
            memcmp(prebind.objects+16, unbound.objects+16, 48*4)) {
            fprintf(stderr, "unbind preservation unverified; resources retained, reboot required\n"); return 1;
        }
    }
    if (dmaLive && release(c, dma) != nvgsp::AdminRpcResult::Success) return 1;
    if (memoryLive && release(c, memory) != nvgsp::AdminRpcResult::Success) return 1;
    Snapshot final = {};
    const bool restored = snapshot(c, &final) &&
        armed_matches(&final, before.usage[1], before.armed);
    if (restored) dump("final", &final);
    printf("result dma-kind=%s ready=%s cursor-armed=%s restored=%s visual-proof=unverified\n",
           manual ? "manual-with-RM-backing" : "RM-bound", isBound ? "yes" : "no", show && accepted ? "yes" : "no", restored ? "yes" : "no");
    return isBound && accepted && restored ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "--bind-only") && strcmp(argv[1], "--show") &&
                     strcmp(argv[1], "--manual-owned-show") &&
                     strcmp(argv[1], "--manual-owned-maxbounds") &&
                     strcmp(argv[1], "--manual-owned-bypass"))) {
        fprintf(stderr, "usage: sudo nvcursor_rm_probe --bind-only|--show|--manual-owned-show|--manual-owned-maxbounds|--manual-owned-bypass\n"); return 2;
    }
    const auto service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("NVGspControl"));
    io_connect_t c = 0;
    if (!service || IOServiceOpen(service, mach_task_self(), 0, &c)) {
        if (service) IOObjectRelease(service);
        return 1;
    }
    const int result = trial(c, service, strcmp(argv[1], "--bind-only") != 0,
                            !strcmp(argv[1], "--manual-owned-show") ||
                            !strcmp(argv[1], "--manual-owned-maxbounds") ||
                            !strcmp(argv[1], "--manual-owned-bypass"),
                            !strcmp(argv[1], "--manual-owned-maxbounds"),
                            !strcmp(argv[1], "--manual-owned-bypass"));
    IOServiceClose(c); IOObjectRelease(service);
    return result;
}
