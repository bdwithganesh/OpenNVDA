# NVGspControl

This kext owns GSP boot, RM messages, GPU memory, channels, interrupts and the display engine. NVDisplay and NVAccelerator call it through platform functions; user-space tools use NVGspControlUserClient.

The usual boot path resizes BAR1 when requested, parses the local firmware inputs, runs FWSEC/SEC2, starts GSP-RM, creates the RM resources and brings the display up to phase 33. Firmware is NVIDIA's and is not included. Generate `NVGspBooterUnloadBlob.hpp` from your own matching file with `tools/gen_booter_unload.py` before compiling.

Source and installed version are both 0.180.186 ([19 November evidence](../../docs/evidence/2026-11-19/README.md)). Since October the kext boots GSP-RM 615.78.08 and runs its own runtime on top of it (the `*615*` files): page tables and external VA spaces, per-client channels, fault decoding, and per-process recovery from hangs and page faults. It also owns double-buffered scan-out with window-channel flips at vblank, and gamma in the head's output LUT. 0.180.186 makes IOSurface wrap/unwrap check only both ends of each page-table run instead of reading every entry back: a 4K surface wraps in 2.8 ms instead of 34 ms.

The cursor path now requires an exact idle ASSY+ARM match before accepting image/show/hide state, with bounded failure cleanup. Cursor point/update stores now each require free PIO space, with timeout/read/write errors returned to the diagnostic caller. Reserved capacity is retained. The diagnostic normal pipeline accepts an enabled cursor state, but cursor bitmap output still has no proof and enabled CRC capture stalls. Software routing stays in place.

Shared-channel recovery has an earlier checked result. Private post-reset recovery still failed with RC109; sleep/wake, all-day use and per-queue isolation remain unqualified. The presence of those code paths does not mean they are release-ready.

Hardware-free checks run with `sh tests/run_host_checks.sh` from the repo root. Firmware parsing tests need your own firmware input and are skipped by default.
