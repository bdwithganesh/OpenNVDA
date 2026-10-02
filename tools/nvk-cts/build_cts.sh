#!/bin/sh
# Build deqp-vk (Khronos Vulkan CTS) on the macOS target for NVK testing.
#   tools/nvk-cts/build_cts.sh [DIR]      (default ~/nvk/cts)
# Needs git, cmake, ninja, python3 (the NVK build venv has meson/ninja).
set -eu
TAG=${CTS_TAG:-vulkan-cts-1.4.6.2}
DIR=${1:-$HOME/nvk/cts}
mkdir -p "$DIR"
if [ ! -d "$DIR/src/.git" ]; then
  git clone --depth 1 -b "$TAG" https://github.com/KhronosGroup/VK-GL-CTS.git "$DIR/src"
fi
cd "$DIR/src"
python3 external/fetch_sources.py
cmake -S . -B "$DIR/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSELECTED_BUILD_TARGETS=deqp-vk
ninja -C "$DIR/build" deqp-vk
echo "deqp-vk: $DIR/build/external/vulkancts/modules/vulkan/deqp-vk"
