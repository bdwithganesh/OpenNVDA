# NVGspCore

Header-only pieces shared by NVGspControl and its host tests. Nothing in here touches IOKit or
hardware directly, which is exactly why it can be tested on any machine.

Some of the bits:

- `NVGspAbi.hpp`, `NVGspInitAbi.hpp` – GSP-RM r570.144 message and RPC layouts
- `NVGspBoot*`, `NVGspFwsec*`, `NVGspVbios.hpp`, `NVGspFalcon.hpp` – firmware parsing and staging for the boot chain
- `NVGspArenaMap.hpp`, `NVGspArenaPages.hpp` – per-client GPU VA arenas (2 MiB and 64 KiB pages)
- `NVGspVramHeap.hpp`, `NVGspEvict.hpp` – VRAM allocator and eviction
- `NVGspChannel.hpp`, `NVGspVideo.hpp` – channel and video-engine bring-up helpers
- `NVGspChainWait.hpp` – when to stop draining the status queue

Layouts come from NVIDIA's open-gpu-kernel-modules (MIT) and nouveau; the headers say which file.
