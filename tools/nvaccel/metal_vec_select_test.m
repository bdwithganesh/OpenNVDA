// metal_vec_select_test: per-component select / comparisons on float2 in a vertex function, the
// corner arithmetic of RenderBox's primitive vertex shader (inset negated per axis where the
// corner coordinate is 1). Vertex ids 0..7 write their result to a buffer from the vertex stage
// (rasterization off); checked against the CPU.
#import <Metal/Metal.h>
#include <stdio.h>
#include <math.h>
int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "vertex void vs(uint v [[vertex_id]], device float2 *out [[buffer(0)]], constant float4 &ins [[buffer(1)]], constant float &uw [[buffer(2)]]) {"
             " ushort i = ushort(v) & 3; float2 c = float2((i == 0 || i == 3) ? 0.0 : 1.0, (1 < i) ? 1.0 : 0.0);"
             " if (ushort(v) > 3) { float a = i < 2 ? (i < 1 ? ins.x : ins.y) : (i < 3 ? ins.z : ins.w);"
             "   float2 d = float2(a); d = select(d, -d, float2(0.5) < c); d.x /= uw; c += d; }"
             " out[v] = c; }\n" options:nil error:&err];
        if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.rasterizationEnabled = NO;
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!ps) { printf("pipeline: %s\n", err.description.UTF8String); return 1; }
        id<MTLBuffer> out = [dev newBufferWithLength:8 * 8 options:MTLResourceStorageModeShared];
        memset(out.contents, 0xff, 64);
        const float ins[4] = { 0.1f, 0.2f, 0.3f, 0.4f }, uw = 2.0f;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = t;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        [re setRenderPipelineState:ps];
        [re setVertexBuffer:out offset:0 atIndex:0];
        [re setVertexBytes:ins length:16 atIndex:1];
        [re setVertexBytes:&uw length:4 atIndex:2];
        [re drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:8];
        [re endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        const float *o = out.contents;
        int bad = 0;
        for (int v = 0; v < 8; v++) {
            const int i = v & 3;
            float cx = (i == 0 || i == 3) ? 0 : 1, cy = i > 1 ? 1 : 0;
            if (v > 3) { float a = ins[i]; float dx = cx > 0.5f ? -a : a, dy = cy > 0.5f ? -a : a; cx += dx / uw; cy += dy; }
            const int ok = fabsf(o[2 * v] - cx) < 1e-6f && fabsf(o[2 * v + 1] - cy) < 1e-6f;
            if (!ok) printf("  vertex %d: got (%g %g) want (%g %g)\n", v, o[2 * v], o[2 * v + 1], cx, cy);
            bad += !ok;
        }
        printf("metal_vec_select_test on %s: %s\n", dev.name.UTF8String, bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
