// Bounded head-0 OLUT data/UPDATE diagnostic. Leaves production routing off.
// Uses separately allocated RM VRAM with a fixed manual display descriptor.
// ARMED acceptance is not proof of gamma affecting the physical display.
#include "nvdisplay_admin.hpp"
#include "nvdisplay_window_probe.hpp"
#include "nvdisplay_cursor_probe.hpp"
#include "nvdisplay_crc_resource.hpp"
#include "../../drivers/NVGspCore/NVGspDisplayLut.hpp"

static constexpr uint32_t backing = 0xc0d000e0, lutDma = 0xc0d000e1;
static constexpr unsigned slot = 235;
static constexpr unsigned legacySlot = slot ^ 4;
static constexpr uint32_t ilutDma = 0xc0d000e2;
static constexpr unsigned ilutSlot = 168, ilutLegacySlot = ilutSlot ^ 4;

struct LutState { Snapshot display; uint32_t assy[4], armed[4], ocscAssy[13], ocscArmed[13]; };
static bool read_state(io_connect_t c, LutState *s) {
    return snapshot(c, &s->display) && peek(c, 0x682280, 4, s->assy) == 0 &&
        peek(c, 0x68a280, 4, s->armed) == 0 &&
        peek(c, 0x682240, 13, s->ocscAssy) == 0 &&
        peek(c, 0x68a240, 13, s->ocscArmed) == 0;
}
static void dump_state(const char *tag, const LutState &s) {
    dump(tag, &s.display);
    for (unsigned i = 0; i < 4; ++i)
        printf("%s OLUT+%02x ASSY=%08x ARMED=%08x\n", tag, i*4, s.assy[i], s.armed[i]);
    for (unsigned i = 0; i < 13; ++i)
        printf("%s OCSC+%02x ASSY=%08x ARMED=%08x\n", tag, i*4, s.ocscAssy[i], s.ocscArmed[i]);
    fflush(stdout);
}
static bool wait_state(io_connect_t c, const char *tag, const LutState &before,
                       const uint32_t state[4], const uint32_t ocsc[13],
                       const WindowProbeState *windowBefore = nullptr,
                       const WindowProbeState *windowWant = nullptr) {
    const auto end = ns_now()+200000000;
    LutState s = {}; WindowProbeState w = {}; bool first = false, firstWindow = false, match = false;
    do {
        if (!read_state(c, &s)) return false;
        if (windowWant && !window_snapshot(c, &w)) return false;
        if (windowBefore && !firstWindow && ((s.display.pending&2) ||
            memcmp(w.exception, windowBefore->exception, sizeof(w.exception)))) {
            window_dump("first-observed-window-exception-change", w); firstWindow = true;
        }
        if (!first && ((s.display.pending&1) || memcmp(s.display.exception,
            before.display.exception, sizeof(s.display.exception)))) {
            dump_state("first-observed-exception-change", s); first = true;
        }
        match = armed_matches(&s.display, before.display.usage[1], before.display.armed) &&
            !memcmp(s.assy, state, sizeof(s.assy)) && !memcmp(s.armed, state, sizeof(s.armed)) &&
            !memcmp(s.ocscAssy, ocsc, sizeof(s.ocscAssy)) &&
            !memcmp(s.ocscArmed, ocsc, sizeof(s.ocscArmed)) &&
            (!windowWant || window_equal(w, *windowWant));
        if (match) break;
        usleep(1000);
    } while (ns_now() < end);
    dump_state(tag, s);
    if (windowWant) window_dump(tag, w);
    printf("%s armed-match=%s\n", tag, match ? "yes" : "no");
    return match;
}
static kern_return_t submit(io_connect_t c, const uint32_t state[4], const uint32_t ocsc[13],
                            bool interlockWindow = false, const Snapshot *cursorRestore = nullptr) {
    uint32_t words[38] = {0x00342240};
    memcpy(words+1, ocsc, 13*4);
    const uint32_t tail[] = {0x00102280, state[0], state[1], state[2], state[3],
                            0x00080218, 0, interlockWindow ? 1U : 0U, 0x00040200, 1};
    memcpy(words+14, tail, sizeof(tail));
    if (cursorRestore) {
        const auto *s = cursorRestore->armed;
        const uint32_t cursor[] = {0x00042030,cursorRestore->usage[1],0x00042098,s[4],
            0x00082088,s[0],s[1],0x00082090,s[2],s[3],0x0004209c,s[5],0x000420a0,s[6]};
        memcpy(words+33, words+19, 5*4);
        memcpy(words+19, cursor, sizeof(cursor));
    }
    return IOConnectCallStructMethod(c, 12, words, (cursorRestore ? 38 : 24)*4, nullptr, nullptr);
}

static int trial(io_connect_t c, io_service_t service, bool halfGain, bool enableOcsc, bool paired,
                  bool cursorTest, bool outputComparison=false, bool cursorOutput=false) {
    LutState before = {}; uint64_t phase = 0, inst = 0;
    uint32_t originalHash[2048], originalObjects[64];
    WindowProbeState windowBefore = {}; uint32_t originalPb[1024];
    if (!read_state(c, &before)) return 1;
    dump_state("before", before);
    if (cursorTest && (before.display.armed[0] || before.display.armed[1] ||
        (before.display.armed[5]&0x80000000U) || (before.display.usage[1]&7))) {
        fprintf(stderr, "cursor comparison requires the original disabled/no-bounds cursor\n"); return 1;
    }
    if (!property_u64(service, CFSTR("NVGspControl-post-init-phase"), &phase) ||
        !property_u64(service, CFSTR("NVGspControl-dispinst-offset"), &inst) ||
        phase != 33 || !inst || inst > vramBytes-0x10000 ||
        !armed_matches(&before.display, before.display.usage[1], before.display.armed) ||
        !(before.display.usage[1]&0x10) || before.armed[2] || before.assy[2] ||
        memcmp(before.assy, before.armed, sizeof(before.armed)) ||
        memcmp(before.ocscAssy, before.ocscArmed, sizeof(before.ocscArmed)) ||
        !vread(c, inst, originalHash, 2048) ||
        !vread(c, inst+0x2000, originalObjects, 64)) {
        fprintf(stderr, "refused: requires phase33, idle matching disabled OLUT and allowed bounds\n"); return 1;
    }
    if(cursorOutput){
        // FREE4 is readable even when fresh boot has no allocated C67A PIO.
        // Initialize through the existing kernel setup route before snapshots
        // that will preserve its cursor descriptor/hash and channel ownership.
        const uint64_t op=0;uint64_t pushbufStatus=~0ULL,allocStatus=~0ULL;
        const auto setup=IOConnectCallScalarMethod(c,35,&op,1,nullptr,nullptr);
        if(setup || !property_u64(service,CFSTR("NVGspControl-cursor-pushbuf-status"),&pushbufStatus) ||
            !property_u64(service,CFSTR("NVGspControl-cursor-alloc-status"),&allocStatus) ||
            pushbufStatus || allocStatus || !read_state(c,&before) ||
            !armed_matches(&before.display,before.display.usage[1],before.display.armed) ||
            before.display.pending || before.display.error || before.display.armed[0] || before.display.armed[1] ||
            (before.display.armed[5]&0x80000000) || (before.display.usage[1]&7) ||
            !vread(c,inst,originalHash,2048) || !vread(c,inst+0x2000,originalObjects,64)){
            fprintf(stderr,"cursor PIO setup unverified: no cursor/CRC trial, reboot required\n");return 1;
        }
        printf("cursor PIO setup transport=%08x pushbuffer=%llx allocation=%llx idle=yes\n",setup,
            (unsigned long long)pushbufStatus,(unsigned long long)allocStatus);
    }
    if (originalHash[slot*2] || originalHash[slot*2+1] ||
        originalHash[legacySlot*2] || originalHash[legacySlot*2+1]) return 1;
    for (unsigned i = 48; i < 56; ++i) if (originalObjects[i]) return 1;
    if (paired) {
        if (!window_guard(c, service, inst, &windowBefore, originalPb) ||
            originalHash[ilutSlot*2] || originalHash[ilutSlot*2+1] ||
            originalHash[ilutLegacySlot*2] || originalHash[ilutLegacySlot*2+1]) {
            fprintf(stderr, "paired guard refused before allocation/mutation\n"); return 1;
        }
        for (unsigned i = 56; i < 64; ++i) if (originalObjects[i]) return 1;
    }
    uint8_t memory[128] = {}, reply[4096]; size_t bytes = 0;
    const uint32_t type = 6, attr = 0x10800000;
    const uint64_t size = cursorTest ? 0x10000 : (paired ? 0x8000 : 0x4000);
    memcpy(memory, &client, 4); memcpy(memory+4, &type, 4);
    memcpy(memory+24, &attr, 4); memcpy(memory+64, &size, 8);
    const auto status = alloc(c, backing, 0x40, memory, sizeof(memory), reply, &bytes);
    if (status == nvgsp::AdminRpcResult::Uncertain) {
        fprintf(stderr, "allocation uncertain: reboot required\n"); return 1;
    }
    if (status != nvgsp::AdminRpcResult::Success) return 1;
    uint64_t offset = 0, allocated = 0; uint32_t actualAttr = 0;
    if (bytes >= 244) {
        memcpy(&offset, reply+196, 8); memcpy(&allocated, reply+180, 8);
        memcpy(&actualAttr, reply+140, 4);
    }
    if (bytes < 244 || !offset || offset > vramBytes-size || (offset&0xfff) ||
        allocated < size || (actualAttr&0x19800000) != attr) {
        fprintf(stderr, "memory reply unverified: backing retained, reboot required\n"); return 1;
    }
    printf("RM backing offset=%llx size=%llx attr=%08x\n",
        (unsigned long long)offset, (unsigned long long)allocated, actualAttr);
    uint32_t table[nvgsp::kIdentityLutWords];
    nvgsp::buildIdentityDisplayLut(table, nvgsp::kIdentityLutWords, false);
    if (halfGain) for (unsigned i = 8; i < nvgsp::kIdentityLutWords; i += 2) {
        const uint32_t value = (table[i]&0xffff)>>1;
        table[i] = value|(value<<16); table[i+1] = value;
    }
    for (unsigned i = 0; i < nvgsp::kIdentityLutWords; i += 1024) {
        const unsigned n = nvgsp::kIdentityLutWords-i < 1024 ? nvgsp::kIdentityLutWords-i : 1024;
        if (!vwrite(c, offset+i*4, table+i, n)) { release(c, backing); return 1; }
    }
    uint32_t readback[nvgsp::kIdentityLutWords];
    if (!vread(c, offset, readback, nvgsp::kIdentityLutWords) ||
        memcmp(table, readback, sizeof(table))) { release(c, backing); return 1; }
    if (paired) {
        nvgsp::buildIdentityDisplayLut(table, nvgsp::kIdentityLutWords, true);
        for (unsigned i = 0; i < nvgsp::kIdentityLutWords; i += 1024) {
            const unsigned n = nvgsp::kIdentityLutWords-i < 1024 ? nvgsp::kIdentityLutWords-i : 1024;
            if (!vwrite(c, offset+0x4000+i*4, table+i, n)) { release(c, backing); return 1; }
        }
        if (!vread(c, offset+0x4000, readback, nvgsp::kIdentityLutWords) ||
            memcmp(table, readback, sizeof(table))) { release(c, backing); return 1; }
    }
    const uint32_t descriptor[] = {5, (uint32_t)(offset>>8), (uint32_t)(offset>>40),
        (uint32_t)((offset+0x3fff)>>8), (uint32_t)((offset+0x3fff)>>40)};
    const uint32_t entry[] = {lutDma, 0x00418001}; // client1/chid0/object20C0
    if (!vwrite(c, inst+0x20c0, descriptor, 5) || !vwrite(c, inst+slot*8, entry, 2) ||
        !vwrite(c, inst+legacySlot*8, entry, 2)) {
        fprintf(stderr, "descriptor write uncertain: backing retained, reboot required\n"); return 1;
    }
    const uint64_t inputOffset = offset+0x4000;
    const uint32_t inputDescriptor[] = {5, (uint32_t)(inputOffset>>8), (uint32_t)(inputOffset>>40),
        (uint32_t)((inputOffset+0x3fff)>>8), (uint32_t)((inputOffset+0x3fff)>>40)};
    const uint32_t inputEntry[] = {ilutDma, 0x0241c001}; // client1/chid1/object20E0
    if (paired && (!vwrite(c, inst+0x20e0, inputDescriptor, 5) ||
        !vwrite(c, inst+ilutSlot*8, inputEntry, 2) ||
        !vwrite(c, inst+ilutLegacySlot*8, inputEntry, 2))) {
        fprintf(stderr, "ILUT descriptor write uncertain: reboot required\n"); return 1;
    }
    uint32_t afterHash[2048], afterObjects[64];
    bool layout = vread(c, inst, afterHash, 2048) &&
        vread(c, inst+0x2000, afterObjects, 64) &&
        !memcmp(afterHash+slot*2, entry, sizeof(entry)) &&
        !memcmp(afterHash+legacySlot*2, entry, sizeof(entry)) &&
        !memcmp(afterObjects+48, descriptor, sizeof(descriptor));
    if (paired) layout = layout && !memcmp(afterHash+ilutSlot*2, inputEntry, sizeof(inputEntry)) &&
        !memcmp(afterHash+ilutLegacySlot*2, inputEntry, sizeof(inputEntry)) &&
        !memcmp(afterObjects+56, inputDescriptor, sizeof(inputDescriptor));
    for (unsigned i = 0; layout && i < 2048; ++i)
        if (i/2 != slot && i/2 != legacySlot &&
            !(paired && (i/2 == ilutSlot || i/2 == ilutLegacySlot)) &&
            afterHash[i] != originalHash[i]) layout = false;
    for (unsigned i = 0; layout && i < 64; ++i)
        if ((i < 48 || i >= 53) && !(paired && i >= 56 && i < 61) &&
            afterObjects[i] != originalObjects[i]) layout = false;
    if (!layout) { fprintf(stderr, "layout unverified: reboot required\n"); return 1; }
    printf("table readback and manual descriptor/RAMHT preservation verified; gain=%s\n",
           halfGain ? "0.5" : "1.0");
    const uint32_t state[] = {0x40509, 0xffffffff, lutDma, 0};
    uint32_t ocsc[13]; memcpy(ocsc, before.ocscArmed, sizeof(ocsc));
    if (enableOcsc) {
        memset(ocsc, 0, sizeof(ocsc)); ocsc[0] = 1;
        ocsc[1] = ocsc[6] = ocsc[11] = 0x10000;
    }
    printf("OCSC identity override=%s\n", enableOcsc ? "yes" : "no");
    WindowProbeState windowTrial = windowBefore; uint32_t windowPut = windowBefore.getput[0];
    if (paired) {
        windowTrial.composition = 0x80; windowTrial.factor = 0x11;
        windowTrial.ilut[0] = 0x40508; windowTrial.ilut[1] = ilutDma; windowTrial.ilut[2] = 0;
        if (!window_queue(c, inst, windowPut, windowTrial, &windowPut)) {
            fprintf(stderr, "window queue uncertain: reboot required\n"); return 1;
        }
    }
    const auto kr = submit(c, state, ocsc, paired);
    printf("OLUT submit=%08x (fetch result only)\n", kr);
    bool accepted = wait_state(c, "OLUT", before, state, ocsc,
        paired ? &windowBefore : nullptr, paired ? &windowTrial : nullptr) && !kr;
    bool cursorAccepted = false;
    if (paired) {
        WindowProbeState observed = {};
        const bool fetched = window_snapshot(c, &observed) && window_equal(observed, windowTrial) &&
            observed.getput[0] == windowPut;
        window_dump("window-trial", observed);
        printf("paired core-armed=%s window-fetch/ASSY-match=%s window-ARM=unpublished\n",
            accepted ? "yes" : "no", fetched ? "yes" : "no");
        accepted = accepted && fetched;
        if(accepted && outputComparison){
            CrcSample sample={};
            if(crc_resource_trial(c,service,true,true,&sample,cursorOutput?offset+0x8000:0,&cursorAccepted)){
                fprintf(stderr,"paired fixture/CRC result unverified: parent LUT backing retained, reboot required\n");return 1;
            }
            printf("paired-output gain=%s compositor=%08x raster=%08x primary-DP-SF=%08x three-captures-stable=yes\n",
                halfGain?"0.5":"1.0",sample.compositor,sample.raster,sample.output);
        }
        if (accepted && cursorTest && !cursorOutput &&
            !cursor_compare(c, inst, offset+0x8000, before.display, &cursorAccepted)) {
            fprintf(stderr, "normal cursor cleanup unverified: backing retained, reboot required\n"); return 1;
        }
        if (!window_queue(c, inst, windowPut, windowBefore, &windowPut)) {
            fprintf(stderr, "window restoration queue uncertain: reboot required\n"); return 1;
        }
    }
    LutState changed = {}; if (!read_state(c, &changed)) changed = before;
    // NVIDIA's viewport setup retains maximum cursor capacity. Reducing
    // NONE after a prepared cursor repeatedly stalls, even in a joint update.
    // Verify disabled/unreferenced resources first; reboot restores bounds.
    LutState restoreExpected = before;
    if (cursorAccepted) restoreExpected.display.usage[0] = restoreExpected.display.usage[1] =
        (before.display.usage[1]&~7U)|2;
    const auto restore = submit(c, before.armed, before.ocscArmed, paired,
                                cursorTest ? &restoreExpected.display : nullptr);
    if (!wait_state(c, "restore", restoreExpected, before.armed, before.ocscArmed,
        paired ? &windowBefore : nullptr, paired ? &windowBefore : nullptr) || restore) {
        fprintf(stderr, "restore unverified: backing retained, reboot required\n"); return 1;
    }
    if (paired && (!window_rewind(c, inst, windowPut, windowBefore, originalPb) ||
        submit(c, before.armed, before.ocscArmed, false, cursorTest ? &restoreExpected.display : nullptr) ||
        !wait_state(c, "restore-no-interlocks", restoreExpected, before.armed, before.ocscArmed))) {
        fprintf(stderr, "window/PB/interlock restoration unverified: reboot required\n"); return 1;
    }
    if (!vwrite(c, inst+slot*8, originalHash+slot*2, 2) ||
        !vwrite(c, inst+legacySlot*8, originalHash+legacySlot*2, 2) ||
        !vwrite(c, inst+0x20c0, originalObjects+48, 5) ||
        (cursorTest && (!vwrite(c, inst+0x20a0, originalObjects+40, 5) ||
                       !vwrite(c, inst+56*8, originalHash+112, 2) ||
                       !vwrite(c, inst+60*8, originalHash+120, 2))) ||
        (paired && (!vwrite(c, inst+ilutSlot*8, originalHash+ilutSlot*2, 2) ||
                    !vwrite(c, inst+ilutLegacySlot*8, originalHash+ilutLegacySlot*2, 2) ||
                    !vwrite(c, inst+0x20e0, originalObjects+56, 5))) ||
        !vread(c, inst, afterHash, 2048) || !vread(c, inst+0x2000, afterObjects, 64) ||
        memcmp(originalHash, afterHash, sizeof(originalHash)) ||
        memcmp(originalObjects, afterObjects, sizeof(originalObjects))) {
        fprintf(stderr, "object restoration unverified: backing retained, reboot required\n"); return 1;
    }
    const bool freed = release(c, backing) == nvgsp::AdminRpcResult::Success;
    printf("result OLUT-armed=%s exact-restoration=%s backing-free=%s output-effect=unverified\n",
           accepted ? "yes" : "no", cursorAccepted ? "no-retained-cursor-bounds" : "yes", freed ? "yes" : "no");
    if (cursorTest) printf("result normal-composition-cursor-armed=%s visual-proof=unverified\n",
                           cursorAccepted ? "yes" : "no");
    if (cursorAccepted) printf("cleanup-except-original-bounds=verified original-bounds-restoration=reboot-required\n");
    return accepted && freed && (!cursorTest || cursorAccepted) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "--identity") && strcmp(argv[1], "--half-gain") &&
                      strcmp(argv[1], "--identity-ocsc") && strcmp(argv[1], "--paired-identity") &&
                      strcmp(argv[1], "--paired-cursor") && strcmp(argv[1], "--paired-output-identity") &&
                      strcmp(argv[1], "--paired-output-half") && strcmp(argv[1], "--paired-output-cursor") && strcmp(argv[1], "--snapshot"))) {
        fprintf(stderr, "usage: sudo nvdisplay_olut_probe --identity|--half-gain|--identity-ocsc|--paired-identity|--paired-cursor|--paired-output-identity|--paired-output-half|--paired-output-cursor|--snapshot\n"); return 2;
    }
    const auto service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    io_connect_t c = 0;
    if (!service || IOServiceOpen(service, mach_task_self(), 0, &c)) {
        if (service) IOObjectRelease(service); return 1;
    }
    if (!strcmp(argv[1], "--snapshot")) {
        LutState s = {}; uint64_t inst = 0; uint32_t hash[2048], objects[64];
        const bool ok = read_state(c, &s) &&
            property_u64(service, CFSTR("NVGspControl-dispinst-offset"), &inst) &&
            inst && inst < vramBytes-0x10000 && vread(c, inst, hash, 2048) &&
            vread(c, inst+0x2000, objects, 64);
        if (ok) {
            dump_state("snapshot", s);
            for (unsigned i = 0; i < 1024; ++i)
                if (hash[i*2] || hash[i*2+1]) printf("snapshot hash[%u]=%08x/%08x\n", i, hash[i*2], hash[i*2+1]);
            for (unsigned i = 40; i < 48; ++i) printf("snapshot cursor-object+%02x=%08x\n", (i-40)*4, objects[i]);
            uint32_t crc[4]={},descriptor[5]={};
            if(!peek(c,0x682180,2,crc) && !peek(c,0x68a180,2,crc+2))
                printf("snapshot CRC ASSY=%08x/%08x ARM=%08x/%08x\n",crc[0],crc[1],crc[2],crc[3]);
            if(vread(c,inst+0x2100,descriptor,5)){
                const uint64_t begin=uint64_t(descriptor[1])<<8 | uint64_t(descriptor[2])<<40;
                const uint64_t limit=(uint64_t(descriptor[3])<<8 | uint64_t(descriptor[4])<<40)|0xff;
                printf("snapshot CRC object=%08x begin=%llx limit=%llx\n",descriptor[0],
                    (unsigned long long)begin,(unsigned long long)limit);
                uint32_t notifier[1024];
                if(descriptor[0]==5 && !(begin&0xfff) && begin && begin<=vramBytes-4096 &&
                    limit==begin+4095 && vread(c,begin,notifier,1024)){
                    printf("snapshot CRC notifier status=%08x count=%u overflow=%x\n",notifier[0],
                        (notifier[0]>>16)&0xfff,notifier[0]&0x38);
                    for(unsigned i=0;i<64;++i)if(notifier[i])printf("snapshot CRC word[%u]=%08x\n",i,notifier[i]);
                }
            }
        }
        IOServiceClose(c); IOObjectRelease(service); return ok ? 0 : 1;
    }
    const bool cursorOutput=!strcmp(argv[1],"--paired-output-cursor");
    const bool outputComparison=!strcmp(argv[1],"--paired-output-identity") || !strcmp(argv[1],"--paired-output-half") || cursorOutput;
    const auto result = trial(c, service, !strcmp(argv[1], "--half-gain") || !strcmp(argv[1],"--paired-output-half"),
                               !strcmp(argv[1], "--identity-ocsc") || !strcmp(argv[1], "--paired-identity") ||
                               !strcmp(argv[1], "--paired-cursor") || outputComparison,
                               !strcmp(argv[1], "--paired-identity") || !strcmp(argv[1], "--paired-cursor") || outputComparison,
                               !strcmp(argv[1], "--paired-cursor") || cursorOutput,outputComparison,cursorOutput);
    IOServiceClose(c); IOObjectRelease(service); return result;
}
