# Third-party notices

OpenNVDA builds on other people's work. The files below keep their original copyright and licence headers, and those headers are what counts. This page is just the index.

## Apple Inc. (via WebKit)

| File | Where it came from | Licence |
|---|---|---|
| `drivers/NVVTDecoder/spi/CMBaseObjectSPI.h` | WebKit, `Source/ThirdParty/libwebrtc/Source/webrtc/sdk/WebKit/CMBaseObjectSPI.h` (commit `8cd1f6cc395ca82effa2968efe87339d54be2b66`) | BSD 2-clause, Copyright (C) 2020 Apple Inc. |
| `drivers/NVVTDecoder/spi/VTVideoDecoderSPI.h` | WebKit, `Source/ThirdParty/libwebrtc/Source/webrtc/webkit_sdk/WebKit/VTVideoDecoderSPI.h` | BSD 2-clause, Copyright (C) 2020 Apple Inc. |

These only declare the VideoToolbox/CoreMedia decoder interfaces, the same way WebKit uses them for its own decoders.

## NVIDIA Corporation

| File | Licence |
|---|---|
| `drivers/nvdec/nvdec_drv.h` | MIT, Copyright (c) 1993-2024 NVIDIA CORPORATION & AFFILIATES |
| `drivers/nvenc/nvenc_drv.h` | MIT, Copyright (c) 1993-2023 NVIDIA CORPORATION & AFFILIATES |

These are NVIDIA's published video engine interface headers (open-gpu-doc / open-gpu-kernel-modules).

**NVIDIA firmware is not in this repo.** GSP-RM, the booters, the VBIOS: none of it. `tools/gen_booter_unload.py` builds the one header the kext needs from your own linux-firmware copy, and checks its hash first.

## Mesa

| Path | Licence |
|---|---|
| `drivers/nvk-macos/mesa-26.0.8-macos-nvk.patch` | Applies to Mesa 26.0.8 (NVK, the Vulkan runtime, WSI). Mesa is MIT. The touched files keep their Mesa copyright headers (Collabora Ltd., Red Hat Inc., Intel Corporation and the other Mesa contributors). Our changes are under the same MIT terms. |

## Read, not copied

Most of the hardware knowledge came from reading these. No code was copied, but where a register or a sequence follows one of them, the comment says so:
- nouveau (Linux DRM), MIT
- NVIDIA open-gpu-kernel-modules, MIT/GPL dual
- envytools
- acidanthera MacKernelSDK (headers used at build time, not shipped here)
