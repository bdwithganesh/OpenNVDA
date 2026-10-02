# Display probes

These are bounded diagnostics for the RTX 4080 test setup, not production display controls. The current cursor output trial stalls the display core; its failure and reboot receipts are in [the evidence folder](../../docs/evidence/2026-10-02/README.md).

On macOS, build the combined probe with:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror nvdisplay_olut_probe.cpp \
  -framework IOKit -framework CoreFoundation -o nvdisplay_olut_probe
sudo ./nvdisplay_olut_probe --snapshot
```

`--snapshot` only reads state. The other modes allocate GPU memory and change head/window/CRC/cursor state, with strict guards and restoration checks. They are specific to the recorded setup. A fetched packet or ASSY match is reported separately from actual core ARM. An uncertain stop or cleanup retains memory and ends the trial; it must not turn into a forced free.

The fixed gray-ramp and identity/half/identity CRC comparison passes on installed control 0.178.27. The enabled-cursor CRC comparison does not. Leave production cursor routing on software until bitmap/output and lifecycle behavior are proved.
