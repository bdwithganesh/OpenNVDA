# NVDisplay

The IOFramebuffer part of the stack. WindowServer talks to this driver; NVGspControl owns the hardware programming. NVDisplay publishes the scanout surface, EDID modes, vblank callbacks, hotplug and DPMS hooks.

Current installed/source display version is 0.9.6. The recorded target has a 4K60 accelerated desktop. Hardware cursor production routing is disabled: accepted cursor register state has not established bitmap output. Production gamma, direct scan-out, HDR/VRR and sleep/wake qualification are still open. See [the current evidence](../../docs/evidence/2026-10-02/README.md).

Build from the repo root with `sh tools/build_kext.sh NVDisplay /path/to/MacKernelSDK`. The build only compiles; installation and validation are separate. `sh tests/run_host_checks.sh` includes the EDID host check.
