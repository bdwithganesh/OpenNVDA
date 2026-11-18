# NVMTLDriver

This is the user-space Metal bundle loaded for NVAccelerator. Current source and installed bundle are 0.9.86. WindowServer composites the whole desktop through it, Liquid Glass included ([19 November evidence](../../docs/evidence/2026-11-19/README.md)). Recent fixes:

- 0.9.74: atomic shader cache writes.
- 0.9.76: L2-cacheable system memory.
- 0.9.81: render pass descriptor snapshots, which fixed icon cross-contamination.
- 0.9.83: block-linear GOB rows.
- 0.9.85: helper-lane loads before derivatives, which fixed the glass seams.
- 0.9.86: one GPU mapping per IOSurface.

The bundle handles buffers, texture views, render/compute pipelines, command buffers, blits and synchronization. AIR goes through the local nakc/NAK compiler rather than a system NVIDIA compiler. That path is still incomplete. Ray tracing and tier 2 argument buffers aren't there, blits between VRAM buffers are slow, and passing our suites doesn't mean every Metal feature or MPS app works.

Build with `sh build.sh [out dir]`. The compiler source and Mesa build patches are in `../../tools/nakc`; `libnakc.dylib` is supplied locally at build/install time. Installation into the test system and system snapshot is separate from compilation. No ready-to-install bundle is shipped here.
