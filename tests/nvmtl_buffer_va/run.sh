#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/nvmtl-buffer-va.XXXXXX")
trap 'rm -rf "$OUT"' EXIT HUP INT TERM
clang -fobjc-arc -Wall -Wextra -Werror -fsanitize=undefined \
    -framework Foundation -framework Metal "$ROOT/tests/nvmtl_buffer_va/check.m" -o "$OUT/check"
"$OUT/check"
