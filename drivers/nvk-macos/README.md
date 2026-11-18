# NVK on macOS

The canonical Mesa 26.0.8 port is `mesa-26.0.8-macos-nvk.patch` in this directory.
The older patch under `tools/nakc` is a separate compiler-era snapshot.

## Shared GR state restoration

Every macOS command-buffer submission rebuilds the context initialization and
queue state (descriptor pools and local shader memory). Those packets are
allocated from the queue's push stream and deferred until the command-buffer
pushes are ready. All packets enter one `nvkmd_ctx_exec` call, with the restore
packets first. The push-stream completion fence is recorded after execution so
its allocations remain alive until the GPU completes them. Queue teardown and
creation failure release the pending packet array. Allocation failures return a
Vulkan error. Linux submission behavior is unchanged.

The previous owner query followed by separate restoration ioctls allowed another
client to change GR state between restore and draw. Restoring on every submit
also handles state changes which occur immediately after an ownership query.

The backend still splits submissions at 64 segments, without inserting a fence
inside an incomplete method chain. Combining the restore with the command pushes
closes their interleaving within one kernel batch. It does not provide per-client
channels or preserve state across larger batches interleaved with other clients.
GPU semaphore acquisition, copy-engine traffic and reset handling remain in the
existing macOS backend.

## Verified on 1 October 2026

On Tahoe 26.7 / RTX 4080 / NVGspControl 0.177.1 / Metal 0.8.60:

- Baseline draw.renderpass selection: 1526 Pass, 370 Fail, 152 NotSupported.
- Combined restoration: 1896 Pass, 0 Fail, 152 NotSupported, same 2048 cases.
- Isolated dynamic-input family: 432 Pass, 0 Fail, 8 NotSupported.
- Checked final build: compute and CE-to-graphics copy pass (32 MiB x 8,
  zero bad rounds). Full draw CTS repeated with Metal suite overlap: 1896 Pass,
  152 NotSupported, 0 Fail; Metal 39 invocations rc0 / failures0.
- Promoted default ICD: compute passes; windowed present 120 frames passes
  at 50.9 fps. No GPU events in these runs. Previous default library backed up
  on the target under `~/nvk-tahoe/codex-backups/`.

The changed queue .c/.h patch applies cleanly to original Mesa sources and
reproduces the cross-built source exactly. Build in an already configured x86
Mesa NVK tree with:

```sh
ninja -C build-nvk-x86 src/nouveau/vulkan/libvulkan_nouveau.dylib
```

Live evidence is in the main checkout's
`evidence/gpu-takeover-codex-20261001/`. Run CTS using the loader directory in
`DYLD_LIBRARY_PATH`, as shown in `tools/nvk-cts/README.md`. These are focused
correctness results; broad concurrent and day-long stability remain unverified.
