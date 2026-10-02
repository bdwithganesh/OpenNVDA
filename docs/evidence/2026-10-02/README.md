# Tahoe display and Metal checks, 2 October 2026

The driver is still in development. We have an accelerated desktop and useful focused tests, but a few proper blockers remain. Here are the results behind the README, including the failed cursor run.

The machine is an RTX 4080 / Ryzen 9 7950X / MSI X870E system on Tahoe 26.7, with NVIDIA 570.144 firmware. [installed-stack.txt](installed-stack.txt) records the loaded versions. Installed versions were control 0.178.27, accelerator 0.5.17, Metal 0.8.69 and display 0.9.6. Published control source is 0.178.30. Its host checks and SDK build pass; it has not been deployed yet.

## Desktop frames

- [Safari at 20:50 IST](safari-on-tahoe.png): the actual target showing the public OpenNVDA page before this update. This was after the 18:21:37 reboot and the fresh 20:49 test run.
- [Desktop at 18:00 IST](desktop-after-lut.png): after the controlled LUT comparison, state restoration and focused tests.

These are untouched 3840×2160 PNGs from `fbgrab`, reading the target scanout surface at VRAM offset 0 with pitch 16384. They were visually checked before publication. Reading that surface shows desktop pixels and the software pointer. It does not include separate hardware cursor or output LUT stages, so these pictures cannot prove either of those features.

## Fresh health check

[post-reboot-health.txt](post-reboot-health.txt) records 39 distinct Metal cases with rc=0, suite exit=0 and the depth/stencil test with 75 checks, zero failures, exit=0. WindowServer PID was 736; control phase was 33 and OS errors were zero. The display core was idle, GET=PUT=0x14, with a disabled/null cursor and the original LUT state.

That is a focused regression result after recovery. It does not stand in for all-day use, a full login replay, sleep/wake or broad app compatibility. The current software cursor and shared recovery routing stay in place.

## Controlled LUT output comparison

The source is a separately allocated opaque 3840×2160 gray ramp, pitch 16384. Every upload/readback was checked, so changing desktop content cannot explain the difference below. Each phase had three stable hardware CRC captures.

| Phase | Compositor | Raster | Primary DP/SF |
|---|---|---|---|
| Identity, before | `0ab14391` | `7f406264` | `5cf9eeb7` |
| Half gain | `0ab14391` | `7a53fd22` | `8c99cfba` |
| Identity, restored | `0ab14391` | `7f406264` | `5cf9eeb7` |

The compositor CRC is unchanged; raster and DP/SF change with half gain and return exactly. This establishes a reversible output LUT effect for this fixed input. It does not establish calibrated monitor colour accuracy or a working production gamma API.

Raw runs: [identity before](lut-identity-before.txt), [half gain](lut-half-gain.txt), [identity after](lut-identity-after.txt). The [comparison JSON](lut-comparison.json) keeps the values together. Each run records actual core ARM, CRC stop/unreference, state restoration and backing-memory free. Window fetch/ASSY is recorded separately; unpublished window ARM registers were not inferred from it.

## Cursor output: still failing

The test puts a fixed 64-pixel cursor into the accepted normal composition/ILUT/OLUT pipeline. Enabled cursor state reaches actual core ARM. But the output test then fails: CRC stop stalls with core `a00c0007`, notifier status zero and count zero. The expected red → green → red data comparison never reaches green. There is no completed enabled-cursor CRC and no bitmap/output proof.

- [PIO setup trial](cursor-with-pio-setup.txt): canonical setup transport, pushbuffer and allocation statuses are all zero, but CRC still stalls.
- [PIO point trial](cursor-with-pio-point.txt): matching two free-space guards and point 64,64 / UPDATE 0 are checked at fetch/ASSY. Same stall; no successful cursor output claim.
- [Recovery receipt](cursor-failure-recovery.txt): possibly referenced allocations were retained, then the unchanged driver was rebooted. They were not force-freed while the display might still reference them.

Shrinking the reserved cursor capacity also stalls in an earlier trial. Cleanup therefore retains the reservation until reboot. IMP/resource reservation is the next thing to investigate; it is only a suspect, not a diagnosed cause. Repeating the same enabled-CRC trial would add no new evidence.

## Source and build boundary

[cursor-pio-sdk-build.txt](cursor-pio-sdk-build.txt) records the compile-only 0.178.30 SDK result: binary SHA256 `86149846bf4f823009a217068763a858279754c436c194f66848e285cab293a8`, control source SHA256 `7edcd358a2490e754e91fdcdd179e60b2f551379d48030c2990582f99df28da5`, poll stack 6408 bytes below the 8192-byte build limit. The binary is not shipped here. [cursor-arm-sdk-build.txt](cursor-arm-sdk-build.txt) keeps the preceding 0.178.29 compile receipt.

The source waits for exact idle cursor ASSY+ARM before accepting an image or show/hide transition. It preserves reserved capacity and records failure cleanup separately. The latest change fixes a separate PIO error: cursorMove previously wrote the point and UPDATE even when the free-space read failed or never became ready, then returned success. It now checks the six-bit free count before each store, bounds each wait to 100 one-millisecond sleeps, checks both writes, and returns the error through cursorTest too. The old actual method fails the new regression; the new one passes delayed-space, map/read/write/timeout, coordinate and caller-result checks. This source has not been deployed, and this fix is not a diagnosed cure for the enabled-cursor CRC stall.

[source-files.json](source-files.json) gives the published source/tool/test digests; [host-checks.txt](host-checks.txt) records checks run from this public checkout. Linux CI also runs the checks. Its first two runs exposed an unsigned-address warning and a redundant class memset in test fixtures; both were corrected, keeping `-Werror`. [The corrected run passed](https://github.com/bdwithganesh/OpenNVDA/actions/runs/37028039899). The next source update adds the cursor PIO test to that same workflow. Host mocks check code paths, not physical GPU output.

## Other open gaps

Private post-reset recovery failed with RC109. Geekbench 7 Metal Background Blur aborts at an unsupported AIR typed load; a 30-second OpenCL diagnostic did not complete, so there is no score. Earlier shared recovery and low-level performance results were not rerun here. All-day/login/sleep, per-queue isolation, full visual/app coverage, advanced display/Metal/video and installation qualification remain unfinished. No current readiness percentage or full-driver completion claim is made.

Text receipts only replace private home paths and target addresses with generic labels. Register values, CRCs, statuses and outcomes are retained. [SHA256SUMS](SHA256SUMS) covers the evidence files, including the original screenshot bytes.
