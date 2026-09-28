// NVMTLGsp: GPU submission helper for NVMTLDriver (blits for now).
//
// For now blit work runs from the bundle itself: it opens its own NVGspControl user
// client, keeps one 2 MiB SYS staging object bound in its own arena, builds
// Ampere-CE (NV90B5 method set, proven by NVK/vkcopy on this GPU) method
// streams in staging, submits them with execSegments(engine 1) and waits
// with ceFenceWait. Results are copied back to the shared MTLBuffers' CPU
// mappings, so no kext submission path is needed for correctness.
//
// v2 (later): move submission into NVAccelerator's command queue behind a
// vendor kernel command, so GPU work orders with the Metal command buffer
// instead of running synchronously inside commit.
#import <Foundation/Foundation.h>

// Staging layout (one 2 MiB object, offsets chosen 64 KiB aligned).
#define NVGSP_STAGE_PATTERN 0x00000ULL   // 64 KiB: CPU-filled u32 pattern
#define NVGSP_STAGE_DATA    0x10000ULL   // 1 MiB: CE fill/copy staging
#define NVGSP_STAGE_DATA_MAX (1 << 20)
#define NVGSP_STAGE_PUSH    0x180000ULL  // 128 KiB: CE method streams
#define NVGSP_STAGE_PUSH_MAX (128 << 10)
#define NVGSP_GR_CODE       0x1C0000ULL  // 64 KiB: compute SASS
#define NVGSP_GR_CB0        0x1D0000ULL  // 4 KiB: cbuf 0 (push constants)
#define NVGSP_GR_QMD        0x1E0000ULL  // 4 KiB: QMD
#define NVGSP_GR_SEM        0x1E0800ULL  // 16 B: 3D end-of-pass semaphore (ROP flush)
#define NVGSP_GR_PUSH       0x1F0000ULL  // 64 KiB: GR method stream
#define NVGSP_GR_VBUF       0x110000ULL  // 256 KiB: vertex buffer staging
#define NVGSP_GR_VBUF_MAX   (256 << 10)
#define NVGSP_GR_VSOUT      0x150000ULL  // 64 KiB: VS clip positions (<=4096 verts)

// Copy methods go to subchannel 4 with NO SET_OBJECT, byte-identical in
// structure to NVK's proven CE pushes. Do NOT touch subch 0: the kext's
// fence tail emits there, and rebinding it wedges the CE ring for every
// client until reboot (live 27 Sep).
#define NVGSP_CE_SUBCH 4

// Open (once) the NVGspControl user client + staging. Returns false on
// failure; all other calls then fail too.
bool nvGspEnsure(void);

// CE copy, both VAs in our arena, any size (chunked at 128 KiB like NVK).
// Synchronous: returns true once the fence completes.
bool nvCeCopy(uint64_t dstVa, uint64_t srcVa, uint64_t bytes);

// Fill [dstVa, dstVa+bytes) with the u32 value (via the pattern page).
bool nvCeFill(uint64_t dstVa, uint64_t bytes, uint32_t value);

// GPU heap: MTLBuffers are page-aligned process memory that the kext wires
// and maps into our GPU VA (NVGspControl >= , selector 33), so compute
// and blits use their VA with no staging copies.
bool nvHeapAlloc(uint64_t bytes, void **cpu, uint64_t *va);
void nvHeapFree(void *cpu, uint64_t bytes);
// VA for [cpu, cpu+bytes) if it lies inside the heap, else 0.
uint64_t nvHeapVa(const void *cpu, uint64_t bytes);
// map memory we did not allocate (an IOSurface's pages) the same way,
// zero-copy; page-aligned start. Unwrap unbinds and unwires, never frees.
bool nvHeapWrap(void *cpu, uint64_t bytes, uint64_t *va);
void nvHeapUnwrap(void *cpu, uint64_t va);
// VRAM for MTLStorageModePrivate buffers: 64 MiB VRAM objects bound with big
// pages at 0x2A00000000+, suballocated 64 KiB aligned (first fit). No CPU view.
bool nvVramAlloc(uint64_t bytes, uint64_t *va);
void nvVramFree(uint64_t va, uint64_t bytes);
// Staging accessors (valid after nvGspEnsure).
uint8_t *nvStageCpu(void);   // CPU mapping of the 2 MiB object
uint64_t nvStageVa(void);    // its arena VA

// GR compute launch (nvrun's nak_launch, via execSegments(0) + fence 18).
// slm = scratch bytes per lane (0 none).
// code/push copied into staging; grid = CTA counts, block = CTA dims
// (runtime dispatch sizes). Synchronous: true once the fence completes.
bool nvGrLaunch(const uint32_t *code, uint32_t codeWords, uint32_t regs, uint32_t slm,
                uint32_t smem, uint32_t barriers,
                const uint32_t *push, uint32_t npush,
                const uint32_t grid[3], const uint32_t block[3]);

// one GPU submitter per process (recursive). nvExecuteOps holds it
// for a whole command buffer.
void nvGpuLock(void);
void nvGpuUnlock(void);
// batched compute. Between Begin/End (takes the GPU lock) nvGrQueue
// only appends the launch; nvGrFlush (and End, and every other submitting
// call) runs what is queued, in order, and waits. Outside a batch nvGrQueue
// is nvGrLaunch. Flush before the CPU looks at memory a queued launch writes.
void nvGrBatchBegin(void);
void nvGrBatchEnd(void);
bool nvGrFlush(void);
// native N1 test (NVMTL_NATIVE=1): queued launches as a family kernel command
bool nvGrFlushNative(id cb);
// native N2: asynchronous commit
void nvGrBatchBeginNative(void);
void nvGrBatchEndNative(void);
void nvNativeDrain(void);
bool nvNativeEnabled(void);
bool nvBatchOn(void);
void nvGrLabel(const char *what);
void nvGrNoteTextures(const char *what);
const char *nvVaState(uint64_t va);         // live / freed / unmapped   // 0.8.12: textures of the next launch (fault dump)   // 0.8.7: name the next queued launch
void nvGrSync(void);          // nvGrFlush + nvNativeDrain: before CPU access or other engines
bool nvGrQueue(const uint32_t *code, uint32_t codeWords, uint32_t regs, uint32_t slm,
               uint32_t smem, uint32_t barriers,
               const uint32_t *push, uint32_t npush,
               const uint32_t grid[3], const uint32_t block[3]);

// Metal render pipelines on the ADA_A (0xC997) 3D class. One draw is
// one self-contained method stream (targets, programs, bindings, state,
// draw), so whatever NVK or another process left on the shared channel does
// not matter. Shader addresses point at the 0x80-byte SPH in front of the code.
typedef struct {
    uint64_t va;            // SPH + code
    uint32_t gprs;
    uint32_t slm;           // scratch bytes per lane
    const uint32_t *push;   // this stage's constant buffer (cbuf 0)
    uint32_t pushWords;
} NV3DStage;

typedef struct {
    uint64_t va;
    uint32_t pitch, w, h, format;   // SET_COLOR_TARGET_FORMAT value
    bool clear;
    float clearColor[4];
    bool blend;
    uint32_t colorOp, colorSrc, colorDst, alphaOp, alphaSrc, alphaDst;   // OGL enums
    uint32_t writeMask;             // CT_WRITE bits: R 1<<0, G 1<<4, B 1<<8, A 1<<12
    bool blockLinear;               // WIDTH is pixels, MEMORY holds the block height
    uint32_t blockHeightLog2;
    // mip level / slice targets. depth > 1 with depthIsZ picks plane
    // `layer` of a 3D level; otherwise layer indexes arrayPitch-spaced layers
    uint32_t blockDepthLog2, depth, layer;
    uint64_t arrayPitch;
    bool depthIsZ;
    uint32_t sx, sy;                // samples per pixel in x / y (MSAA), 0 = 1
} NV3DTarget;

typedef struct {
    bool used;
    uint32_t stream, offset, widths, type;   // SET_VERTEX_ATTRIBUTE_A fields
    bool swapRB;
} NV3DAttr;

typedef struct {
    bool used;
    uint64_t va, size;
    uint32_t stride, divisor;       // divisor 0 = per vertex
} NV3DStream;

typedef struct {
    NV3DTarget rt[8];
    uint32_t nrt;
    double vp[6];                   // x, y, w, h, znear, zfar (MTLViewport)
    uint32_t sc[4];                 // x, y, w, h
    NV3DStage vs, fs;
    // tessellation (tes.va != 0): generated TCS, the app's
    // post-tessellation vertex function as TES, vs = control point fetch
    NV3DStage tcs, tes;
    uint32_t patchCps, tessParams;
    NV3DAttr attr[32];
    NV3DStream stream[32];
    uint32_t topology;              // BEGIN_OP
    bool indexed;
    uint64_t indexVa, indexBytes;
    uint32_t indexSize;             // 1 = 16-bit, 2 = 32-bit (SET_INDEX_BUFFER_E)
    uint32_t first, count;          // first vertex / first index, count
    uint32_t instances, baseInstance;
    int32_t baseVertex;
    uint32_t cull;                  // 0 none, 1 front, 2 back
    bool frontCCW;
    bool draw;                      // false: only the clears (empty pass)
    // depth (block-linear zeta surface, 0.5.1)
    uint64_t zVa;                   // 0 = no depth target
    uint32_t zWidthEl, zHeight, zBlockHeightLog2, zFormat;
    bool zClear, zTest, zWrite;
    float zClearValue;
    uint32_t zFunc;                 // OGL 0x200 + MTLCompareFunction
    uint32_t zBpp;                  // bytes per zeta element (0 = 4)
    uint32_t aaMode;                // SET_ANTI_ALIAS samples mode, 0 = 1X1
    // stencil (OGL enums); [0] front, [1] back
    bool sTest, sClear;
    uint32_t sClearValue;
    uint32_t sFail[2], sZFail[2], sZPass[2], sFunc[2], sRef[2], sReadMask[2], sWriteMask[2];
} NV3DDraw;

// block-linear VRAM surfaces (their own objects, PTE kind given,
// e.g. 6 = GENERIC_MEMORY for depth32 and block-linear colour)
bool nvVramAllocKind(uint64_t bytes, uint32_t kind, uint64_t *va, uint32_t *handle);
void nvVramFreeKind(uint32_t handle);

bool nvGr3DDraw(const NV3DDraw *d);

// 2D copy between a pitch-linear surface and a block-linear one
// (1 GOB wide, 2^bh GOBs high) on the copy engine. toBL picks the direction.
bool nvCeCopy2DBL(bool toBL, uint64_t pitchVa, uint32_t pitch, uint64_t blVa, uint32_t blWidthBytes,
                  uint32_t bh, uint32_t widthBytes, uint32_t height);

// region copy between a linear buffer and one level of a block-linear
// texture (blBase = that level in that layer; ylog/zlog its block shape; x in
// bytes; z the depth slice for 3D)
// one side of a copy-engine copy. Linear: va is the first byte,
// pitch the row stride. Block linear: va is the level/slice base, with the
// level's row bytes / rows / depth / block log2s and the origin inside it.
typedef struct {
    uint64_t va;
    bool bl;
    uint32_t pitch;
    uint32_t rowBytes, rows, depth, ylog, zlog;
    uint32_t xBytes, y, z;
} NVCeSurf;
// any combination of linear and block linear (one depth slice)
bool nvCeCopySurf(const NVCeSurf *src, const NVCeSurf *dst, uint32_t widthBytes, uint32_t height);
bool nvCeCopyBLRegion(bool toBL, uint64_t linVa, uint32_t linPitch, uint64_t blBase, uint32_t blRowBytes,
                      uint32_t blRows, uint32_t blDepth, uint32_t ylog, uint32_t zlog,
                      uint32_t xBytes, uint32_t y, uint32_t z, uint32_t widthBytes, uint32_t height);
