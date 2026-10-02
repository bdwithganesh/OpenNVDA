#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/nvk-dgc-check.XXXXXX")
trap 'rm -rf "$OUT"' EXIT HUP INT TERM
${CXX:-c++} -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    "$ROOT/tests/nvk_dgc/submit_check.cpp" -o "$OUT/submit_check"
"$OUT/submit_check"
