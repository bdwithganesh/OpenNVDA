// Staged Metal test for NVAccelerator/NVMTLDriver (B4, M15-M18).
//   metal_test [stage]   stages: 1 buffer, 2 map+rw, 3 queue, 4 empty command buffer,
//                        5 blit fill, 6 compute (vector add), 7 render triangle,
//                        8 render triangle to IOSurface, 9 BGRA8 render,
//                        10 indexed quad, 11 fences/events
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <stdlib.h>
static id<MTLDevice> findNV(void) {
    for (id<MTLDevice> d in MTLCopyAllDevices())
        if ([d.name containsString:@"RTX"]) return d;
    return nil;
}
#define STEP(n, what) do { printf("[%d] %s ...\n", n, what); fflush(stdout); } while (0)
int main(int argc, char **argv) {
    int upto = argc > 1 ? atoi(argv[1]) : 6;
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no RTX Metal device\n"); return 1; }
        printf("device %s\n", dev.name.UTF8String);
        STEP(1, "newBufferWithLength 1 MiB shared");
        id<MTLBuffer> buf = [dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared];
        printf("    buffer %p length %lu gpuAddress 0x%llx\n", (__bridge void *)buf, (unsigned long)buf.length,
               [buf respondsToSelector:@selector(gpuAddress)] ? (unsigned long long)[(id)buf gpuAddress] : 0ULL);
        if (!buf || upto < 2) return !buf;
        STEP(2, "contents write/read");
        uint32_t *p = buf.contents;
        if (!p) { printf("    contents NULL\n"); return 1; }
        for (int i = 0; i < 1024; i++) p[i] = 0x1000 + i;
        int bad = 0;
        for (int i = 0; i < 1024; i++) bad += p[i] != 0x1000u + i;
        printf("    rw %s\n", bad ? "FAIL" : "ok");
        if (upto < 3) return bad;
        STEP(3, "newCommandQueue");
        id<MTLCommandQueue> q = [dev newCommandQueue];
        printf("    queue %p\n", (__bridge void *)q);
        if (!q || upto < 4) return !q;
        STEP(4, "empty command buffer commit + wait");
        id<MTLCommandBuffer> cb = [q commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
        printf("    status %ld error %s\n", (long)cb.status, cb.error ? cb.error.description.UTF8String : "-");
        if (upto < 5) return cb.status != MTLCommandBufferStatusCompleted;
        STEP(5, "blit fillBuffer 0xab");
        cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl fillBuffer:buf range:NSMakeRange(0, 4096) value:0xab];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        bad = 0;
        for (int i = 0; i < 1024; i++) bad += p[i] != 0xababababu;
        printf("    status %ld fill %s\n", (long)cb.status, bad ? "FAIL" : "ok");
        STEP(5, "blit copyBuffer 4 KiB");
        id<MTLBuffer> buf2 = [dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared];
        uint32_t *p2 = buf2.contents;
        for (int i = 0; i < 1024; i++) p2[i] = 0;
        cb = [q commandBuffer];
        bl = [cb blitCommandEncoder];
        [bl copyFromBuffer:buf sourceOffset:0 toBuffer:buf2 destinationOffset:0 size:4096];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        int bad2 = !buf2 || !p2;
        for (int i = 0; i < 1024; i++) bad2 += p2[i] != 0xababababu;
        printf("    status %ld copy %s\n", (long)cb.status, bad2 ? "FAIL" : "ok");
        bad += bad2;
        if (upto < 6) return bad;
        STEP(6, "compute vector add");
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"kernel void add(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],"
             " device float *c [[buffer(2)]], uint i [[thread_position_in_grid]]) { c[i] = a[i] + b[i]; }"
                                              options:nil error:&err];
        printf("    library %p %s\n", (__bridge void *)lib, err ? err.description.UTF8String : "");
        if (!lib) return 1;
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"add"] error:&err];
        printf("    pipeline %p %s\n", (__bridge void *)ps, err ? err.description.UTF8String : "");
        if (!ps) return 1;
        const int N = 4096;
        id<MTLBuffer> a = [dev newBufferWithLength:N * 4 options:0], b = [dev newBufferWithLength:N * 4 options:0],
                      c = [dev newBufferWithLength:N * 4 options:0];
        float *fa = a.contents, *fb = b.contents, *fc = c.contents;
        for (int i = 0; i < N; i++) { fa[i] = i; fb[i] = 2 * i; fc[i] = -1; }
        cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps];
        [ce setBuffer:a offset:0 atIndex:0];
        [ce setBuffer:b offset:0 atIndex:1];
        [ce setBuffer:c offset:0 atIndex:2];
        [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        bad = 0;
        for (int i = 0; i < N; i++) bad += fc[i] != 3.0f * i;
        printf("    status %ld add %s (%d bad)\n", (long)cb.status, bad ? "FAIL" : "PASS", bad);
        STEP(6, "compute sgemm 128x128 vs CPU");
        id<MTLLibrary> lib2 = [dev newLibraryWithSource:
            @"kernel void sgemm(device const float *A [[buffer(0)]], device const float *B [[buffer(1)]],"
             " device float *C [[buffer(2)]], uint i [[thread_position_in_grid]]) {"
             " uint row = i / 128u, col = i % 128u; float s = 0.0f;"
             " for (uint k = 0u; k < 128u; k++) s += A[row * 128u + k] * B[k * 128u + col];"
             " C[i] = s; }"
                                              options:nil error:&err];
        int sbad = !lib2;
        id<MTLComputePipelineState> ps2 = lib2 ?
            [dev newComputePipelineStateWithFunction:[lib2 newFunctionWithName:@"sgemm"] error:&err] : nil;
        sbad += !ps2;
        const int SN = 128, SN2 = SN * SN;
        id<MTLBuffer> mA = [dev newBufferWithLength:SN2 * 4 options:0],
                      mB = [dev newBufferWithLength:SN2 * 4 options:0],
                      mC = [dev newBufferWithLength:SN2 * 4 options:0];
        float *fA = mA.contents, *fB = mB.contents, *fC = mC.contents;
        sbad += !fA || !fB || !fC;
        for (int i = 0; i < SN2; i++) { fA[i] = (i % 7) - 3; fB[i] = (i % 5) - 2; fC[i] = -1; }
        if (!sbad) {
            cb = [q commandBuffer];
            ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps2];
            [ce setBuffer:mA offset:0 atIndex:0];
            [ce setBuffer:mB offset:0 atIndex:1];
            [ce setBuffer:mC offset:0 atIndex:2];
            [ce dispatchThreads:MTLSizeMake(SN2, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            for (int r = 0; r < SN && !sbad; r++)
                for (int cc = 0; cc < SN; cc++) {
                    float s = 0;
                    for (int k = 0; k < SN; k++) s += fA[r * SN + k] * fB[k * SN + cc];
                    float d = fC[r * SN + cc] - s;
                    if (d < -1e-3 || d > 1e-3) { sbad++; break; }
                }
        }
        printf("    status %ld sgemm %s\n", (long)cb.status, sbad ? "FAIL" : "PASS");
        if (upto < 7) return (bad || sbad) != 0;
        STEP(7, "render red triangle to 64x64 RGBA8");
        id<MTLLibrary> lib3 = [dev newLibraryWithSource:
            @"vertex float4 vmain(device const float2 *pos [[buffer(0)]], uint vid [[vertex_id]]) {"
            @" return float4(pos[vid].x, pos[vid].y, 0.0, 1.0); }"
            @" fragment float4 fmain() { return float4(1.0, 0.0, 0.0, 1.0); }"
                                              options:nil error:&err];
        printf("    library %p %s\n", (__bridge void *)lib3, err ? err.description.UTF8String : "");
        int rbad = !lib3;
        id<MTLRenderPipelineState> rps = nil;
        if (lib3) {
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib3 newFunctionWithName:@"vmain"];
            rd.fragmentFunction = [lib3 newFunctionWithName:@"fmain"];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            rps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
            printf("    pipeline %p %s\n", (__bridge void *)rps, err ? err.description.UTF8String : "");
            rbad += !rps;
        }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:64 height:64 mipmapped:NO];
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        rbad += !rt;
        float verts[6] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
        id<MTLBuffer> vb = [dev newBufferWithBytes:verts length:sizeof verts options:MTLResourceStorageModeShared];
        rbad += !vb;
        if (!rbad) {
            MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
            pd.colorAttachments[0].texture = rt;
            pd.colorAttachments[0].loadAction = MTLLoadActionClear;
            pd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            pd.colorAttachments[0].storeAction = MTLStoreActionStore;
            cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pd];
            [re setRenderPipelineState:rps];
            [re setVertexBuffer:vb offset:0 atIndex:0];
            [re setViewport:(MTLViewport){0, 0, 64, 64, 0, 1}];
            [re setScissorRect:(MTLScissorRect){0, 0, 64, 64}];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            uint32_t px[64 * 64];
            [rt getBytes:px bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            int red = 0;
            for (int i = 0; i < 64 * 64; i++) red += px[i] == 0xFF0000FFu;
            int centerRed = px[32 * 64 + 32] == 0xFF0000FFu;
            int cornerBlack = px[0] == 0xFF000000u;
            printf("    status %ld red=%d center=%d corner=%d\n", (long)cb.status, red, centerRed, cornerBlack);
            rbad += !(red > 300 && red < 800 && centerRed && cornerBlack);
        }
        printf("    triangle %s\n", rbad ? "FAIL" : "PASS");
        if (upto < 8) return (bad || sbad || rbad) != 0;
        STEP(8, "render triangle to IOSurface-backed texture");
        int ibad = rbad; // needs the stage-7 pipeline + vertex buffer
        IOSurfaceRef io = NULL;
        if (!ibad) {
            NSDictionary *props = @{(id)kIOSurfaceWidth: @64, (id)kIOSurfaceHeight: @64,
                                    (id)kIOSurfacePixelFormat: @(kCVPixelFormatType_32RGBA),
                                    (id)kIOSurfaceBytesPerElement: @4};
            io = IOSurfaceCreate((CFDictionaryRef)props);
            ibad += !io;
        }
        id<MTLTexture> iot = NULL;
        if (!ibad) {
            IOSurfaceLock(io, 0, NULL);
            iot = [dev newTextureWithDescriptor:td iosurface:io plane:0];
            printf("    io texture %p bpr %zu\n", (__bridge void *)iot, IOSurfaceGetBytesPerRow(io));
            ibad += !iot;
        }
        if (!ibad) {
            MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
            pd.colorAttachments[0].texture = iot;
            pd.colorAttachments[0].loadAction = MTLLoadActionClear;
            pd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            pd.colorAttachments[0].storeAction = MTLStoreActionStore;
            cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pd];
            [re setRenderPipelineState:rps];
            [re setVertexBuffer:vb offset:0 atIndex:0];
            [re setViewport:(MTLViewport){0, 0, 64, 64, 0, 1}];
            [re setScissorRect:(MTLScissorRect){0, 0, 64, 64}];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            // Independent oracle: count straight from the surface mapping.
            uint32_t *spx = IOSurfaceGetBaseAddress(io);
            int red = 0;
            for (int i = 0; i < 64 * 64; i++) red += spx[i] == 0xFF0000FFu;
            int centerRed = spx[32 * 64 + 32] == 0xFF0000FFu;
            int cornerBlack = spx[0] == 0xFF000000u;
            printf("    status %ld red=%d center=%d corner=%d\n", (long)cb.status, red, centerRed, cornerBlack);
            ibad += !(red == 512 && centerRed && cornerBlack);
            IOSurfaceUnlock(io, 0, NULL);
        }
        if (io) CFRelease(io);
        printf("    iosurface triangle %s\n", ibad ? "FAIL" : "PASS");
        if (upto < 9) return (bad || sbad || rbad || ibad) != 0;
        STEP(9, "render red triangle to 64x64 BGRA8 (blue clear)");
        int gbad = rbad; // needs the stage-7 pipeline + vertex buffer
        MTLTextureDescriptor *td9 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:64 height:64 mipmapped:NO];
        id<MTLTexture> rt9 = [dev newTextureWithDescriptor:td9];
        gbad += !rt9 || rt9.pixelFormat != MTLPixelFormatBGRA8Unorm;
        if (!gbad) {
            MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
            pd.colorAttachments[0].texture = rt9;
            pd.colorAttachments[0].loadAction = MTLLoadActionClear;
            pd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
            pd.colorAttachments[0].storeAction = MTLStoreActionStore;
            cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pd];
            [re setRenderPipelineState:rps];
            [re setVertexBuffer:vb offset:0 atIndex:0];
            [re setViewport:(MTLViewport){0, 0, 64, 64, 0, 1}];
            [re setScissorRect:(MTLScissorRect){0, 0, 64, 64}];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            uint32_t px9[64 * 64];
            [rt9 getBytes:px9 bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            int red = 0;
            for (int i = 0; i < 64 * 64; i++) red += px9[i] == 0xFFFF0000u;
            int centerRed = px9[32 * 64 + 32] == 0xFFFF0000u;
            int cornerBlue = px9[0] == 0xFF0000FFu;
            printf("    status %ld red=%d center=%d corner=%d\n", (long)cb.status, red, centerRed, cornerBlue);
            gbad += !(red == 512 && centerRed && cornerBlue);
        }
        printf("    bgra triangle %s\n", gbad ? "FAIL" : "PASS");
        if (upto < 10) return (bad || sbad || rbad || ibad || gbad) != 0;
        STEP(10, "indexed quad (uint16 + uint32)");
        int xbad = rbad; // needs the stage-7 pipeline
        float qv[8] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
        id<MTLBuffer> qvb = [dev newBufferWithBytes:qv length:sizeof qv options:MTLResourceStorageModeShared];
        uint16_t qi16[6] = {0, 1, 2, 0, 2, 3};
        uint32_t qi32[6] = {0, 1, 2, 0, 2, 3};
        id<MTLBuffer> ib16 = [dev newBufferWithBytes:qi16 length:sizeof qi16 options:MTLResourceStorageModeShared];
        id<MTLBuffer> ib32 = [dev newBufferWithBytes:qi32 length:sizeof qi32 options:MTLResourceStorageModeShared];
        xbad += !qvb || !ib16 || !ib32;
        for (int pass = 0; pass < 2 && !xbad; pass++) {
            id<MTLTexture> qrt = [dev newTextureWithDescriptor:td];
            xbad += !qrt;
            if (xbad) break;
            MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
            pd.colorAttachments[0].texture = qrt;
            pd.colorAttachments[0].loadAction = MTLLoadActionClear;
            pd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            pd.colorAttachments[0].storeAction = MTLStoreActionStore;
            cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pd];
            [re setRenderPipelineState:rps];
            [re setVertexBuffer:qvb offset:0 atIndex:0];
            [re setViewport:(MTLViewport){0, 0, 64, 64, 0, 1}];
            [re setScissorRect:(MTLScissorRect){0, 0, 64, 64}];
            if (pass == 0)
                [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16
                              indexBuffer:ib16 indexBufferOffset:0];
            else
                [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt32
                              indexBuffer:ib32 indexBufferOffset:0];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            uint32_t qpx[64 * 64];
            [qrt getBytes:qpx bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
            int qred = 0;
            for (int i = 0; i < 64 * 64; i++) qred += qpx[i] == 0xFF0000FFu;
            int qcenter = qpx[32 * 64 + 32] == 0xFF0000FFu;
            int qcorner = qpx[0] == 0xFF000000u;
            printf("    pass %d (%s): red=%d center=%d corner=%d\n", pass, pass ? "u32" : "u16", qred, qcenter, qcorner);
            xbad += !(qred > 950 && qred < 1100 && qcenter && qcorner);
        }
        printf("    indexed quad %s\n", xbad ? "FAIL" : "PASS");
        if (upto < 11) return (bad || sbad || rbad || ibad || gbad || xbad) != 0;
        STEP(11, "fences + shared events");
        int ebad = rbad; // render-encoder leg needs the stage-7 pipeline
        id<MTLFence> fence = [dev newFence];
        ebad += !fence;
        id<MTLSharedEvent> sev = [dev newSharedEvent];
        ebad += !sev;
        if (!ebad) {
            sev.signaledValue = 42;
            ebad += sev.signaledValue != 42;
            dispatch_queue_t lq = dispatch_queue_create("mt11", DISPATCH_QUEUE_SERIAL);
            MTLSharedEventListener *lis = [[MTLSharedEventListener alloc] initWithDispatchQueue:lq];
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            __block uint64_t got = 0;
            [sev notifyListener:lis atValue:42 block:^(id<MTLSharedEvent> e, uint64_t v) {
                (void)e; got = v; dispatch_semaphore_signal(sem);
            }];
            ebad += dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0;
            ebad += got != 42;
            got = 0;
            [sev notifyListener:lis atValue:100 block:^(id<MTLSharedEvent> e, uint64_t v) {
                (void)e; got = v; dispatch_semaphore_signal(sem);
            }];
            sev.signaledValue = 100;
            ebad += dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0;
            ebad += got != 100;
            printf("    event set/get/notify %s\n", ebad ? "FAIL" : "ok");
        }
        if (!ebad) {
            cb = [q commandBuffer];
            ce = [cb computeCommandEncoder];
            [ce waitForFence:fence];
            [ce updateFence:fence];
            [ce endEncoding];
            bl = [cb blitCommandEncoder];
            [bl waitForFence:fence];
            [bl updateFence:fence];
            [bl endEncoding];
            [cb encodeSignalEvent:sev value:7];
            [cb encodeWaitForEvent:sev value:7];
            [cb commit];
            [cb waitUntilCompleted];
            ebad += cb.status != MTLCommandBufferStatusCompleted;
            ebad += sev.signaledValue != 100; // signal(7) must not move 100 back
            printf("    fence+signal/wait commit %s (value=%llu)\n",
                   ebad ? "FAIL" : "ok", sev.signaledValue);
        }
        if (!ebad) {
            MTLRenderPassDescriptor *pd11 = [MTLRenderPassDescriptor renderPassDescriptor];
            pd11.colorAttachments[0].texture = rt;
            pd11.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pd11.colorAttachments[0].storeAction = MTLStoreActionStore;
            cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pd11];
            [re waitForFence:fence beforeStages:MTLRenderStageVertex];
            [re setRenderPipelineState:rps];
            [re setVertexBuffer:vb offset:0 atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re updateFence:fence afterStages:MTLRenderStageFragment];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            ebad += cb.status != MTLCommandBufferStatusCompleted;
            printf("    render fence leg %s\n", ebad ? "FAIL" : "ok");
        }
        if (!ebad) {
            // Never-signaled wait must time out, never hang.
            id<MTLSharedEvent> ev2 = [dev newSharedEvent];
            cb = [q commandBuffer];
            [cb encodeWaitForEvent:ev2 value:9999];
            [cb commit];
            [cb waitUntilCompleted];
            ebad += !ev2 || cb.status != MTLCommandBufferStatusCompleted;
            printf("    unsatisfied wait no-hang %s\n", ebad ? "FAIL" : "ok");
        }
        printf("    fences/events %s\n", ebad ? "FAIL" : "PASS");
        return (bad || sbad || rbad || ibad || gbad || xbad || ebad) != 0;
    }
}
