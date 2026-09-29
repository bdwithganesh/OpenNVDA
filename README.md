# OpenNVDA

**An RTX 4080 running natively on macOS. Not a framebuffer hack: the GPU's own firmware booted from a macOS kext, a real display driver, compute, video decode, Vulkan, and Metal slowly coming up on top.**

[![Buy me a coffee](https://img.buymeacoffee.com/button-api/?text=Buy%20me%20a%20coffee&emoji=&slug=bdwithganesh&button_colour=FFDD00&font_colour=000000&font_family=Cookie&outline_colour=000000&coffee_colour=ffffff)](https://www.buymeacoffee.com/bdwithganesh)

> Made for fun, on one PC, by one person with a lot of reboots. It is not a product and it is not ready for your daily machine. If you want it to become a real driver, see [Funding](#want-this-to-become-a-real-driver) below.

Apple never shipped a driver for any NVIDIA card newer than Kepler. From Mojave onwards there is nothing at all. On a modern Hackintosh an RTX card is dead weight, or at best a dumb framebuffer on the firmware's GOP screen.

We wanted to see how far we could actually get. Turns out, pretty far.

---

## What actually works today

Everything below was run on real hardware, on this exact box, not in a simulator:

- **Machine:** MSI X870E GAMING PLUS WIFI, Ryzen 9 7950X, RTX 4080 16 GB (AD103)
- **OS:** macOS Sonoma 14.8.9
- **GPU firmware:** NVIDIA GSP-RM r570.144

| Area | Status |
|---|---|
| GSP-RM firmware boot from a macOS kext (FWSEC, SEC2 booter chain, all done by us) | ✅ works |
| DisplayPort modeset at 4K 60 Hz, EDID, hotplug, vblank interrupts, DPMS | ✅ works |
| Resizable BAR set up by the kext itself: 16 GiB BAR1, even though macOS only sizes BARs up to 1 GiB | ✅ works |
| Write-combined BAR1 mappings (fixed the PAT so XNU's WC is real): CPU writes to VRAM 2.1 → 9.4 GB/s | ✅ works |
| GPU idles in P8 like on Windows and boosts to ~2850 MHz under load | ✅ works |
| Compute: our own QMD submission with shaders compiled by Mesa's NAK | ✅ works |
| Copy engines, NVK (Mesa Vulkan) running on our kext, host-visible buffers in VRAM (vkcopy 44 GB/s) | ✅ works |
| NVDEC: H.264 decode through VideoToolbox, bit-exact against libavcodec | ✅ works |
| NVDEC: HEVC | ✅ works (host tests + target) |
| Metal: device, buffers, textures, blits, compute, render, MSAA, tessellation, MPS kernels | 🟡 works in tests, needs hardening |
| WindowServer compositing on our GPU and flipping through IOAccelDisplayPipe | 🟡 works, flips are still a CPU copy (6.6 ms per 4K frame) |
| Activity Monitor GPU graph, per-app GPU memory | 🟡 new, lightly tested |
| Full login session on the native Metal path | 🔴 a GR hang in some draws in a real user session, being chased |
| Hardware cursor | 🔴 not yet (core channel exception) |
| GPU reset recovery without reboot | 🔴 partial |
| Sleep (S3) | 🔴 GSP comes back, but the display PLL isn't reprogrammed so the monitor sees no signal |
| macOS Tahoe 26 | 🔴 not ported yet. Tahoe installs and boots on the same PC with the GPU driver off |

## The numbers (the part we are proudest of)

Raw compute through our own runtime (`nvrun` + NAK shaders), warmed up. Same PC, same card, booted into each OS:

| Test | Linux (NVIDIA 570) | Windows (NVIDIA) | **macOS + OpenNVDA** |
|---|---|---|---|
| GPU memory fill | 663 GB/s | 649 GB/s | **668.5 GB/s** |
| GPU copy kernel | 293 GB/s | 290 GB/s | **309.0 GB/s** |
| FP32 compute | 47.27 TFLOPS | 47.38 TFLOPS | **47.72 TFLOPS** |
| Host ↔ GPU (pinned / shared) | 26.9 / 26.4 GB/s | 26.8 / 26.4 GB/s | 26 GB/s |

So yes, on raw compute the 4080 on macOS keeps up with NVIDIA's own drivers.

Be honest with yourself about what this means, though. These numbers come from our low-level path, not from Metal apps. Metal on top is much younger. Tiled SGEMM through Metal does about 3.2 TFLOPS today and dispatch latency is around 50 µs. Closing that gap is the main work left.

How these were measured: `macos-lab-tools/nvrun`, `macos-lab-tools/bench` and the gold kernels in `macos-lab-tools/nakc/kernels`. The Linux and Windows numbers were taken on the same box with the official NVIDIA driver.

## What's in here

| Folder | What it is |
|---|---|
| `drivers/NVGspControl` | The main kext. Boots GSP, owns RM, memory, channels, interrupts, the display engine, sleep/wake |
| `drivers/NVGspCore` | Header-only core shared by the kexts and the host tests (ABI structs, boot staging, VA arena, heap) |
| `drivers/NVDisplay` | IOFramebuffer driver on top of NVGspControl, the thing WindowServer talks to |
| `drivers/NVAccelerator` | IOAcceleratorFamily2 accelerator, the kernel half of Metal |
| `drivers/NVMTLDriver` | Metal driver bundle, the user half of Metal |
| `drivers/nvdec` | H.264 and HEVC decode on NVDEC |
| `drivers/nvenc` | NVENC encode, early |
| `drivers/NVVTDecoder` | VideoToolbox decoder plug-in (parked experiment) |
| `drivers/nvk-macos` | Mesa's NVK Vulkan driver, ported to run on our kext |
| `drivers/NVFramebuffer` | The first framebuffer kext, only scans out the GOP surface. Kept as a fallback |
| `drivers/NVFBProbe` | The early probe that measured BARs, console and VBIOS before we wrote anything real |
| `research/full-metal-rnd` | Notes and experiments on the road to full Metal |
| `tools/gen_booter_unload.py` | Makes the one NVIDIA firmware header the kext needs, from your own linux-firmware copy |

## Building

You need MacKernelSDK and a workspace made by `macos-lab/workspace.sh` (it links `drivers/NV*` into the layout the build script expects):

```sh
python3 tools/gen_booter_unload.py /path/to/linux-firmware/nvidia/ad103/gsp/booter_unload-570.144.bin
sh tools/build_kext.sh NVGspControl /path/to/MacKernelSDK [gsp-package.bin]
sh tools/build_kext.sh NVDisplay    /path/to/MacKernelSDK
```

**No NVIDIA firmware is in this repo, and none ever will be.** GSP-RM, the booters and the VBIOS stay NVIDIA's. You take them from linux-firmware or your own NVIDIA driver install, and `macos-lab-tools/nvgsp_package.py` packs them for the kext.

Host tests live next to each part, for example:
- `drivers/NVGspControl/tests`
- `drivers/nvdec/tests/*/run.sh`
- `drivers/nvk-macos/tests/nvkmd_macos/run.sh`

## Please read before you try it

- It's written for **one** card (AD103) on **one** board. Other Ada cards may get somewhere. Anything else won't.
- It needs our OpenCore setup (`macos-lab-efi`), SIP and authenticated root off, and a second OS you can boot when a test goes wrong.
- A bad GPU state can hang the machine until a cold reboot. Keep your data backed up. We are not responsible for anything that happens to your machine.
- Not affiliated with NVIDIA or Apple in any way. All trademarks belong to their owners.

## Want this to become a real driver?

Right now it is a hobby. Nights and weekends on one PC. To turn it into something other people can install and trust, it needs:
- time (months, not weeks);
- a couple more NVIDIA cards to test on (a 4070/4090, a 30-series);
- a spare machine so one broken boot doesn't stop everything.

If you want an RTX card working on macOS for real, you can help fund it:

<a href="https://www.buymeacoffee.com/bdwithganesh"><img src="https://img.buymeacoffee.com/button-api/?text=Buy%20me%20a%20coffee&emoji=&slug=bdwithganesh&button_colour=FFDD00&font_colour=000000&font_family=Cookie&outline_colour=000000&coffee_colour=ffffff" alt="Buy me a coffee" height="45"></a>

Sponsors and backers get their name here. If a company or a group wants to fund proper work (more cards, Tahoe support, a stable release), open an issue titled **"Funding"** and we'll talk.

Even a star or a share helps. It tells us people actually want this.

## Credits

**Built by [bdwithganesh](https://github.com/bdwithganesh).** Being honest about how: a lot of the code was written with AI help. The rest was me:
- the idea, and the stubbornness to keep going;
- every direction call;
- the hardware and every single test on it: hundreds of reboots, Linux and Windows reference captures, reading logs at 3 am;
- deciding what "working" means.

This work stands on other people's shoulders:
- **nouveau** developers, for years of reverse engineering NVIDIA GPUs (MIT)
- **NVIDIA** for open-gpu-kernel-modules and open-gpu-doc (MIT), without which GSP-RM would be a black box
- **Mesa**, and especially the NVK and NAK folks, for the Vulkan driver and the shader compiler we build on (MIT)
- **envytools**, for register docs
- **acidanthera** for MacKernelSDK, Lilu and OpenCore
- **WebKit** for the VideoToolbox SPI declarations (BSD 2-clause)

Where our code follows one of them closely, the comment right there says so. Full list with licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

MIT, see [LICENSE](LICENSE). Third-party files keep their own headers and licences.
