# NVGspControl

The heart of the stack. This kext takes the RTX 4080 from "the firmware drew a boot picture" to "GSP-RM
is running and we can allocate memory, create channels and program the display".

## Rough flow at boot

1. Resize BAR1 (if `nvgsp-rebar` is set) before anyone maps it, and move the console along with it
2. Parse the VBIOS, run FWSEC-FRTS, then boot GSP through the SEC2 booter
3. Bring RM up over the message queues (the "chain": a list of RPCs and control calls, each one a phase)
4. Allocate the client/device/subdevice, our VA space, GR / CE / display channels
5. Take over the display from the GOP without a flash, then park at phase 33 and serve clients

After that it handles MSI interrupts (GSP, display vblank, GR non-stall), RC / MMU fault events, hotplug,
hardware cursor, DPMS, GPU reset, and S3 sleep/wake with GSP suspend/resume.

Userspace talks to it through `NVGspControlUserClient` (memory objects, VA binds, submits, fences,
present), which is what `nvk-macos` and the tools in `macos-lab-tools/nvrun` use. NVDisplay and
NVAccelerator talk to it in-kernel through `callPlatformFunction`, so there's no link dependency.

## NVRAM knobs

| Variable | Meaning |
|---|---|
| `nvgsp-rebar` | BAR1 size in GiB (e.g. `16`). Unset or `0` = leave BAR1 as macOS set it |
| `nvgsp-rebar-boot` | set by the kext as a boot guard; if a resize crashed, the next boot skips it |
| `nvgsp-sr` | `0` / `off` disables GSP suspend/resume on sleep (falls back to a cold GSP boot on wake) |
| `nvgsp-autoreset` | `1` = reset the GPU automatically after a channel RC |
| `nvgsp-registry` | extra GSP-RM registry keys, packed into `SET_REGISTRY` |

Boot-arg `nvgspdraw` is a debugging switch for the early display takeover, you normally don't need it.

## Tests

`tests/` has host checks for the pure logic in `NVGspCore` (ABI sizes, boot staging, arena maps, the
VRAM heap, chain wait policy and so on). Most build with plain `clang++ -std=gnu++17 -I <workspace>`.
The firmware-parsing ones take the firmware file as argument.
