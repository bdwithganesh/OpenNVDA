// Shadow mapping as games do it: pass 1 renders depth only (no fragment
// function) into a Depth32Float texture: a quad over the left half at
// z = 0.3, the rest cleared to 1.0. Pass 2 samples that texture as
// depth2d<float> with a comparison sampler (less-equal against 0.5) and
// also with read(), writing lit (green) or shadowed (red) per pixel.
// The left half must be shadowed, the right half lit, both ways.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "vertex float4 vshadow(uint vid [[vertex_id]]) {\n"
     "  float2 p[6] = {float2(-1, -1), float2(0, -1), float2(-1, 1), float2(-1, 1), float2(0, -1), float2(0, 1)};\n"
     "  return float4(p[vid], 0.3, 1); }\n"
     "struct VO { float4 pos [[position]]; float2 uv; };\n"
     "vertex VO vfull(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "  VO o; o.pos = float4(p * 2 - 1, 0, 1); o.uv = float2(p.x, 1 - p.y); return o; }\n"
     "fragment half4 fcmp(VO in [[stage_in]], depth2d<float> sm [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "  float lit = sm.sample_compare(s, in.uv, 0.5); return lit > 0.5 ? half4(0, 1, 0, 1) : half4(1, 0, 0, 1); }\n"
     "fragment half4 fread(VO in [[stage_in]], depth2d<float> sm [[texture(0)]]) {\n"
     "  uint2 c = uint2(in.pos.xy); float z = sm.read(c); return z < 0.5 ? half4(1, 0, 0, 1) : half4(0, 1, 0, 1); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        if (!lib) { printf("%s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const NSUInteger N = 64;
        MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:N height:N mipmapped:NO];
        zd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; zd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> sm = [dev newTextureWithDescriptor:zd];
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vshadow"];
        rd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> sp = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        if (!sp) { printf("shadow pipeline: %s\nmetal_shadow_map_test: FAIL\n", e.localizedDescription.UTF8String); return 1; }
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionLess; dd.depthWriteEnabled = YES;
        MTLSamplerDescriptor *cs = [MTLSamplerDescriptor new];
        cs.compareFunction = MTLCompareFunctionLessEqual;   // lit when 0.5 <= stored depth
        cs.minFilter = cs.magFilter = MTLSamplerMinMagFilterNearest;
        id<MTLSamplerState> cmp = [dev newSamplerStateWithDescriptor:cs];
        int bad = 0;
        for (int mode = 0; mode < 2; mode++) {
            MTLRenderPipelineDescriptor *fd = [MTLRenderPipelineDescriptor new];
            fd.vertexFunction = [lib newFunctionWithName:@"vfull"];
            fd.fragmentFunction = [lib newFunctionWithName:mode ? @"fread" : @"fcmp"];
            fd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> fp = [dev newRenderPipelineStateWithDescriptor:fd error:&e];
            if (!fp) { printf("  lighting pipeline: %s\n", e.localizedDescription.UTF8String); bad++; continue; }
            MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:N height:N mipmapped:NO];
            cd.usage = MTLTextureUsageRenderTarget; cd.storageMode = MTLStorageModeManaged;
            id<MTLTexture> ct = [dev newTextureWithDescriptor:cd];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *p1 = [MTLRenderPassDescriptor renderPassDescriptor];
            p1.depthAttachment.texture = sm; p1.depthAttachment.loadAction = MTLLoadActionClear;
            p1.depthAttachment.clearDepth = 1.0; p1.depthAttachment.storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e1 = [cb renderCommandEncoderWithDescriptor:p1];
            [e1 setRenderPipelineState:sp];
            [e1 setDepthStencilState:[dev newDepthStencilStateWithDescriptor:dd]];
            [e1 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            [e1 endEncoding];
            MTLRenderPassDescriptor *p2 = [MTLRenderPassDescriptor renderPassDescriptor];
            p2.colorAttachments[0].texture = ct; p2.colorAttachments[0].loadAction = MTLLoadActionClear;
            p2.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e2 = [cb renderCommandEncoderWithDescriptor:p2];
            [e2 setRenderPipelineState:fp];
            [e2 setFragmentTexture:sm atIndex:0];
            [e2 setFragmentSamplerState:cmp atIndex:0];
            [e2 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e2 endEncoding];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be synchronizeResource:ct];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            uint8_t px[64 * 64 * 4];
            [ct getBytes:px bytesPerRow:N * 4 fromRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0];
            int wrong = 0;
            for (NSUInteger y = 0; y < N; y++)
                for (NSUInteger x = 0; x < N; x++) {
                    const uint8_t *p = px + (y * N + x) * 4;
                    const bool red = p[0] > 200 && p[1] < 50, green = p[1] > 200 && p[0] < 50;
                    if (x < N / 2 ? !red : !green) wrong++;
                }
            printf("  %-28s %4d of %lu pixels wrong (left %d,%d right %d,%d)\n", mode ? "depth2d read()" : "depth2d sample_compare()",
                   wrong, (unsigned long)(N * N), px[0], px[1], px[(N - 1) * 4], px[(N - 1) * 4 + 1]);
            if (wrong)
                for (NSUInteger y = 0; y < N; y += 8) {
                    printf("    ");
                    for (NSUInteger x = 0; x < N; x += 2) {
                        const uint8_t *p = px + (y * N + x) * 4;
                        putchar(p[0] > 200 && p[1] < 50 ? 'R' : p[1] > 200 && p[0] < 50 ? 'G' : '?');
                    }
                    putchar('\n');
                }
            bad += wrong != 0;
        }
        printf("metal_shadow_map_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
