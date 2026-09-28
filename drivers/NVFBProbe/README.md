# NVFBProbe

A read-only survey kext from the very beginning of the GPU work. It attaches to nothing (Sonoma's
IONDRVFramebuffer keeps the GOP desktop), it just measures things and publishes them as
`NVFBProbe-*` properties: where the boot console lives (BAR1 + 0), surface geometry, BAR sizes, the
PCI command register, fuse versions for the GSP and SEC2 ucode, and so on.

Later it also became the test bed for the first GSP boot experiments, behind boot-args that are all
off by default:

| Boot-arg | What it tries |
|---|---|
| `nvgspreset=1` | a GSP falcon reset |
| `nvgspfwsec=1` | running FWSEC from the VBIOS |
| `nvgspstage=1` | staging the embedded GSP package (needs a build with the package embedded) |
| `nvgspresource=1` | loading the GSP package through `OSKextRequestResource` instead |

All of that moved into NVGspControl long ago. Keep this one for when the firmware hand-off looks odd
and you want numbers without starting the whole driver.
