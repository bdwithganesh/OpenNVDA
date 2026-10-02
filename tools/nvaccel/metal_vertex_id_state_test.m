// Check Metal vertex_id after another client changes the shared GR register.
// Default: portable Metal reference checks. --poison: RTX only, writes -1 to
// VERTEX_ID_BASE via NVGspControl before each draw, then restores zero on exit.
// The shader masks its output index, so bad vertex IDs cannot cause an MMU fault.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>

static kern_return_t setVertexIdBase(io_connect_t conn, uint32_t value) {
    const uint32_t words[] = {0x20010000u, 0xc997u, 0x20010446u, value};
    uint64_t seq = 0, done = 0;
    uint32_t n = 1;
    kern_return_t kr = IOConnectCallMethod(conn, 17, NULL, 0, words, sizeof(words),
                                          &seq, &n, NULL, NULL);
    if (kr) return kr;
    const uint64_t wait[] = {seq, 2000000};
    n = 1;
    return IOConnectCallMethod(conn, 18, wait, 2, NULL, 0, &done, &n, NULL, NULL);
}

static io_connect_t openPoisonClient(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
    if (!svc) return IO_OBJECT_NULL;
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr) return IO_OBJECT_NULL;
    // A ready arena is required even for a register-only submission. The
    // small object and its mapping are released by closing this client.
    uint64_t in[] = {65536, 0}, out[2] = {0, 0};
    uint32_t n = 2;
    kr = IOConnectCallMethod(conn, 22, in, 2, NULL, 0, out, &n, NULL, NULL);
    if (!kr) {
        const uint64_t bind[] = {out[0], 0x2800000000ull, 0};
        kr = IOConnectCallMethod(conn, 24, bind, 3, NULL, 0, NULL, NULL, NULL, NULL);
    }
    if (kr) { IOServiceClose(conn); return IO_OBJECT_NULL; }
    return conn;
}

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--poison"))) {
        fprintf(stderr, "usage: metal_vertex_id_state_test [--poison]\n");
        return 2;
    }
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        NSError *error = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct V { float4 p [[position]]; };\n"
             "vertex V vs(uint v [[vertex_id]], device uint *seen [[buffer(0)]]) {\n"
             " seen[v & 15u] = v; V o; uint j = v % 3u;\n"
             " float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
             " o.p = float4(p[j],0,1); return o; }\n"
             "fragment half4 fs() { return half4(0,1,0,1); }\n"
            options:nil error:&error];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = lib ? [dev newRenderPipelineStateWithDescriptor:pd error:&error] : nil;
        if (!ps) { fprintf(stderr, "pipeline: %s\n", error.description.UTF8String); return 1; }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLBuffer> seen = [dev newBufferWithLength:16 * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        const uint32_t indices[] = {1, 2, 3};
        id<MTLBuffer> ib = [dev newBufferWithBytes:indices length:sizeof(indices) options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:16 height:16 mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [dev newTextureWithDescriptor:td];
        if (!queue || !seen || !ib || !texture) { fprintf(stderr, "allocation failed\n"); return 1; }
        io_connect_t poison = argc == 2 ? openPoisonClient() : IO_OBJECT_NULL;
        if (argc == 2 && !poison) { fprintf(stderr, "poison requires a ready NVGspControl GPU\n"); return 1; }
        int failures = 0;
        // Warm up the pipeline, then check arrays at 0/1 and indexed base -1.
        for (int mode = 0; mode < 4; ++mode) {
            uint32_t *values = seen.contents;
            for (int i = 0; i < 16; ++i) values[i] = 0xabcdef01u;
            const uint32_t first = mode == 2 ? 1 : 0;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
            [re setRenderPipelineState:ps];
            [re setVertexBuffer:seen offset:0 atIndex:0];
            if (mode == 3) {
                [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt32
                              indexBuffer:ib indexBufferOffset:0 instanceCount:1 baseVertex:-1 baseInstance:0];
            } else {
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:first vertexCount:3];
            }
            [re endEncoding];
            if (poison && mode > 0) {
                kern_return_t kr = setVertexIdBase(poison, 0xffffffffu);
                if (kr) { fprintf(stderr, "poison failed: 0x%x\n", kr); failures++; break; }
            }
            [cb commit]; [cb waitUntilCompleted];
            int bad = 0;
            for (uint32_t i = 0; i < 16; ++i) {
                uint32_t expected = i >= first && i < first + 3 ? i : 0xabcdef01u;
                if (values[i] != expected) {
                    printf("  slot %u = 0x%x, want 0x%x\n", i, values[i], expected);
                    bad++;
                }
            }
            BOOL ok = !bad && cb.status == MTLCommandBufferStatusCompleted;
            printf("mode %d (%s, first=%u): status=%ld wrong=%d %s\n", mode,
                   mode == 3 ? "indexed base=-1" : "arrays", first, (long)cb.status, bad, ok ? "PASS" : "FAIL");
            failures += !ok;
        }
        if (poison) {
            kern_return_t kr = setVertexIdBase(poison, 0);
            if (kr) { fprintf(stderr, "restore failed: 0x%x\n", kr); failures++; }
            IOServiceClose(poison);
        }
        printf("metal_vertex_id_state_test on %s (%s): %s\n", dev.name.UTF8String,
               poison ? "poison" : "reference", failures ? "FAIL" : "PASS");
        return failures != 0;
    }
}
