# nvda-smi

A read-only status tool for the native NVIDIA stack on macOS, in the style of `nvidia-smi`. It reads `ioreg`
(NVGspControl and NVAccelerator properties), `kmutil showloaded`, `launchctl` and the driver logs under
/Library/NativeDriverLab.

| Command | Shows |
|---|---|
| `nvda-smi` | One box: GPU, firmware state, temperature, P-state, GPC and DRAM clocks, VRAM, resets and RC events, display |
| `nvda-smi -l 1` | The same box, refreshed every second |
| `nvda-smi displays` | All 7 display IDs on the board (3 DP + 1 HDMI): connected or not, which one is driven, scan-out and hotplug counters |
| `nvda-smi boot` | Kernel boot timeline, native615 stage times, and the uptime marks from the autogpu and display boot logs |
| `nvda-smi doctor` | Pass/fail checks: kexts loaded, firmware ready, accelerator registered, DP display found, scan-out copy, launch daemons, hotplug watcher. Exits 2 if any check fails |
| `nvda-smi logs [N]` | The last N lines of the newest autogpu, display-boot and hotplug logs |
| `nvda-smi -q [TEXT]` | Every NV* property, filtered by TEXT |

## Install

```
sudo sh install.sh       # -> /usr/local/bin/nvda-smi (root:wheel 755)
```

## Self-check

`python3 test_nvda_smi.py` runs the formatting against fake ioreg data. It works on any OS.
