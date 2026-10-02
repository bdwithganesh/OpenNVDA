// Shader barycentrics ([[barycentric_coord]]) and pull-model interpolation
// (interpolant<T>::interpolate_at_center / _at_offset): a full-screen
// triangle into 64x64; the pixels are compared with the M1's (saved by the
// first run on the M1 with "save", checked on the RTX with "check").
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 pos [[position]]; float4 col; };\n"
     "vertex V vs(uint vid [[vertex_id]]) {\n"
     "  const float2 p[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };\n"
     "  V o; o.pos = float4(p[vid], 0, 1); o.col = float4(vid == 0, vid == 1, vid == 2, 1); return o; }\n"
     "struct F { float4 pos [[position]]; interpolant<float4, interpolation::perspective> col; };\n"
     "fragment float4 fsBary(V in [[stage_in]], float3 b [[barycentric_coord]]) { return float4(b, 1); }\n"
     "fragment float4 fsPull(F in [[stage_in]]) {\n"
     "  float4 c = in.col.interpolate_at_offset(float2(0.4, -0.3));\n"
     "  float4 s = in.col.interpolate_at_center();\n"
     "  return float4(c.r, s.g, (c.b - s.b) * 8 + 0.5, 1); }\n";

int main(int argc, char **argv) {
    @autoreleasepool {
        const BOOL save = argc > 1 && !strcmp(argv[1], "save");
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        if (!lib) { printf("%s\nmetal_bary_test: FAIL\n", e.description.UTF8String); return 1; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeManaged;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int bad = 0;
        for (int k = 0; k < 2; k++) {
            NSString *fs = k ? @"fsPull" : @"fsBary";
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:@"vs"];
            rd.fragmentFunction = [lib newFunctionWithName:fs];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            if (!ps) { printf("%s: %s\n", fs.UTF8String, e.description.UTF8String); bad++; continue; }
            id<MTLTexture> t = [dev newTextureWithDescriptor:td];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
            rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:ps];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be synchronizeResource:t];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            NSMutableData *px = [NSMutableData dataWithLength:64 * 64 * 4];
            [t getBytes:px.mutableBytes bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            NSString *ref = [NSString stringWithFormat:@"bary_ref_%@.bin", fs];
            if (save) { [px writeToFile:ref atomically:YES]; printf("  %s saved\n", fs.UTF8String); continue; }
            NSData *want = [NSData dataWithContentsOfFile:ref];
            if (!want) { printf("  %s: no %s\n", fs.UTF8String, ref.UTF8String); bad++; continue; }
            const uint8_t *a = px.bytes, *w = want.bytes;
            int diff = 0, maxd = 0;
            for (int i = 0; i < 64 * 64 * 4; i++) { const int d = abs(a[i] - w[i]); if (d > 2) diff++; if (d > maxd) maxd = d; }
            printf("  %s: %d channel values off by more than 2 (max %d), centre %d %d %d\n", fs.UTF8String, diff, maxd,
                   a[(32 * 64 + 32) * 4], a[(32 * 64 + 32) * 4 + 1], a[(32 * 64 + 32) * 4 + 2]);
            bad += diff > 0;
        }
        if (!save) printf("metal_bary_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
