# NVAccelerator

The kernel half of Metal. It publishes an IOAcceleratorFamily2 device for the RTX 4080, owns system-memory/surface and display-pipe plumbing, and loads NVMTLDriver through the Metal plug-in name. Actual GPU execution goes through NVGspControl.

Installed/source version is 0.5.17. WindowServer is using the accelerator on the recorded Tahoe setup; [the current evidence](../../docs/evidence/2026-10-02/README.md) has the focused suite and actual desktop frames. That does not establish every framework hook, reset/fault path or app workload.

The launcher still waits for `NVAcceleratorGo` after a clean GSP boot, with the user-space crash guard. Keep that gate separate from the GPU personality: waiting for IOResourceMatch there can prevent WindowServer from getting a device. Reset containment and stamp watchdogs have actual-source host checks in the repo root tests.

`IOAF2.hpp` and the associated declarations describe Apple's private kernel interfaces. They are used at build time; a successful compile is not proof of compatibility with another macOS release.
