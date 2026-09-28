# full-metal-rnd

Research notes and experiments for full Metal acceleration on the RTX 4080: what Apple's
IOAcceleratorFamily2 and Metal.framework expect from a vendor driver, what AppleParavirtGPU does, and
how our GSP-side pieces (channels, memory, fences) have to line up with that.

Most notes are in Hinglish, since that's how we wrote them while working. `SCOPE.md` is the plan, the folders follow it: `A3-modes` (mode list and IMP per mode),
`B0-mem-manager`, `B1-compute`, and the KDK symbol work in `KDK-USE/`.

Target: Sonoma 14.8.9 (23J631), GSP r570.144. The Apple KDK itself isn't in this repo, grab it from
Apple's developer site if you want to rerun the symbol scripts.
