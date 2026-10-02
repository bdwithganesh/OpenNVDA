// NVMTLRender: M6 v1 render path (B4). Vertex shaders run as compute
// launches (clip positions to a param area); a fixed GLSL rasterizer runs
// per pixel per triangle; results copy back to the texture's buffer.
// Fixed-function raster/ROP, MSAA, depth and blending are deferred (v1
// accepts both windings, ignores cull/frontFace, constant-color FS only).
#import <Foundation/Foundation.h>
#import "NVMTLLog.h"
#import <Metal/Metal.h>
#import "NVMTLCompiler.h"
#import "NVMTLGsp.h"

static NSString *kRasterGLSL(bool bgra) {
    NSString *pack = bgra ?
        @"  uvec3 bgr = uvec3(pc.color.bgr*255.0);\n"
        @"  uint px = bgr.r | (bgr.g<<8) | (bgr.b<<16) | (uint(pc.color.a*255.0)<<24);\n" :
        @"  uvec3 rgb = uvec3(pc.color.rgb*255.0);\n"
        @"  uint px = rgb.r | (rgb.g<<8) | (rgb.b<<16) | (uint(pc.color.a*255.0)<<24);\n";
    return [@"#version 460\n"
    @"#extension GL_EXT_buffer_reference : require\n"
    @"#extension GL_EXT_scalar_block_layout : require\n"
    @"#extension GL_EXT_shader_explicit_arithmetic_types : require\n"
    @"layout(push_constant, scalar) uniform PC {\n"
    @"  uint64_t rt;\n"
    @"  uint vW, vH, vStride;\n"
    @"  vec4 v0, v1, v2;\n"
    @"  vec4 vp;\n"
    @"  vec4 color;\n"
    @"  uvec4 sc;\n"
    @"} pc;\n"
    @"layout(buffer_reference, scalar) buffer Pixels { uint p[]; };\n"
    @"layout(local_size_x=256) in;\n"
    @"float edge(vec2 a, vec2 b, vec2 c) { return (c.x-a.x)*(b.y-a.y)-(c.y-a.y)*(b.x-a.x); }\n"
    @"void main() {\n"
    @"  uint i = gl_GlobalInvocationID.x;\n"
    @"  uint W = pc.vW, H = pc.vH;\n"
    @"  if (i >= W*H) return;\n"
    @"  uint x = i % W, y = i / W;\n"
    @"  if (x < pc.sc.x || y < pc.sc.y || x >= pc.sc.x+pc.sc.z || y >= pc.sc.y+pc.sc.w) return;\n"
    @"  float nx = ((float(x)+0.5-pc.vp.x)/pc.vp.z)*2.0-1.0;\n"
    @"  float ny = 1.0-((float(y)+0.5-pc.vp.y)/pc.vp.w)*2.0;\n"
    @"  vec2 p = vec2(nx, ny);\n"
    @"  vec2 a = pc.v0.xy, b = pc.v1.xy, c = pc.v2.xy;\n"
    @"  float e0 = edge(a,b,p), e1 = edge(b,c,p), e2 = edge(c,a,p);\n"
    @"  bool inside = (e0>=0.0 && e1>=0.0 && e2>=0.0) || (e0<=0.0 && e1<=0.0 && e2<=0.0);\n"
    @"  if (!inside) return;\n"
    stringByAppendingString:[pack stringByAppendingString:
    @"  Pixels RT = Pixels(pc.rt);\n"
    @"  RT.p[y*pc.vStride+x] = px;\n"
    @"}\n"]];
}

static NVMTLKernel *gRaster[2] = {nil, nil};

NVMTLKernel *nvRasterKernelForBGRA(bool bgra) {
    if (!gRaster[bgra]) gRaster[bgra] = nvCompileGLSL(bgra ? @"nv_raster_bgra" : @"nv_raster", kRasterGLSL(bgra));
    return gRaster[bgra];
}

// Push dwords for the raster (scalar layout: rt[0,1] W[2] H[3] stride[4]
// v0[5-8] v1[9-12] v2[13-16] vp[17-20] color[21-24] sc[25-28]).
static void rasterPush(uint32_t *push, uint64_t rt, uint32_t W, uint32_t H, uint32_t stride,
                       const float *v0, const float *v1, const float *v2,
                       const double *vp, const float *color, const NSUInteger *sc) {
    push[0] = (uint32_t)rt; push[1] = (uint32_t)(rt >> 32);
    push[2] = W; push[3] = H; push[4] = stride;
    memcpy(push + 5, v0, 16); memcpy(push + 9, v1, 16); memcpy(push + 13, v2, 16);
    float vf[4] = {(float)vp[0], (float)vp[1], (float)vp[2], (float)vp[3]};
    memcpy(push + 17, vf, 16);
    memcpy(push + 21, color, 16);
    push[25] = (uint32_t)sc[0]; push[26] = (uint32_t)sc[1];
    push[27] = (uint32_t)sc[2]; push[28] = (uint32_t)sc[3];
}

// Execute one draw (triangles) op. rt* describes the color attachment's
// staging (already holding loaded/cleared bytes on entry).
bool nvRenderDraw(NVMTLKernel *vs, const float fsColor[4],
                  NSArray *vbufs,               // per index: {buf, off} or NSNull
                  const double *vp, const NSUInteger *sc,
                  uint32_t rtW, uint32_t rtH, uint32_t rtStride,
                  NSUInteger vStart, NSUInteger vCount, bool bgra,
                  const uint32_t *idx, NSUInteger idxCount) {
    if (!vs || !vs.isVertex) { NSLog(@"NVMTLRender: no vertex kernel"); return false; }
    const NSUInteger tri = idx ? idxCount : vCount;
    if (!idx && idxCount) { NSLog(@"NVMTLRender: idxCount without idx"); return false; }
    if (tri % 3) { NSLog(@"NVMTLRender: index/vertex count not a multiple of 3"); return false; }
    if (!nvGspEnsure()) return false;
    NVMTLKernel *rast = nvRasterKernelForBGRA(bgra);
    if (!rast) return false;
    uint8_t *st = nvStageCpu();
    const uint64_t sv = nvStageVa();
    // 1. Vertex stage: transform vCount verts to clip float4s.
    uint64_t stageOff = 0;
    uint32_t push[64] = {0};
    const uint32_t nPush = vs.nbuf * 2 + 5; // bufs + out(u64) + vstart + vcount + iid
    if (nPush > 64) { NSLog(@"NVMTLRender: too many vertex bufs"); return false; }
    for (NSUInteger i = 0; i < vbufs.count && i < vs.nbuf; i++) {
        NSDictionary *be = vbufs[i];
        if ((id)be == [NSNull null]) continue;
        id<MTLBuffer> b = be[@"buf"];
        NSUInteger off = [be[@"off"] unsignedIntegerValue];
        uint8_t *c = b.contents;
        if (!c || off >= b.length) { NSLog(@"NVMTLRender: bad vertex buf"); return false; }
        NSUInteger len = b.length - off;
        if (stageOff + len > NVGSP_GR_VBUF_MAX)
            { NSLog(@"NVMTLRender: vertex staging overflow"); return false; }
        memcpy(st + NVGSP_GR_VBUF + stageOff, c + off, len);
        const uint64_t va = sv + NVGSP_GR_VBUF + stageOff;
        push[2 * i] = (uint32_t)va; push[2 * i + 1] = (uint32_t)(va >> 32);
        stageOff += (len + 63) & ~63ull;
    }
    const uint64_t outVa = sv + NVGSP_GR_VSOUT;
    push[vs.nbuf * 2] = (uint32_t)outVa; push[vs.nbuf * 2 + 1] = (uint32_t)(outVa >> 32);
    push[vs.nbuf * 2 + 2] = (uint32_t)vStart;
    push[vs.nbuf * 2 + 3] = (uint32_t)vCount;
    uint32_t grid[3] = {(uint32_t)((vCount + 255) / 256), 1, 1};
    uint32_t block[3] = {256, 1, 1};
    // Guard: VS writes OUT[gid] for gid < grid*block; over-launch writes
    // past vCount but inside the 64 KiB VSOUT for vCount <= 4093.
    if (vCount > 4093 || vCount == 0) { NSLog(@"NVMTLRender: vertexCount %lu out of v1 range", (unsigned long)vCount); return false; }
    const uint32_t *code = vs.code.bytes;
    if (!nvGrLaunch(code, (uint32_t)(vs.code.length / 4), vs.regs, vs.slm, vs.smem, vs.barriers,
                    push, nPush, grid, block)) {
        NSLog(@"NVMTLRender: VS launch failed"); return false;
    }
    // 2. Rasterize each triangle (clip verts read back on CPU).
    const float *clip = (const float *)(st + NVGSP_GR_VSOUT);
    const uint64_t rtVa = sv + NVGSP_STAGE_DATA;
    const uint32_t npix = rtW * rtH;
    uint32_t rgrid[3] = {(npix + 255) / 256, 1, 1};
    uint32_t rblock[3] = {256, 1, 1};
    const uint32_t *rcode = rast.code.bytes;
    const uint32_t rwords = (uint32_t)(rast.code.length / 4);
    for (NSUInteger t = 0; t < tri; t += 3) {
        const NSUInteger i0 = idx ? idx[t] : t, i1 = idx ? idx[t + 1] : t + 1, i2 = idx ? idx[t + 2] : t + 2;
        if (idx && (i0 >= vCount || i1 >= vCount || i2 >= vCount)) {
            NSLog(@"NVMTLRender: index out of range"); return false;
        }
        uint32_t rp[29];
        rasterPush(rp, rtVa, rtW, rtH, rtStride, clip + 4 * i0, clip + 4 * i1, clip + 4 * i2,
                   vp, fsColor, sc);
        if (!nvGrLaunch(rcode, rwords, rast.regs, rast.slm, rast.smem, rast.barriers, rp, 29, rgrid, rblock)) {
            NSLog(@"NVMTLRender: raster launch failed"); return false;
        }
    }
    return true;
}
