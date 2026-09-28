# NVMTLDriver

The user half of Metal: `NVMTLDriver.bundle`, which Metal.framework loads for any accelerator whose
`MetalPluginName` points to it (that's NVAccelerator).

What works right now, checked on the real card with `metal_test` (stages 1 to 11):

- buffers (alloc, CPU map, free)
- command queues and command buffers
- blit fill and copy on the copy engine
- compute, through a small MetalSL subset that we turn into GLSL, then SPIR-V, then NAK
- a basic triangle render into a texture or an IOSurface, RGBA8 and BGRA8, indexed draws too
- fences and shared events

`metal_present` puts a Metal render on the actual screen, and `metal_bench` gave about 9.6 GFLOPS
SGEMM, 4.1 GB/s copy and 2.5 GB/s fill (all through a 1 MiB staging buffer, so slow).

Still missing: proper fixed-function 3D, a real shader compiler, texture sampling,
`newLibraryWithData` (AIR), and WindowServer using us for the desktop. Also WindowServer won't load
the bundle anyway since it's not a platform binary.

The compute path needs the shader tools (glslangValidator, nakc) in `/usr/local/libexec` on the
machine.

Build with `sh build.sh [out dir]` (plain clang, the bundle lands in `build/` by default).
