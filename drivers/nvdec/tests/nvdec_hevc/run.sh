#!/bin/sh
# Host test: HEVC syntax parser (ASan/UBSan build). Vector: x265 (PyAV) 208x120, 20
# frames, bframes=3, keyint=16, scaling-list=default.
set -e
cd "$(dirname "$0")/../.."
out=${TMPDIR:-/tmp}/hevc_parse_check
cc -std=c99 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I. \
   tests/nvdec_hevc/hevc_parse_check.c hevc_parse.c h264_parse.c -o "$out"
"$out" tests/nvdec_hevc/vectors/main_b_scaling.h265
