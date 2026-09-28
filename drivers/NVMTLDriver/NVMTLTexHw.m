#import "NVMTLTexHw.h"
#import "NVMTLGsp.h"
#include <os/lock.h>
#include <string.h>
#include <stdlib.h>

// Pool: 16384 texture headers then 4096 samplers, 32 bytes each, in one
// zero-copy host allocation (the CPU writes descriptors, the GPU reads them
// through its header / sampler caches, invalidated at every launch).
enum { kTicCount = 16384, kTscCount = 4096, kDescBytes = 32 };
static uint8_t *gPoolCpu;
static uint64_t gPoolVa;
static uint32_t gTicUsed[kTicCount / 32], gTscUsed[kTscCount / 32];
static os_unfair_lock gPoolLock = OS_UNFAIR_LOCK_INIT;

static bool ticBuildDesc(MTLPixelFormat fmt, uint64_t va, uint32_t pitch, uint32_t w, uint32_t h, uint32_t d[8]);
static bool poolEnsure(void) {
    if (gPoolCpu) return true;
    void *cpu = NULL;
    uint64_t va = 0;
    const uint64_t bytes = (uint64_t)(kTicCount + kTscCount) * kDescBytes;
    if (!nvGspEnsure() || !nvHeapAlloc(bytes + 4096, &cpu, &va)) return false;
    memset(cpu, 0, bytes + 4096);
    gTicUsed[0] = gTscUsed[0] = 1;              // index 0 is never handed out
    // and it is a real 1x1 texture on a page of zeros: a shader that
    // samples an unbound texture (resource id 0) read VA 0 before (MMU fault)
    uint32_t d0[8];
    if (ticBuildDesc(MTLPixelFormatRGBA8Unorm, va + bytes, 256, 1, 1, d0)) memcpy(cpu, d0, kDescBytes);
    gPoolVa = va;
    gPoolCpu = cpu;
    return true;
}

bool nvTexPool(uint64_t *ticVa, uint32_t *ticMax, uint64_t *tscVa, uint32_t *tscMax) {
    if (!gPoolCpu) return false;
    *ticVa = gPoolVa;
    *ticMax = kTicCount - 1;
    *tscVa = gPoolVa + (uint64_t)kTicCount * kDescBytes;
    *tscMax = kTscCount - 1;
    return true;
}

static uint32_t take(uint32_t *used, uint32_t n) {
    for (uint32_t w = 0; w < n / 32; ++w) {
        if (used[w] == ~0u) continue;
        const uint32_t b = (uint32_t)__builtin_ctz(~used[w]);
        used[w] |= 1u << b;
        return w * 32 + b;
    }
    return 0;
}

static void put(uint32_t *d, uint32_t lo, uint32_t hi, uint64_t v) {
    for (uint32_t bit = lo; bit <= hi; ++bit, v >>= 1)
        if (v & 1) d[bit / 32] |= 1u << (bit % 32);
        else d[bit / 32] &= ~(1u << (bit % 32));
}

// Metal format -> TIC components, data type, swizzle (source per X Y Z W:
// 0 zero, 2 R, 3 G, 4 B, 5 A, 6 one int, 7 one float), sRGB
typedef struct { MTLPixelFormat f; uint8_t comp, type, sx, sy, sz, sw, srgb, swap; } TicFmt;
enum { T_SNORM = 1, T_UNORM = 2, T_SINT = 3, T_UINT = 4, T_FLOAT = 7 };
static const TicFmt kFmts[] = {
    {MTLPixelFormatR8Unorm,          0x1d, T_UNORM, 2, 0, 0, 7, 0, 0},
    {MTLPixelFormatR8Snorm,          0x1d, T_SNORM, 2, 0, 0, 7, 0, 0},
    {MTLPixelFormatR8Uint,           0x1d, T_UINT,  2, 0, 0, 6, 0, 0},
    {MTLPixelFormatR8Sint,           0x1d, T_SINT,  2, 0, 0, 6, 0, 0},
    {MTLPixelFormatA8Unorm,          0x1d, T_UNORM, 0, 0, 0, 2, 0, 0},
    {MTLPixelFormatRG8Unorm,         0x18, T_UNORM, 2, 3, 0, 7, 0, 0},
    {MTLPixelFormatRG8Snorm,         0x18, T_SNORM, 2, 3, 0, 7, 0, 0},
    {MTLPixelFormatRG8Uint,          0x18, T_UINT,  2, 3, 0, 6, 0, 0},
    {MTLPixelFormatR16Unorm,         0x1b, T_UNORM, 2, 0, 0, 7, 0, 0},
    {MTLPixelFormatR16Float,         0x1b, T_FLOAT, 2, 0, 0, 7, 0, 0},
    {MTLPixelFormatR16Uint,          0x1b, T_UINT,  2, 0, 0, 6, 0, 0},
    {MTLPixelFormatR16Sint,          0x1b, T_SINT,  2, 0, 0, 6, 0, 0},
    {MTLPixelFormatRGBA8Unorm,       0x08, T_UNORM, 2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRGBA8Unorm_sRGB,  0x08, T_UNORM, 2, 3, 4, 5, 1, 0},
    {MTLPixelFormatRGBA8Snorm,       0x08, T_SNORM, 2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRGBA8Uint,        0x08, T_UINT,  2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRGBA8Sint,        0x08, T_SINT,  2, 3, 4, 5, 0, 0},
    {MTLPixelFormatBGRA8Unorm,       0x08, T_UNORM, 4, 3, 2, 5, 0, 1},
    {MTLPixelFormatBGRA8Unorm_sRGB,  0x08, T_UNORM, 4, 3, 2, 5, 1, 1},
    {MTLPixelFormatRGB10A2Unorm,     0x09, T_UNORM, 2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRG11B10Float,     0x21, T_FLOAT, 2, 3, 4, 7, 0, 0},
    {MTLPixelFormatRG16Unorm,        0x0c, T_UNORM, 2, 3, 0, 7, 0, 0},
    {MTLPixelFormatRG16Float,        0x0c, T_FLOAT, 2, 3, 0, 7, 0, 0},
    {MTLPixelFormatRG16Uint,         0x0c, T_UINT,  2, 3, 0, 6, 0, 0},
    {MTLPixelFormatR32Float,         0x0f, T_FLOAT, 2, 0, 0, 7, 0, 0},
    {MTLPixelFormatR32Uint,          0x0f, T_UINT,  2, 0, 0, 6, 0, 0},
    {MTLPixelFormatR32Sint,          0x0f, T_SINT,  2, 0, 0, 6, 0, 0},
    {MTLPixelFormatRGBA16Unorm,      0x03, T_UNORM, 2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRGBA16Float,      0x03, T_FLOAT, 2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRGBA16Uint,       0x03, T_UINT,  2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRGBA16Sint,       0x03, T_SINT,  2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRG32Float,        0x04, T_FLOAT, 2, 3, 0, 7, 0, 0},
    {MTLPixelFormatRG32Uint,         0x04, T_UINT,  2, 3, 0, 6, 0, 0},
    {MTLPixelFormatRGBA32Float,      0x01, T_FLOAT, 2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRGBA32Uint,       0x01, T_UINT,  2, 3, 4, 5, 0, 0},
    {MTLPixelFormatRGBA32Sint,       0x01, T_SINT,  2, 3, 4, 5, 0, 0},
};

static const TicFmt *ticBuild(MTLPixelFormat fmt, uint64_t va, uint32_t pitch, uint32_t w, uint32_t h,
                              uint32_t d[8]);
static bool ticBuildDesc(MTLPixelFormat fmt, uint64_t va, uint32_t pitch, uint32_t w, uint32_t h, uint32_t d[8]) {
    return ticBuild(fmt, va, pitch, w, h, d) != NULL;
}
uint32_t nvTicAlloc(MTLPixelFormat fmt, uint64_t va, uint32_t pitch, uint32_t w, uint32_t h,
                    bool *swapRB) {
    uint32_t d[8];
    const TicFmt *f = ticBuild(fmt, va, pitch, w, h, d);
    if (!f) return 0;
    os_unfair_lock_lock(&gPoolLock);
    const bool ok = poolEnsure();
    const uint32_t idx = ok ? take(gTicUsed, kTicCount) : 0;
    if (idx) memcpy(gPoolCpu + (size_t)idx * kDescBytes, d, kDescBytes);
    os_unfair_lock_unlock(&gPoolLock);
    if (swapRB) *swapRB = f->swap;
    return idx;
}

static const TicFmt *ticBuild(MTLPixelFormat fmt, uint64_t va, uint32_t pitch, uint32_t w, uint32_t h,
                              uint32_t d[8]) {
    const TicFmt *f = NULL;
    for (size_t i = 0; i < sizeof kFmts / sizeof kFmts[0]; ++i)
        if (kFmts[i].f == fmt) { f = &kFmts[i]; break; }
    if (!f || !w || !h || (va & 31) || (pitch & 31) || pitch >= (1u << 21) || w > 65536 || h > 131072 ||
        (va >> 48))
        return NULL;
    memset(d, 0, 32);
    put(d, 0, 6, f->comp);
    put(d, 7, 9, f->type); put(d, 10, 12, f->type); put(d, 13, 15, f->type); put(d, 16, 18, f->type);
    put(d, 19, 21, f->sx); put(d, 22, 24, f->sy); put(d, 25, 27, f->sz); put(d, 28, 30, f->sw);
    put(d, 37, 63, (va >> 5) & 0x7ffffff);          // ADDRESS_BITS31TO5
    put(d, 64, 79, (va >> 32) & 0xffff);            // ADDRESS_BITS47TO32
    put(d, 85, 87, 2);                              // HEADER_VERSION SELECT_PITCH
    put(d, 96, 111, pitch >> 5);                    // PITCH_BITS20TO5
    put(d, 112, 112, 1);                            // LOD_ANISO_QUALITY2
    put(d, 113, 113, 1);                            // LOD_ANISO_QUALITY high
    put(d, 114, 114, 1);                            // LOD_ISO_QUALITY high
    put(d, 128, 143, w - 1);                        // WIDTH_MINUS_ONE
    put(d, 150, 150, f->srgb);
    put(d, 151, 154, 7);                            // TEXTURE_TYPE TWO_D_NO_MIPMAP
    put(d, 157, 159, 7);                            // BORDER_SIZE BORDER_SAMPLER_COLOR
    put(d, 160, 175, (h - 1) & 0xffff);             // HEIGHT_MINUS_ONE
    put(d, 146, 146, ((h - 1) >> 16) & 1);          // HEIGHT_MINUS_ONE_BIT16 (Pascal+)
    put(d, 191, 191, 1);                            // NORMALIZED_COORDS (samplers may force off)
    put(d, 215, 216, 2);                            // ANISO_FINE_SPREAD_FUNC TWO
    put(d, 217, 218, 1);                            // ANISO_COARSE_SPREAD_FUNC ONE
    return f;
}

// a header as the GPU sees it, for fault dumps
bool nvTicRead(uint32_t index, uint32_t d[8]) {
    if (!gPoolCpu || index >= kTicCount) return false;
    memcpy(d, gPoolCpu + (size_t)index * kDescBytes, kDescBytes);
    return true;
}

void nvTicFree(uint32_t index) {
    if (!index || index >= kTicCount) return;
    os_unfair_lock_lock(&gPoolLock);
    gTicUsed[index / 32] &= ~(1u << (index % 32));
    os_unfair_lock_unlock(&gPoolLock);
}

// LOD clamps are unsigned 4.8 fixed point
static uint32_t ufix48(float v) {
    if (!(v > 0)) return 0;
    if (v > 15.99f) v = 15.99f;
    return (uint32_t)(v * 256.0f + 0.5f);
}

uint32_t nvTscAllocRaw(uint32_t minF, uint32_t magF, uint32_t mipF, uint32_t addrU, uint32_t addrV,
                       uint32_t addrW, bool normalized, float lodMin, float lodMax,
                       uint32_t compare, uint32_t border) {
    uint32_t d[8] = {0};
    put(d, 0, 2, addrU); put(d, 3, 5, addrV); put(d, 6, 8, addrW);
    if (compare) { put(d, 9, 9, 1); put(d, 10, 12, compare - 1); }
    put(d, 32 + 0, 32 + 2, magF);
    put(d, 32 + 4, 32 + 5, minF);
    put(d, 32 + 6, 32 + 7, mipF);
    put(d, 32 + 8, 32 + 9, 2);                      // CUBEMAP_INTERFACE_FILTERING AUTO_SPAN_SEAM
    put(d, 32 + 25, 32 + 25, normalized ? 0 : 1);   // FLOAT_COORD_NORMALIZATION
    put(d, 64 + 0, 64 + 11, ufix48(lodMin));
    put(d, 64 + 12, 64 + 23, ufix48(lodMax));
    // border: 0 transparent black, 1 opaque black, 2 opaque white (Metal order)
    const uint32_t one = 0x3f800000u;
    d[4] = border == 2 ? one : 0;
    d[5] = border == 2 ? one : 0;
    d[6] = border == 2 ? one : 0;
    d[7] = border ? one : 0;
    os_unfair_lock_lock(&gPoolLock);
    const bool ok = poolEnsure();
    const uint32_t idx = ok ? take(gTscUsed, kTscCount) : 0;
    if (idx) memcpy(gPoolCpu + (size_t)(kTicCount + idx) * kDescBytes, d, kDescBytes);
    os_unfair_lock_unlock(&gPoolLock);
    return idx;
}

static uint32_t addrMode(MTLSamplerAddressMode m) {
    switch (m) {
    case MTLSamplerAddressModeRepeat: return 0;                 // WRAP
    case MTLSamplerAddressModeMirrorRepeat: return 1;           // MIRROR
    case MTLSamplerAddressModeMirrorClampToEdge: return 5;      // MIRROR_ONCE_CLAMP_TO_EDGE
    case MTLSamplerAddressModeClampToZero:
    case MTLSamplerAddressModeClampToBorderColor: return 3;     // BORDER
    default: return 2;                                          // CLAMP_TO_EDGE
    }
}

uint32_t nvTscAlloc(MTLSamplerDescriptor *sd) {
    const uint32_t minF = sd.minFilter == MTLSamplerMinMagFilterLinear ? 2 : 1;
    const uint32_t magF = sd.magFilter == MTLSamplerMinMagFilterLinear ? 2 : 1;
    const uint32_t mipF = sd.mipFilter == MTLSamplerMipFilterLinear ? 3
                        : sd.mipFilter == MTLSamplerMipFilterNearest ? 2 : 1;
    uint32_t border = 0;
    if (sd.sAddressMode == MTLSamplerAddressModeClampToBorderColor ||
        sd.tAddressMode == MTLSamplerAddressModeClampToBorderColor)
        border = sd.borderColor == MTLSamplerBorderColorOpaqueWhite ? 2
               : sd.borderColor == MTLSamplerBorderColorOpaqueBlack ? 1 : 0;
    const uint32_t cmp = sd.compareFunction != MTLCompareFunctionNever ? (uint32_t)sd.compareFunction + 1 : 0;
    return nvTscAllocRaw(minF, magF, mipF, addrMode(sd.sAddressMode), addrMode(sd.tAddressMode),
                         addrMode(sd.rAddressMode), sd.normalizedCoordinates, sd.lodMinClamp,
                         sd.lodMaxClamp, cmp, border);
}

void nvTscFree(uint32_t index) {
    if (!index || index >= kTscCount) return;
    os_unfair_lock_lock(&gPoolLock);
    gTscUsed[index / 32] &= ~(1u << (index % 32));
    os_unfair_lock_unlock(&gPoolLock);
}

// AIR constexpr sampler word (reverse-engineered from the Metal compiler's
// output, 28 Sep): bits 0-2 / 3-5 / 6-8 s / t / r address (0 zero or border,
// 1 clamp to edge, 2 repeat, 3 mirrored repeat, 4 mirrored clamp to edge),
// 9 mag linear, 11 min linear, 13-14 mip (0 none, 1 nearest, 2 linear),
// 15 pixel coordinates, 16-19 compare (8 none; 0 never 1 less 2 less_equal
// 3 greater 4 greater_equal 5 equal 6 not_equal 7 always), 20-23 max
// anisotropy - 1, 24-39 / 40-55 lod min / max as half floats, 56-57 border
// color (0 transparent black, 1 opaque black, 2 opaque white).
static float half2f(uint32_t h) {
    const uint32_t e = (h >> 10) & 31, m = h & 1023;
    float v = e ? (1.0f + m / 1024.0f) * (float)(1u << e) / 32768.0f : m / 16777216.0f;
    return (h & 0x8000) ? -v : v;
}

uint32_t nvTscFromAirBits(uint64_t bits) {
    static const uint32_t addr[8] = {3, 2, 0, 1, 5, 2, 2, 2};
    static const uint32_t cmp[9] = {0 + 1, 1 + 1, 3 + 1, 4 + 1, 6 + 1, 2 + 1, 5 + 1, 7 + 1, 0};
    const uint32_t mip = (bits >> 13) & 3;
    const uint32_t c = (bits >> 16) & 15;
    return nvTscAllocRaw((bits >> 11) & 1 ? 2 : 1, (bits >> 9) & 1 ? 2 : 1,
                         mip == 2 ? 3 : mip == 1 ? 2 : 1,
                         addr[bits & 7], addr[(bits >> 3) & 7], addr[(bits >> 6) & 7],
                         !((bits >> 15) & 1), half2f((uint32_t)(bits >> 24) & 0xffff),
                         half2f((uint32_t)(bits >> 40) & 0xffff), c < 9 ? cmp[c] : 0,
                         (uint32_t)(bits >> 56) & 3);
}

// ---------------------------------------------------------------- 0.5.2
static uint32_t ilog2ceil(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }

void nvTexLayoutInit(NVTexLayout *l, uint32_t w, uint32_t h, uint32_t d, uint32_t layers,
                     uint32_t levels, uint32_t bpp, bool is3D) {
    memset(l, 0, sizeof(*l));
    l->w = w; l->h = h; l->d = is3D ? d : 1; l->layers = layers ? layers : 1;
    l->levels = levels ? (levels > 16 ? 16 : levels) : 1; l->bpp = bpp;
    uint32_t y = 5, z = is3D ? (getenv("NVMTL_Z0") ? (uint32_t)atoi(getenv("NVMTL_Z0")) : 5) : 0;
    // level 0 clamp
    const uint32_t hg0 = (h + 7) / 8;
    if (ilog2ceil(hg0) < y) y = ilog2ceil(hg0);
    if (ilog2ceil(l->d) < z) z = ilog2ceil(l->d);
    l->y0 = y; l->z0 = z;
    uint64_t off = 0;
    for (uint32_t i = 0; i < l->levels; i++) {
        const uint32_t lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1, ld = l->d >> i ? l->d >> i : 1;
        uint32_t ly = y, lz = z;
        if (ilog2ceil((lh + 7) / 8) < ly) ly = ilog2ceil((lh + 7) / 8);
        if (ilog2ceil(ld) < lz) lz = ilog2ceil(ld);
        const uint32_t rb = (lw * bpp + 63) & ~63u;
        const uint32_t tr = 8u << ly, td = 1u << lz;
        const uint32_t rows = (lh + tr - 1) / tr * tr, dep = (ld + td - 1) / td * td;
        l->offset[i] = off; l->rowBytes[i] = rb; l->rows[i] = rows; l->depth[i] = dep;
        l->ylog[i] = ly; l->zlog[i] = lz;
        off += (uint64_t)rb * rows * dep;
    }
    const uint64_t tile0 = 64ull * (8u << y) * (1u << z);
    l->arrayStride = (off + tile0 - 1) / tile0 * tile0;
    l->size = l->arrayStride * l->layers;
}

uint32_t nvTicAllocBL(MTLPixelFormat fmt, uint64_t va, uint32_t type, const NVTexLayout *l, bool *swapRB) {
    const TicFmt *f = NULL;
    for (size_t i = 0; i < sizeof kFmts / sizeof kFmts[0]; ++i)
        if (kFmts[i].f == fmt) { f = &kFmts[i]; break; }
    if (!f || (va & 511) || (va >> 48)) return 0;
    uint32_t d[8] = {0};
    put(d, 0, 6, f->comp);
    put(d, 7, 9, f->type); put(d, 10, 12, f->type); put(d, 13, 15, f->type); put(d, 16, 18, f->type);
    put(d, 19, 21, f->sx); put(d, 22, 24, f->sy); put(d, 25, 27, f->sz); put(d, 28, 30, f->sw);
    put(d, 41, 63, (va >> 9) & 0x7fffff);           // ADDRESS_BITS31TO9
    put(d, 64, 79, (va >> 32) & 0xffff);            // ADDRESS_BITS47TO32
    put(d, 85, 87, 3);                              // HEADER_VERSION SELECT_BLOCKLINEAR
    put(d, 96, 98, 0);                              // GOBS_PER_BLOCK_WIDTH one
    put(d, 99, 101, l->y0);                         // GOBS_PER_BLOCK_HEIGHT
    put(d, 102, 104, l->z0);                        // GOBS_PER_BLOCK_DEPTH
    put(d, 106, 108, 0);                            // TILE_WIDTH_IN_GOBS
    put(d, 112, 112, 1); put(d, 113, 113, 1); put(d, 114, 114, 1);   // LOD quality
    put(d, 124, 127, l->levels - 1);                // MAX_MIP_LEVEL
    put(d, 128, 143, l->w - 1);                     // WIDTH_MINUS_ONE
    put(d, 150, 150, f->srgb);
    put(d, 151, 154, type);                         // TEXTURE_TYPE
    put(d, 157, 159, 7);                            // BORDER_SIZE sampler colour
    const uint32_t hm1 = (type == 0 || type == 4) ? 0 : l->h - 1;
    put(d, 160, 175, hm1 & 0xffff);
    put(d, 146, 146, (hm1 >> 16) & 1);
    // DEPTH_MINUS_ONE: depth for 3D, layers for arrays, cubes for cube arrays
    uint32_t dm1 = 0;
    if (type == 2) dm1 = l->d - 1;
    else if (type == 4 || type == 5) dm1 = l->layers - 1;
    else if (type == 8) dm1 = l->layers / 6 - 1;
    put(d, 176, 189, dm1 & 0x3fff);
    put(d, 145, 145, (dm1 >> 14) & 1);
    put(d, 191, 191, 1);                            // NORMALIZED_COORDS
    put(d, 215, 216, 2); put(d, 217, 218, 1);       // aniso spread
    put(d, 224, 227, 0);                            // RES_VIEW_MIN_MIP_LEVEL
    put(d, 228, 231, l->levels - 1);                // RES_VIEW_MAX_MIP_LEVEL
    os_unfair_lock_lock(&gPoolLock);
    const bool ok = poolEnsure();
    const uint32_t idx = ok ? take(gTicUsed, kTicCount) : 0;
    if (idx) memcpy(gPoolCpu + (size_t)idx * kDescBytes, d, kDescBytes);
    os_unfair_lock_unlock(&gPoolLock);
    if (swapRB) *swapRB = f->swap;
    return idx;
}

uint32_t nvFormatBytes(MTLPixelFormat fmt) {
    for (size_t i = 0; i < sizeof kFmts / sizeof kFmts[0]; ++i) {
        if (kFmts[i].f != fmt) continue;
        switch (kFmts[i].comp) {
        case 0x1d: return 1;
        case 0x18: case 0x1b: return 2;
        case 0x03: case 0x04: return 8;
        case 0x01: return 16;
        default: return 4;
        }
    }
    return 0;
}
