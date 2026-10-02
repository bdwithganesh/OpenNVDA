# NVK on macOS

The Mesa 26.0.8 port is `mesa-26.0.8-macos-nvk.patch` in this folder. The patch under `tools/nakc` is an older compiler-era snapshot, so use this one for the Vulkan driver.

Each submission restores context initialization and queue state before the command-buffer pushes, in one kernel batch. Push-stream allocations remain alive through the recorded completion fence. Submission splitting, shared channels and reset handling still have limits; this is not full Vulkan conformance or queue-isolation certification.

An earlier 1 October focused draw.renderpass selection reported 1896 passes and 152 unsupported cases, with zero failures; the separate restoration build had 370 failures in that selection. Those results were not rerun for this publication. Current [2 October evidence](../../docs/evidence/2026-10-02/README.md) is for display and focused Metal checks.

The resumable runner is in `tools/nvk-cts`. It journals completed cases and stops on requested crash/timeout boundaries. It does not recover the GPU by itself. Build the patched source in a configured x86 Mesa NVK tree:

```sh
ninja -C build-nvk-x86 src/nouveau/vulkan/libvulkan_nouveau.dylib
```
