# NVGspControl

This kext owns GSP boot, RM messages, GPU memory, channels, interrupts and the display engine. NVDisplay and NVAccelerator call it through platform functions; user-space tools use NVGspControlUserClient.

The usual boot path resizes BAR1 when requested, parses the local firmware inputs, runs FWSEC/SEC2, starts GSP-RM, creates the RM resources and brings the display up to phase 33. Firmware is NVIDIA's and is not included. Generate `NVGspBooterUnloadBlob.hpp` from your own matching file with `tools/gen_booter_unload.py` before compiling.

Published control source is 0.178.29. The SDK build and actual-source host checks pass, but installed control on the recorded Tahoe runs is 0.178.27. Keep that difference in mind when using [the evidence](../../docs/evidence/2026-10-02/README.md).

The cursor path now requires an exact idle ASSY+ARM match before accepting image/show/hide state, with bounded failure cleanup. Reserved capacity is retained. The diagnostic normal pipeline accepts an enabled cursor state, but cursor bitmap output still has no proof and enabled CRC capture stalls. Software routing stays in place.

Shared-channel recovery has an earlier checked result. Private post-reset recovery still failed with RC109; sleep/wake, all-day use and per-queue isolation remain unqualified. The presence of those code paths does not mean they are release-ready.

Hardware-free checks run with `sh tests/run_host_checks.sh` from the repo root. The older relocated pure-core tests remain in this folder's `tests/` directory. Firmware parsing tests need your own firmware input and are not part of the default host check run.
