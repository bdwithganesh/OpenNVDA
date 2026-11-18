# 19 November 2026 checkpoint

What's in this folder, and how far each file actually goes.

| File | What it shows | What it doesn't |
|---|---|---|
| `installed-stack.txt` | kextstat and bundle versions on the test guest, runtime state (`native615-ready = Yes`), and what System Information reports (RTX 4080, 16 GB, "Metal Support: Metal 4", 4K at 60 Hz) | That macOS treats it like an Apple-supported GPU in every framework |
| `mtlbench.txt` | Fill, copy, blit and FP32 numbers measured through plain Metal with `tools/bench/mtlbench.m` | App performance. And FP32 comes out well below the raw number, for reasons not yet known (see below) |
| `regressions.txt` | The regression run after the 0.180.187 kernel went in: Metal 3 9/9, Metal 4 16/16, blur, Liquid Glass at 60 fps, hang and page-fault recovery | Full Metal conformance. Those are our own contract tests, not Apple's |
| `host-checks.txt` | `sh tests/run_host_checks.sh`: 47 pass, 8 skipped (7 need NVIDIA firmware you have to supply yourself, 1 needs a fault capture that isn't in the fixtures), 0 fail. Ran on the guest with Apple clang | Anything about hardware. Nothing is loaded |
| `probe-gate.txt` | Selector 48 called as root and as a normal user on 0.180.187: probe/EDID flags refused for the user (`0xe00002c1`), bad flag bits refused for both (`0xe00002c2`) | That every other selector is locked down the same way |
| `desktop-scanout-720p.png` | A 1280×720 downscale of the 4K frame read straight out of the scan-out buffer in VRAM, 18 Nov 21:06, NVMTLDriver 0.9.85 | The hardware cursor and the output LUT stage (the read happens before the LUT) |

## The setup changed

The 2 October results came from macOS running on the PC directly. This checkpoint runs macOS Tahoe 26.7.3 as a Hyper-V guest on Windows 11. The RTX 4080 is handed to the guest whole with Discrete Device Assignment (DDA). The guest drives the card itself: GSP boot, memory, channels and the display engine. The monitor (a BenQ PD2705U) is plugged into the card. The VM only provides the CPU (8 vCPUs of a Ryzen 9 7950X), 32 GB of RAM and the disks.

So these numbers are not a bare-metal rerun. Host↔GPU bandwidth in particular goes through the hypervisor's IOMMU, which probably explains part of the 21-22 GB/s.

## Source matches what's installed

Several sources were checked with md5 against the build directories on the guest:

- NVGspControl 0.180.187 (ESP receipt UUID `D1E45607-FB9D-3E11-963D-796888C85949`), NVAccelerator 0.5.37, NVMTLDriver 0.9.86 and nakc 84b are byte-identical to what the installed binaries were built from.
- NVDisplay is the 1.4.1 source that is installed. The guest's build tree holds a 1.4.2 that was tried and rolled back.

A few edits went in while preparing this push.

Built and installed as NVGspControl 0.180.187:

- `cursorTest` now returns the cursor move result, not just the image upload result. This came from the 2 October branch.
- The submit selector's display probe and EDID flags now need an admin client. Unknown flag bits are rejected. `probe-gate.txt` has the calls, as root and as a normal user.

Source only, not rebuilt yet:

- NVAccelerator and NVDisplay `module.cpp` now declare the same kmod name and version as their `Info.plist`. NVGspControl got the same fix in 0.180.187. A rebuilt accelerator needs a new bundle id before it will load, so that one waits for the next real accelerator change.

## Known weak spots in these numbers

- **FP32 through Metal is 21 TFLOPS, about 44 % of what the raw runtime got.** The cause isn't known yet. Memory bandwidth is fine, since fill and copy match the old numbers. Boost requests reach GSP: 4162 were sent this boot, and the P-state read-back says P0. `NVGspControl-boost-native-failed = Yes` is a sticky flag that only means at least one request was refused while the runtime was busy. Still to check: the shader clock we actually get, and the code nakc emits for this kernel (the September 47.7 came from hand-written kernels).
- **Metal blits between VRAM buffers manage only ~45 GB/s,** while a compute copy of the same buffers does ~298 GB/s. Why isn't investigated yet.
