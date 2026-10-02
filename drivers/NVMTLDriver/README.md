# NVMTLDriver

This is the user-space Metal bundle loaded for NVAccelerator. Current source and installed bundle are 0.8.69. WindowServer uses it on the Tahoe test machine; [the current receipts](../../docs/evidence/2026-10-02/README.md) show the desktop and focused Metal/depth checks.

The bundle handles buffers, texture views, render/compute pipelines, command buffers, blits and synchronization. AIR goes through the local nakc/NAK compiler rather than a system NVIDIA compiler. That path is still incomplete: Geekbench 7 Background Blur hits an unsupported AIR typed load. Passing the focused suite does not mean every Metal feature or MPS app works.

Build with `sh build.sh [out dir]`. The compiler source and Mesa build patches are in `../../tools/nakc`; `libnakc.dylib` is supplied locally at build/install time. Installation into the test system and system snapshot is separate from compilation. No ready-to-install bundle is shipped here.
