// metal_mrt_mask_test: RenderBox's coverage accumulation: colour 0 masked off, colour 1 RG16Float
// with write mask green only and additive blending (one, one), fragment functions that return only
// [[color(1)]] (or both colours). Two overlapping full-target draws of 0.25 must leave green 0.5;
// red stays at its clear value 0.125, colour 0 at its clear colour. Variants:
//   only1  fragment returns struct { half2 [[color(1)]] }
//   both   fragment returns struct { half4 [[color(0)]]; half2 [[color(1)]] }
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static int gMaskAll, gNoBlend, gNoStencil, gC0Mask, gStencilDontCare;
static int run(id<MTLDevice> dev, const char *fn)
{
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct VO { float4 p [[position]]; };\n"
         "struct O1 { half2 c1 [[color(1)]]; };\n"
         "struct O2 { half4 c0 [[color(0)]]; half2 c1 [[color(1)]]; };\n"
         "vertex VO vs(uint v [[vertex_id]]) { float2 t[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(t[v], 0, 1); return o; }\n"
         "fragment O1 only1() { O1 o; o.c1 = half2(0.75, 0.25); return o; }\n"
         "fragment O2 both() { O2 o; o.c0 = half4(1, 1, 1, 1); o.c1 = half2(0.75, 0.25); return o; }\n"
                                          options:nil error:&err];
    if (!lib) { printf("compile: %s\n", err.description.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction = [lib newFunctionWithName:@"vs"];
    rd.fragmentFunction = [lib newFunctionWithName:@(fn)];
    rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    rd.colorAttachments[0].writeMask = gC0Mask ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    rd.colorAttachments[1].pixelFormat = MTLPixelFormatRG16Float;
    rd.colorAttachments[1].writeMask = gMaskAll ? MTLColorWriteMaskAll : MTLColorWriteMaskGreen;
    rd.colorAttachments[1].blendingEnabled = !gNoBlend;
    rd.colorAttachments[1].sourceRGBBlendFactor = rd.colorAttachments[1].destinationRGBBlendFactor = MTLBlendFactorOne;
    rd.colorAttachments[1].sourceAlphaBlendFactor = rd.colorAttachments[1].destinationAlphaBlendFactor = MTLBlendFactorOne;
    if (!gNoStencil) rd.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
    if (!ps) { printf("pipeline: %s\n", err.description.UTF8String); return 1; }
    const int S = 32;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:S height:S mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> c0 = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatRG16Float;
    id<MTLTexture> c1 = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatStencil8;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> st = [dev newTextureWithDescriptor:td];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = c0;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0.5, 0, 1);
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[1].texture = c1;
    p.colorAttachments[1].loadAction = MTLLoadActionClear;
    p.colorAttachments[1].clearColor = MTLClearColorMake(0.125, 0, 0, 0);
    p.colorAttachments[1].storeAction = MTLStoreActionStore;
    if (!gNoStencil) {
        p.stencilAttachment.texture = st;
        p.stencilAttachment.loadAction = gStencilDontCare ? MTLLoadActionDontCare : MTLLoadActionClear;
    }
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
    [re setRenderPipelineState:ps];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
    id<MTLBuffer> b0 = [dev newBufferWithLength:S * S * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> b1 = [dev newBufferWithLength:S * S * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:c0 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
               toBuffer:b0 destinationOffset:0 destinationBytesPerRow:S * 4 destinationBytesPerImage:S * S * 4];
    [bl copyFromTexture:c1 sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S, S, 1)
               toBuffer:b1 destinationOffset:0 destinationBytesPerRow:S * 4 destinationBytesPerImage:S * S * 4];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint8_t *p0 = (const uint8_t *)b0.contents + (16 * S + 16) * 4;
    const __fp16 *p1 = (const __fp16 *)b1.contents + (16 * S + 16) * 2;
    const float r = p1[0], g = p1[1];
    const int ok = p0[0] == 0 && p0[1] >= 127 && p0[1] <= 128 && p0[3] == 255 && r > 0.12f && r < 0.13f && g > 0.49f && g < 0.51f;
    printf("%-6s c0 (%u %u %u %u) want (0 128 0 255); c1 (%.3f %.3f) want (0.125 0.500): %s\n", fn, p0[0], p0[1], p0[2], p0[3],
           r, g, ok ? "ok" : "FAIL");
    return !ok;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        // debug variants: maskall, noblend, nostencil, c0mask (colour 0 written), sdontcare (stencil load DontCare);
        // only the default run is the conformance check
        for (int i = 1; i < argc; i++) { gMaskAll |= !strcmp(argv[i], "maskall"); gNoBlend |= !strcmp(argv[i], "noblend");
                                         gNoStencil |= !strcmp(argv[i], "nostencil"); gC0Mask |= !strcmp(argv[i], "c0mask");
                                         gStencilDontCare |= !strcmp(argv[i], "sdontcare"); }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        const int fails = run(dev, "only1") + run(dev, "both");
        printf("metal_mrt_mask_test on %s: %s\n", dev.name.UTF8String, fails ? "FAIL" : "PASS");
        return fails ? 1 : 0;
    }
}
