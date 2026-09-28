#!/bin/sh
# Host test: H.264 front end of the NVDEC driver (ASan/UBSan build).
set -e
cd "$(dirname "$0")/../.."
out=${TMPDIR:-/tmp}/nvdec_h264_check
cc -std=c99 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I. \
   tests/nvdec_h264/nvdec_h264_check.c h264_parse.c nvdec_h264.c -o "$out"
"$out" tests/nvdec_h264/vectors
