#pragma once

#include <stdint.h>

namespace nvgsp {

// 0.110.0: window-channel (C67E) surface formats accepted for scan-out
// (clc67e.h NVC67E_SET_PARAMS_FORMAT_*). All are 32 bits per pixel.
constexpr uint32_t kWndFormatA8R8G8B8 = 0xCF;     // VK B8G8R8A8
constexpr uint32_t kWndFormatX8R8G8B8 = 0xE6;
constexpr uint32_t kWndFormatA8B8G8R8 = 0xD5;     // VK R8G8B8A8
constexpr uint32_t kWndFormatX8B8G8R8 = 0xF9;
constexpr uint32_t kWndFormatA2B10G10R10 = 0xD1;  // VK A2B10G10R10

inline bool windowFormatValid(uint32_t format) {
    return format == kWndFormatA8R8G8B8 || format == kWndFormatX8R8G8B8 ||
           format == kWndFormatA8B8G8R8 || format == kWndFormatX8B8G8R8 ||
           format == kWndFormatA2B10G10R10;
}

// Pitch-linear surface for window 0: pitch in 64-byte units fits the 13-bit
// PLANAR_STORAGE_PITCH field, holds a full row, and the surface is not
// larger than the head's raster (SIZE_IN == SIZE_OUT, no scaler).
inline bool windowSurfaceValid(uint32_t pitch, uint32_t width, uint32_t height,
                               uint32_t format, uint32_t maxWidth, uint32_t maxHeight) {
    return windowFormatValid(format) && width && height && width <= maxWidth &&
           height <= maxHeight && !(pitch & 63) && (pitch >> 6) <= 0x1fff &&
           static_cast<uint64_t>(width) * 4 <= pitch;
}

}  // namespace nvgsp
