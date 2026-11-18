# NVAccelerator

The kernel half of Metal. It publishes an IOAcceleratorFamily2 device for the RTX 4080, owns system-memory/surface and display-pipe plumbing, and loads NVMTLDriver through the Metal plug-in name. Actual GPU execution goes through NVGspControl.

Installed/source version is 0.5.37 (bundle id `NVAccelerator15`: unsigned AuxKC approval is per binary, so every rebuilt accelerator gets a new id). It publishes a hardware display pipe to CoreDisplay, with a plane scaler, plane gamma caps and post-gamma routed to the head's output LUT. That is what took CoreDisplay's full-screen colour shader off the GPU (desktop 54 → 60 fps, GPU load ~65 % → ~5 %). Frames still reach scan-out through a CPU copy in `copySurface`. See [the 19 November evidence](../../docs/evidence/2026-11-19/README.md).

The launcher still waits for `NVAcceleratorGo` after a clean GSP boot, with the user-space crash guard. Keep that gate separate from the GPU personality: waiting for IOResourceMatch there can prevent WindowServer from getting a device. Reset containment and stamp watchdogs have actual-source host checks in the repo root tests.

`IOAF2.hpp` and the associated declarations describe Apple's private kernel interfaces. They are used at build time; a successful compile is not proof of compatibility with another macOS release.
