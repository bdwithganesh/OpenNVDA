#include "../../NVGspCore/NVGspWindowSurface.hpp"
#include <cassert>
#include <initializer_list>
#include <cstdio>

int main() {
    // Desktop surface (GOP / WindowServer) and a tightly packed NVK image.
    assert(nvgsp::windowSurfaceValid(16384, 3840, 2160, 0xCF, 3840, 2160));
    assert(nvgsp::windowSurfaceValid(15360, 3840, 2160, 0xCF, 3840, 2160));
    // Smaller surfaces scan out top-left.
    assert(nvgsp::windowSurfaceValid(2560, 640, 480, 0xD5, 3840, 2160));
    // Pitch: 64-byte multiple, holds the row, fits 13 bits of 64-byte units.
    assert(!nvgsp::windowSurfaceValid(15392, 3840, 2160, 0xCF, 3840, 2160));
    assert(!nvgsp::windowSurfaceValid(15296, 3840, 2160, 0xCF, 3840, 2160));
    assert(nvgsp::windowSurfaceValid(0x1fff * 64, 3840, 2160, 0xCF, 3840, 2160));
    assert(!nvgsp::windowSurfaceValid(0x2000 * 64, 3840, 2160, 0xCF, 3840, 2160));
    // Size bounds and formats.
    assert(!nvgsp::windowSurfaceValid(16384, 3841, 2160, 0xCF, 3840, 2160));
    assert(!nvgsp::windowSurfaceValid(16384, 3840, 2161, 0xCF, 3840, 2160));
    assert(!nvgsp::windowSurfaceValid(16384, 0, 2160, 0xCF, 3840, 2160));
    assert(!nvgsp::windowSurfaceValid(16384, 3840, 0, 0xCF, 3840, 2160));
    assert(!nvgsp::windowSurfaceValid(16384, 3840, 2160, 0xC6, 3840, 2160));  // FP16: 8 B/px
    for (uint32_t f : {0xCFu, 0xE6u, 0xD5u, 0xF9u, 0xD1u}) assert(nvgsp::windowFormatValid(f));
    std::puts("nvgsp_window_surface_check: PASS");
    return 0;
}
