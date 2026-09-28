// Hardware texturing (28 Sep): texture headers (TIC) and samplers (TSC) in
// one pool the compute channel is pointed at before every launch. A sampled
// texture handle is tic | tsc << 20 and a storage image handle is tic, the
// encoding NVK and NAK use (nvk_descriptor_types.h). Field layouts from
// NVIDIA's clb097tex.h / clc097tex.h (MIT, open-gpu-doc).
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdint.h>

// Pool addresses for SET_TEX_HEADER_POOL / SET_TEX_SAMPLER_POOL. False
// until the first descriptor is made.
bool nvTexPool(uint64_t *ticVa, uint32_t *ticMax, uint64_t *tscVa, uint32_t *tscMax);
bool nvTicRead(uint32_t index, uint32_t d[8]);   // fault dumps

// A pitch-linear 2D texture header. 0 when the format has no hardware
// mapping yet or the address / pitch are not 32-byte aligned. *swapRB is set
// for formats the hardware stores in the other channel order (BGRA), which
// shader writes have to swizzle themselves.
uint32_t nvTicAlloc(MTLPixelFormat fmt, uint64_t va, uint32_t pitch, uint32_t w, uint32_t h,
                    bool *swapRB);
void nvTicFree(uint32_t index);

uint32_t nvTscAlloc(MTLSamplerDescriptor *sd);
// Sampler from AIR's constexpr sampler bits (decoded by the caller into the
// same fields a descriptor has).
uint32_t nvTscAllocRaw(uint32_t minF, uint32_t magF, uint32_t mipF, uint32_t addrU, uint32_t addrV,
                       uint32_t addrW, bool normalized, float lodMin, float lodMax,
                       uint32_t compare, uint32_t border);
void nvTscFree(uint32_t index);

// Sampler from the 64-bit sampler-state word of an AIR constexpr sampler.
uint32_t nvTscFromAirBits(uint64_t bits);

// block-linear textures (mipmaps, arrays, cube, 3D), laid out the way
// NIL does it for Turing+ colour GOBs (64 B x 8 rows): level 0 takes a block
// of 2^y GOBs high and 2^z deep (up to 32), later levels clamp it to their
// size; each level is aligned to its block; layers sit array_stride apart.
typedef struct {
    uint32_t levels, layers;           // layers: array length (cube: 6 x cubes)
    uint32_t w, h, d, bpp;
    uint32_t y0, z0;                   // level 0 block: 2^y0 GOBs high, 2^z0 deep
    uint64_t offset[16];               // level offsets inside a layer
    uint32_t rowBytes[16], rows[16], depth[16], ylog[16], zlog[16];
    uint64_t arrayStride, size;
} NVTexLayout;

void nvTexLayoutInit(NVTexLayout *l, uint32_t w, uint32_t h, uint32_t d, uint32_t layers,
                     uint32_t levels, uint32_t bpp, bool is3D);

// Texture header for a block-linear texture. type: TEXHEAD texture type
// (0 1D, 1 2D, 2 3D, 3 cube, 4 1D array, 5 2D array, 8 cube array).
uint32_t nvTicAllocBL(MTLPixelFormat fmt, uint64_t va, uint32_t type, const NVTexLayout *l, bool *swapRB);

// bytes per pixel of a format the header table knows, 0 otherwise
uint32_t nvFormatBytes(MTLPixelFormat fmt);
