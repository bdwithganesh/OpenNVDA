// A linked [[visible]] function with globals of its own: a constexpr sampler
// and a constant table, called from a fragment shader in another library
// (SecurityAgent's RenderBox effect failed with "global __air_sampler_state
// in a linked function"). Both libraries come from source here. Prints the
// 16x16 result's corner pixels; every pixel must match the CPU expectation.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kMain =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "[[visible]] half4 fx(texture2d<half> t, float2 uv, float k);\n"
     "struct VO { float4 pos [[position]]; float2 uv; };\n"
     "vertex VO vmain(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "  VO o; o.pos = float4(p * 2 - 1, 0, 1); o.uv = float2(p.x, 1 - p.y); return o; }\n"
     "fragment half4 fmain(VO in [[stage_in]], texture2d<half> t [[texture(0)]], constant float &k [[buffer(0)]])\n"
     "{ return fx(t, in.uv, k); }\n";
static NSString *const kImpl =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "constant float kTable[4] = {0.25, 0.5, 0.75, 1.0};\n"
     "[[visible]] half4 fx(texture2d<half> t, float2 uv, float k) {\n"
     "  constexpr sampler s(filter::nearest, address::clamp_to_edge, coord::normalized);\n"
     "  half4 c = t.sample(s, uv);\n"
     "  return half4(c.rgb * half(kTable[int(k) & 3]), 1.0h);\n"
     "}\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kMain options:nil error:&e];
        id<MTLLibrary> impl = [dev newLibraryWithSource:kImpl options:nil error:&e];
        id<MTLFunction> fn = [impl newFunctionWithName:@"fx"];
        if (!lib || !fn) { printf("libraries: %s\nmetal_linked_globals_test: FAIL\n", e.localizedDescription.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vmain"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions];
        lf.functions = @[fn];
        rd.fragmentLinkedFunctions = lf;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        if (!ps) { printf("pipeline: %s\nmetal_linked_globals_test: FAIL\n", e.localizedDescription.UTF8String); return 1; }
        // 2x2 source: red, green / blue, white
        MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
        id<MTLTexture> src = [dev newTextureWithDescriptor:sd];
        const uint8_t sp[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        [src replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:sp bytesPerRow:8];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:ps];
        [enc setFragmentTexture:src atIndex:0];
        const float k = 2.0f;   // kTable[2] = 0.75
        [enc setFragmentBytes:&k length:4 atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl synchronizeResource:rt];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        uint8_t px[16 * 16 * 4];
        [rt getBytes:px bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
        int bad = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                // uv.y = 1 - p.y flips: the top rows sample texel row 0
                const uint8_t *s = sp + ((y < 8 ? 0 : 1) * 2 + (x < 8 ? 0 : 1)) * 4;
                for (int c = 0; c < 3; c++) {
                    const int want = (int)(s[c] * 0.75f / 255.0f * 255.0f + 0.5f);
                    if (abs(px[(y * 16 + x) * 4 + c] - want) > 2) bad++;
                }
            }
        printf("  corners: %d %d %d / %d %d %d / %d %d %d / %d %d %d\n", px[0], px[1], px[2], px[60], px[61], px[62],
               px[15 * 64], px[15 * 64 + 1], px[15 * 64 + 2], px[15 * 64 + 60], px[15 * 64 + 61], px[15 * 64 + 62]);
        printf("metal_linked_globals_test: %s (%d wrong)\n", bad ? "FAIL" : "PASS", bad);
        return bad != 0;
    }
}
