// A fragment shader calling a [[visible]] function from another library,
// bound through MTLRenderPipelineDescriptor.fragmentLinkedFunctions (what
// RenderBox custom effects and SwiftUI [[stitchable]] shaders do).
//   metal_linked_test vf.metallib vfimpl.metallib
// vf.metal:
//   [[visible]] half4 custom_fn(float2 p, half4 c, float k);
//   struct VO { float4 pos [[position]]; float2 uv; };
//   vertex VO vmain(uint vid [[vertex_id]]) { ...full-screen triangle... }
//   fragment half4 fmain(VO in [[stage_in]], constant float &k [[buffer(0)]])
//   { return custom_fn(in.uv, half4(0.25h, 0.5h, 0.75h, 1.0h), k); }
// vfimpl.metal:
//   [[visible]] half4 custom_fn(float2 p, half4 c, float k) { return half4(c.rgb * half(k), c.a); }
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) { fprintf(stderr, "usage: %s vf.metallib vfimpl.metallib\n", argv[0]); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
        id<MTLLibrary> impl = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&e];
        id<MTLFunction> fn = [impl newFunctionWithName:@"custom_fn"];
        if (!lib || !fn) { printf("libraries: %s\nmetal_linked_test: FAIL\n", e.localizedDescription.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vmain"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions];
        lf.functions = @[fn];
        rd.fragmentLinkedFunctions = lf;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        if (!ps) { printf("pipeline: %s\nmetal_linked_test: FAIL\n", e.localizedDescription.UTF8String); return 1; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:16 height:16 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:ps];
        const float k = 1.2f;
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
        const int want[4] = {77, 153, 230, 255};   // 0.3 0.6 0.9 1.0
        int bad = 0;
        for (int i = 0; i < 16 * 16; i++)
            for (int c = 0; c < 4; c++)
                if (abs(px[i * 4 + c] - want[c]) > 2 && bad++ < 4)
                    printf("  pixel %d channel %d: %d want %d\n", i, c, px[i * 4 + c], want[c]);
        printf("metal_linked_test: %s (%d wrong)\n", bad ? "FAIL" : "PASS", bad);
        return bad != 0;
    }
}
