# nvk-macos

Mesa's NVK (the open Vulkan driver for NVIDIA) running on macOS on top of NVGspControl.

NVK talks to the kernel through a small backend interface called `nvkmd`. On Linux that's nouveau's
DRM; here `nvkmd_macos.c` implements it with our kext's user client instead (memory objects, VA binds,
submits, fences). The rest of NVK runs pretty much unchanged, which is the nice part.

## Files

- `mesa-26.0.8-macos-nvk.patch` – changes to the Mesa tree (build glue, the new backend hook)
- `nvkmd_macos.c` – the backend itself
- `nvk_macos_wsi.c`, `nvk_macos_layer*.{c,m}` – presenting to a window / to the screen
- `libdrm_stubs.c` – the few libdrm functions NVK expects, stubbed
- `vkcopy.c`, `vkpresent.c`, `vkwindow.m` – small test programs

## Building

Get Mesa 26.0.8, apply the patch, and build NVK with meson as usual (`-Dvulkan-drivers=nouveau`,
`-Dgallium-drivers=`), pointing it at this folder. It's fiddly on macOS the first time; you'll need
python3 with mako, meson and ninja.

## Tests

`tests/nvkmd_macos/run.sh <patched mesa tree>` runs the backend against a mock of the kext, no GPU
needed (it runs on Linux too).
