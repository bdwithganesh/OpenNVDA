// NVMTLCompiler: a small MetalSL-subset compute frontend that drives NAK.
//
// Subset: one or more `kernel void name(device [const] T *buf
// [[buffer(N)]], ..., uint tid [[thread_position_in_grid]]) { body }` with
// T in {float,int,uint}, scalar indexing only. Each kernel is translated to
// GLSL compute (push-constant u64 buffer addresses + buffer_reference, as
// nakc's ABI expects), compiled with glslangValidator, then nakc, and
// cached by kernel name. Anything outside the subset fails the pipeline
// with a log (never silently).
#import <Foundation/Foundation.h>

@interface NVMTLKernel : NSObject
@property (nonatomic) NSString *name;
@property (nonatomic) NSData *code;      // SASS words
@property (nonatomic) uint32_t regs;     // GPRs (QMD)
@property (nonatomic) uint32_t slm;      // scratch bytes per lane
@property (nonatomic) uint32_t smem;     // shared bytes needed
@property (nonatomic) uint32_t barriers; // barrier count
@property (nonatomic) uint32_t nbuf;     // buffer params (push = 2 dwords each)
@property (nonatomic) uint32_t ntex;     // texture params (2 + 4 dwords each, see compiler)
@property (nonatomic) uint32_t nsamp;    // sampler params (1 dword each)
@property (nonatomic) NSString *glsl;    // compute source, rebuilt per threadgroup shape
@property (nonatomic) NSMutableDictionary *variants;   // "x,y,z" -> NVMTLKernel
@property (nonatomic) BOOL isVertex;     // vertex shader: runtime appends out+vstart push
// Vertex output layout: vec4 slots per vertex (1 for a float4
// return, one per field for a struct return) and the [[position]] slot.
@property (nonatomic) uint32_t voutStride;
@property (nonatomic) uint32_t posSlot;
// 28 Sep: kernels from a precompiled metallib (AIR), compiled by nakc --air.
@property (nonatomic) NSString *airLib;   // metallib file in the cache dir
@property (nonatomic) NSArray<NSString *> *airArgs;   // extra nakc args (function constants)
@property (nonatomic) uint32_t stage;       // 0 compute, 1 vertex, 2 fragment, 3 post-tess vertex, 4 generated tcs
@property (nonatomic) uint32_t tessDomain, tessCps;   // post-tess vertex: 1 triangle / 2 quad, control points
@property (nonatomic) uint32_t tessParams;  // SET_TESSELLATION_PARAMETERS (domain | spacing << 4 | prims << 8)
@property (nonatomic) NSString *io;         // vertex: its varying list for the fragment stage
@property (nonatomic) uint64_t codeVa;      // vertex/fragment: SPH + code uploaded (0 = not yet)
@property (nonatomic) NSData *cdata;      // constant globals to upload
@property (nonatomic) uint32_t cdataDword;// push dword of the cdata pointer, 0 = none
@property (nonatomic) id cdataBuf;        // uploaded copy (MTLBuffer), made at first dispatch
@property (nonatomic) BOOL hwTex;         // textures/samplers as hardware handles (AIR kernels)
@property (nonatomic) uint32_t nconstSamp;// constexpr samplers after the nsamp argument slots
@property (nonatomic) NSData *constTsc;   // their sampler indices (uint32 each)
@end

// Fragment function. v1: constant color only (parsed). 0.3.0: `raster` is
// the fragment body fused into a per-triangle compute rasterizer (stage_in
// struct interpolated perspective-correct, textures/samplers/buffers like
// kernels, blending in the kernel); nil when the body is outside the subset.
@interface NVMTLFragment : NSObject
@property (nonatomic) NSString *name;
@property (nonatomic) NVMTLKernel *raster;
@property (nonatomic) BOOL hasColor;
- (void)nvSetColor:(const float *)c;
- (const float *)nvColor;
@end

// Compile every kernel in the MetalSL source; returns name -> NVMTLKernel.
// Empty (but non-nil) when nothing usable was found.
NSDictionary<NSString *, NVMTLKernel *> *nvCompileKernels(NSString *source);

// Parse every fragment function (constant-color subset).
NSDictionary<NSString *, NVMTLFragment *> *nvCompileFragments(NSString *source);

// Compile one GLSL compute source to a kernel (used for the fixed raster).
NVMTLKernel *nvCompileGLSL(NSString *name, NSString *glsl);

// The kernel built for a threadgroup shape (NIR folds gl_LocalInvocationID
// against the declared local size, so 2D/3D groups need their own build).
// Returns k itself for 256x1x1 or when k has no stored source.
// tessellation pipeline stages nakc generates: "vs" (control point
// attributes -> varyings) or "tcs" (pass-through + factors from the buffer)
NVMTLKernel *nvCompileTessGen(NSString *kind, NSArray<NSNumber *> *locs, uint32_t cps, uint32_t domain);
NVMTLKernel *nvKernelForBlock(NVMTLKernel *k, uint32_t x, uint32_t y, uint32_t z);

// 28 Sep: precompiled Metal libraries. nvSaveMetallib keeps the bytes in the
// cache dir (nakc reads a file) and returns that path; nvCompileAir builds one
// kernel of it for a threadgroup shape. nil when the kernel is outside what
// the AIR translator handles (the log says why).
NSString *nvSaveMetallib(NSData *lib);
NSString *nvSaveAir(NSData *bc);   // one function's bitcode
NVMTLKernel *nvCompileAir(NSString *libPath, NSString *name, uint32_t x, uint32_t y, uint32_t z,
                          NSArray<NSString *> *extra);
