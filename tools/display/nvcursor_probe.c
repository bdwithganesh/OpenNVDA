// Bounded head-0 cursor diagnostic. Does not enable NVDisplay cursor routing.
// Build: clang -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation
//        tools/display/nvcursor_probe.c -o nvcursor_probe
// --snapshot is read-only. --bounds changes only usage; --disabled/--show
// upload a fixed 64px image with enable off/on, respectively. Capture the
// first exception before recovery, waits for ARMED (not just GET==PUT), then
// restores the exact prior cursor state. No register/physical-address inputs.
#include "nvcursor_common.h"

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "--snapshot") && strcmp(argv[1], "--show") &&
                     strcmp(argv[1], "--bounds") && strcmp(argv[1], "--disabled") &&
                     strcmp(argv[1], "--staged"))) {
        fprintf(stderr, "usage: sudo nvcursor_probe --snapshot|--bounds|--disabled|--show|--staged\n"); return 2;
    }
    const bool trial = strcmp(argv[1], "--snapshot") != 0;
    const bool bounds = !strcmp(argv[1], "--bounds");
    const bool enable = !strcmp(argv[1], "--show");
    const bool staged = !strcmp(argv[1], "--staged");
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                        IOServiceMatching("NVGspControl"));
    io_connect_t c = 0;
    if (!service || IOServiceOpen(service, mach_task_self(), 0, &c)) {
        fprintf(stderr, "NVGspControl open failed\n");
        if (service) IOObjectRelease(service);
        return 1;
    }
    int result = 1;
    Snapshot before = {0}, setup = {0};
    if (!snapshot(c, &before)) goto done;
    dump("before", &before);
    if (!trial) { result = 0; goto done; }
    uint64_t scratch = 0, phase = 0;
    if (!property_u64(service, CFSTR("NVGspControl-scratch-offset"), &scratch) ||
        !property_u64(service, CFSTR("NVGspControl-post-init-phase"), &phase) ||
        phase != 33 || !scratch || scratch > UINT64_MAX - 0xff08000 ||
        before.getput[0] != before.getput[1] ||
        ((before.core >> 16) & 0x1f) != 0xb || (before.armed[5] & 0x80000000)) {
        fprintf(stderr, "trial refused: requires idle core, phase33, disabled cursor\n");
        goto done;
    }
    uint64_t op = 0;
    kern_return_t kr = bounds ? KERN_SUCCESS :
        IOConnectCallScalarMethod(c, 35, &op, 1, NULL, NULL);
    printf("setup kr=%08x\n", kr);
    if (kr || !snapshot(c, &setup)) goto done;
    const uint64_t setup_end = ns_now() + 200000000;
    while (((setup.core >> 16) & 0x1f) != 0xb && ns_now() < setup_end) {
        usleep(1000);
        if (!snapshot(c, &setup)) goto done;
    }
    dump("after-setup", &setup);
    // cursorSetup issues an UPDATE: require idle before adding a second one.
    if (setup.getput[0] != setup.getput[1] ||
        ((setup.core >> 16) & 0x1f) != 0xb) {
        fprintf(stderr, "trial refused: setup left core busy; no image/show submitted\n");
        goto done;
    }
    uint32_t image[4096];
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x)
            image[y * 64 + x] = (x < 4 || y < 4 || x > 59 || y > 59)
                                  ? 0xffff0000 : 0x800000ff;
    for (unsigned i = 0; !bounds && i < 4; ++i) {
        uint64_t in[3] = {scratch + 0xff00000 + i * 4096, 1024, 1};
        kr = IOConnectCallMethod(c, 11, in, 3, image + i * 1024, 4096,
                                 NULL, NULL, NULL, NULL);
        if (kr) { fprintf(stderr, "upload kr=%08x\n", kr); goto done; }
    }
    uint32_t state[7] = {0xc0d0d002, 0xc0d0d002, 0, 0, 0,
                        enable ? 0x800001cf : 0x1cf, 0x75ff};
    if (bounds) memcpy(state, before.armed, sizeof(state));
    const uint32_t usage = (before.usage[1] & ~7U) | 2;
    kr = core(c, usage, state);
    printf("%s submit kr=%08x (fetch result only)\n", argv[1], kr);
    bool armed = wait_armed(c, argv[1], &setup, usage, state);
    if (staged && armed && !kr) {
        Snapshot prepared = {0};
        if (!snapshot(c, &prepared)) prepared = setup;
        state[5] |= 0x80000000;
        kr = core(c, usage, state);
        printf("staged-enable submit kr=%08x (fetch result only)\n", kr);
        armed = wait_armed(c, "staged-enable", &prepared, usage, state);
        // Disable at the prepared bounds first: shrinking usage on a live
        // head may stall. The following exact-restore phase checks that too.
        Snapshot enabled = {0};
        if (!snapshot(c, &enabled)) enabled = prepared;
        state[5] &= ~0x80000000;
        kr = core(c, usage, state);
        printf("staged-disable submit kr=%08x\n", kr);
        if (!wait_armed(c, "staged-disable", &enabled, usage, state) || kr)
            armed = false;
    }
    // Always attempt exact prior state restoration, even after failed fetch.
    Snapshot failed = {0};
    if (!snapshot(c, &failed)) failed = setup;
    kr = core(c, before.usage[1], before.armed);
    printf("restore submit kr=%08x\n", kr);
    bool restored = wait_armed(c, "restore", &failed, before.usage[1], before.armed);
    printf("result trial=%s trial-armed=%s prior-state-armed=%s; visual proof still required\n",
           argv[1], armed ? "yes" : "no", restored && !kr ? "yes" : "no");
    if (kr || !restored) fprintf(stderr, "restore unverified: reboot required before further trials\n");
    result = armed && restored && !kr ? 0 : 1;
done:
    IOServiceClose(c);
    IOObjectRelease(service);
    return result;
}
