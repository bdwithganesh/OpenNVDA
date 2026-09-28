# NVFramebuffer

Our first real GPU kext, and where the whole project started. It publishes the linear surface the
UEFI GOP already set up as an IOFramebuffer, so WindowServer has *something* to draw on. No modesets,
no GSP, no acceleration.

It also carries a lot of debugging machinery from the days when the machine froze during boot and we
had no serial port: an on-screen HUD, NVRAM tracing, a UDP log to another machine. All of that is off
unless you pass `nvfb-diag=1` / `nvfb-nvram=1`. The comment at the top of `NVFramebuffer.cpp` tells
the story if you're curious.

NVDisplay replaced it for day-to-day use; keep this one as a fallback.
