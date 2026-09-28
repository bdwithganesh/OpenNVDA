# NVDisplay

The IOFramebuffer driver WindowServer sees. It doesn't program the hardware itself; NVGspControl owns
the display engine, and NVDisplay asks it for modesets, vblank callbacks, cursor and DPMS.

What it does:

- Publishes the scan-out surface in BAR1 (it follows BAR1 around if NVGspControl resized/moved it)
- Reads the EDID and builds the mode list from it (`NVDisplayEdid.hpp`)
- Real vblank interrupts, so the desktop isn't just running on a timer
- Hotplug, display sleep (DPMS) and wake
- Kicks `IONDRVFramebuffer` off the device so there's only one framebuffer

Boot-arg `nvdisp-hz` forces a refresh rate if the EDID timing picked is not the one you want.

```sh
sh tools/build_kext.sh NVDisplay /path/to/MacKernelSDK
clang++ -std=gnu++17 -I <workspace> tests/nvdisplay_edid_check.cpp -o /tmp/edid && /tmp/edid
```
