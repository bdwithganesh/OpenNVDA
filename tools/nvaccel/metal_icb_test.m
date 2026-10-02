// Indirect command buffers, CPU-encoded: two compute dispatches with their own
// buffers, and one render draw, replayed by executeCommandsInBuffer.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "kernel void fill(device uint *o [[buffer(0)]], constant uint &v [[buffer(1)]],"
     " uint i [[thread_position_in_grid]]) { o[i] = v + i; }\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs(uint v [[vertex_id]], constant float2 *pos [[buffer(0)]]) { V o; o.p = float4(pos[v], 0, 1); return o; }\n"
     "fragment half4 fs(constant half4 &c [[buffer(0)]]) { return c; }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        if (!lib) { printf("library: %s\nmetal_icb_test: FAIL\n", e.localizedDescription.UTF8String); return 1; }
        int bad = 0;
        // compute
        MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = [lib newFunctionWithName:@"fill"];
        cd.supportIndirectCommandBuffers = YES;
        id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&e];
        MTLIndirectCommandBufferDescriptor *icd = [MTLIndirectCommandBufferDescriptor new];
        icd.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
        icd.inheritBuffers = NO;
        icd.inheritPipelineState = NO;
        icd.maxKernelBufferBindCount = 2;
        id<MTLIndirectCommandBuffer> icb = [dev newIndirectCommandBufferWithDescriptor:icd maxCommandCount:2 options:0];
        if (!cps || !icb) { printf("compute pipeline %p icb %p\nmetal_icb_test: FAIL\n", cps, icb); return 1; }
        id<MTLBuffer> out[2], val[2];
        for (int k = 0; k < 2; k++) {
            out[k] = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
            val[k] = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
            *(uint32_t *)val[k].contents = 1000u * (k + 1);
            id<MTLIndirectComputeCommand> c = [icb indirectComputeCommandAtIndex:k];
            [c setComputePipelineState:cps];
            [c setKernelBuffer:out[k] offset:0 atIndex:0];
            [c setKernelBuffer:val[k] offset:0 atIndex:1];
            [c concurrentDispatchThreadgroups:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        for (int k = 0; k < 2; k++) [ce useResource:out[k] usage:MTLResourceUsageWrite];
        [ce executeCommandsInBuffer:icb withRange:NSMakeRange(0, 2)];
        [ce endEncoding];
        // render
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        rd.supportIndirectCommandBuffers = YES;
        id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
        MTLIndirectCommandBufferDescriptor *ird = [MTLIndirectCommandBufferDescriptor new];
        ird.commandTypes = MTLIndirectCommandTypeDraw;
        ird.inheritPipelineState = NO;
        ird.inheritBuffers = NO;
        ird.maxVertexBufferBindCount = 1;
        ird.maxFragmentBufferBindCount = 1;
        id<MTLIndirectCommandBuffer> ricb = [dev newIndirectCommandBufferWithDescriptor:ird maxCommandCount:1 options:0];
        const float tri[6] = {-1, -1, 3, -1, -1, 3};
        const __fp16 col[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        id<MTLBuffer> vb = [dev newBufferWithBytes:tri length:sizeof tri options:MTLResourceStorageModeShared];
        id<MTLBuffer> fb = [dev newBufferWithBytes:col length:sizeof col options:MTLResourceStorageModeShared];
        id<MTLIndirectRenderCommand> rc = [ricb indirectRenderCommandAtIndex:0];
        [rc setRenderPipelineState:rps];
        [rc setVertexBuffer:vb offset:0 atIndex:0];
        [rc setFragmentBuffer:fb offset:0 atIndex:0];
        [rc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:1 baseInstance:0];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:8 height:8 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re executeCommandsInBuffer:ricb withRange:NSMakeRange(0, 1)];
        [re endEncoding];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl synchronizeResource:rt];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        for (int k = 0; k < 2; k++)
            for (uint32_t i = 0; i < 64; i++)
                if (((uint32_t *)out[k].contents)[i] != 1000u * (k + 1) + i && bad++ < 4)
                    printf("  compute %d [%u] = %u\n", k, i, ((uint32_t *)out[k].contents)[i]);
        uint8_t px[8 * 8 * 4];
        [rt getBytes:px bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
        for (int i = 0; i < 64; i++)
            if ((px[i * 4] > 2 || px[i * 4 + 1] < 253 || px[i * 4 + 3] < 253) && bad++ < 8)
                printf("  pixel %d = %d %d %d %d\n", i, px[i * 4], px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]);
        printf("metal_icb_test: %s (%d wrong)\n", bad ? "FAIL" : "PASS", bad);
        return bad != 0;
    }
}
