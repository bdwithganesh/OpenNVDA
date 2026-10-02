// Front-face winding and cull mode: one triangle, both vertex orders, each
// winding / cull combination; prints whether it drew. Compare with Apple's
// driver (same program on the M1).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs(uint v [[vertex_id]], constant float2 *q [[buffer(0)]]) { V o; o.p = float4(q[v], 0.5, 1); return o; }\n"
     "fragment half4 fs() { return half4(0, 1, 0, 1); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        // counter-clockwise in NDC (y up): (-1,-1) (1,-1) (0,1); clockwise: the reverse
        const float ccw[6] = {-1, -1, 1, -1, 0, 1}, cw[6] = {0, 1, 1, -1, -1, -1};
        const char *res[2][2][3];
        for (int tri = 0; tri < 2; tri++)
            for (int wind = 0; wind < 2; wind++)
                for (int cull = 0; cull < 3; cull++) {
                    MTLTextureDescriptor *ct = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                  width:32 height:32 mipmapped:NO];
                    ct.usage = MTLTextureUsageRenderTarget;
                    ct.storageMode = MTLStorageModePrivate;
                    id<MTLTexture> c = [dev newTextureWithDescriptor:ct];
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = c;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                    [re setRenderPipelineState:ps];
                    [re setVertexBytes:tri ? cw : ccw length:24 atIndex:0];
                    [re setFrontFacingWinding:wind ? MTLWindingCounterClockwise : MTLWindingClockwise];
                    [re setCullMode:cull == 0 ? MTLCullModeNone : cull == 1 ? MTLCullModeBack : MTLCullModeFront];
                    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [re endEncoding];
                    id<MTLBuffer> out = [dev newBufferWithLength:32 * 32 * 4 options:MTLResourceStorageModeShared];
                    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                    [be copyFromTexture:c sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:MTLSizeMake(32, 32, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:128
                destinationBytesPerImage:32 * 128];
                    [be endEncoding];
                    [cb commit];
                    [cb waitUntilCompleted];
                    int g = 0;
                    for (int i = 0; i < 32 * 32; i++) if (((uint8_t *)out.contents)[i * 4 + 1] > 200) g++;
                    res[tri][wind][cull] = g > 100 ? "drawn " : "culled";
                }
        printf("triangle   front=CW: none back front | front=CCW: none back front\n");
        for (int tri = 0; tri < 2; tri++)
            printf("%-10s          %s %s %s |            %s %s %s\n", tri ? "CW verts" : "CCW verts", res[tri][0][0], res[tri][0][1],
                   res[tri][0][2], res[tri][1][0], res[tri][1][1], res[tri][1][2]);
        return 0;
    }
}
