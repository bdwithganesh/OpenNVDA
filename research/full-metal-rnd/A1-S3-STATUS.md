# A1 S3 resume - status (25 Sep 21:06 IST, read-only check)

Doc item: Phase A A1 - S3 resume: GSP re-boot (0.99.1 WPR meta fix) + modeset + picture.

## Live state (ssh read-only, no sleep triggered)
- Boot: 20:48:31 chain, phase 33, modeset 0x0, bar1-bound Yes, bad-reason 0
- pmset sleep 0 → S3 auto-sleep OFF (panic-cycle prevention, worklog 25 Sep 20:xx)
- NVDisplay 0.7.0 pagingState fix: TEST 1 (20:37-20:44) NO PANIC, WindowServer alive 6+ min after wake
- 0.99.1 fix: GspStaging::rewriteMetadata() every executeBoot (bootCount/verified reset, NVIDIA kernel_gsp_tu102.c:816 mirror)
- Cold-boot regression: NONE (20:48 chain + modeset 0x0, exec-meta-rewritten Yes)

## Open (TEST 2)
- Worklog last line: "S3 TEST 2 start." - result not logged yet.
- Live uptime 18 min (20:48 boot → 21:06 check) → TEST 2 ya to ho chuka (no panic visible) ya abhi pending.
- Next (needs user + LOCK): RTC-wake S3 cycle with serial log capture:
  1. `pmset sleep 1` nahi - sirf ek manual `pmset sleepnow` with RTC wake
  2. wake par: gsp-alive? chain phase? head0/SOR/vblank? WindowServer pid alive?
  3. agar phase 1 par stuck (TEST 1 jaisa) → GSP cpuctl 0x111388 + mbox0 check (HALTED 0x10?)

## Rule
- Is workspace se S3 trigger NAHI karenge bina user confirm + LOCK ke.
- Ye file sirf status ke liye hai; asli test NVGspControl ke sleep/wake flow se hoga.
