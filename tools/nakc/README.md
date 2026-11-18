# Shader compiler

The single current project guide, native build procedure, shader ABI and measured validation results are in the [root README](../../README.md#shader-compiler-and-metal-bundle).

## Tensor cores and bfloat (16 Nov 2026)

- `air_nir.c` lowers `simdgroup_matrix<half|bfloat, 8, 8>` multiply-accumulate to one NAK
  `cmat_muladd_nv` (HMMA m16n8k8, fp32 accumulate); `NAKC_NO_HMMA=1` restores the shuffle/FFMA path.
- bfloat values are fp32 in NIR; loads widen bf16 bits, stores round to nearest even
  (the previous lowering treated bf16 bits as IEEE half and computed wrong results).
- `mesa-26.0.8-nak-bf16-hmma.patch` (apply after `mesa-26.0.8-macos-nak.patch`): NAK HMMA with
  BF16 multiplicands on sm80+ (`HmmaSrcType`), needed for bfloat matrices.
- Checks: `bf16_check` and `smat_check` (fresh-install/bf16) compare against the CPU.
