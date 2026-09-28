# nvdec

Hardware video decode on NVDEC (class `NVC9B0` on AD103) for H.264 and HEVC.

The NVDEC firmware does the heavy lifting (slice data, CABAC, reconstruction). What the driver has
to do is everything around it, and that's what lives here:

- `h264_parse.*`, `hevc_parse.*` – bitstream parsers (SPS, PPS, the slice header parts we need),
  written from the ITU-T specs
- `nvdec_h264.*` – the H.264 decoding process: POC, reference marking, output order, picture setup
  structs, the slice table and the method stream
- `nvdec_drv.h` – NVIDIA's own NVDEC interface header (MIT, from open-gpu-doc), unchanged

All of it is plain C99 without allocation, so it builds for the host and for macOS the same way.

## Tests

```sh
sh tests/nvdec_h264/run.sh     # x264 vectors, checks POC, marking, output order, methods, tiling
sh tests/nvdec_hevc/run.sh     # x265 stream through the HEVC parser
```

Both build with ASan/UBSan. The small test vectors are in `tests/*/vectors`, generated with
`tools/nvdec/gen_vectors.py`.
