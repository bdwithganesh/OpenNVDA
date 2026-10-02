// metal_present: Metal render -> physical screen (B4 M7 slice).
// Renders a red-on-blue triangle via our Metal driver at 512x512 BGRA8,
// verifies the pixels, composes centered onto a 3840x2160 A8R8G8B8 frame,
// writes it to a VRAM object through NVGspControl PRAMIN (selector 11),
// presents on window 0 (selector 27, vsync-paced re-presents), spot-checks
// the VRAM bytes back (selector 11 read), then hands back (selector 28).
// The flip machinery is the proven present path (TEST-BATCH vkpresent);
// the rendered pixels are verified before compose, so photons-on-screen
// is the only step needing eyes.
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static double now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1e6 + tv.tv_usec;
}

static id<MTLDevice> findNV(void) {
    for (id<MTLDevice> d in MTLCopyAllDevices())
        if ([d.name containsString:@"RTX"]) return d;
    return nil;
}

#define DW 3840
#define DH 2160
#define DPITCH (DW * 4)
#define RW 512
#define RH 512

int main(void) {
    @autoreleasepool {
        // 1. Metal render at 512x512 BGRA8.
        id<MTLDevice> dev = findNV();
        if (!dev) { printf("no RTX Metal device\n"); return 1; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"vertex float4 vmain(device const float2 *pos [[buffer(0)]], uint vid [[vertex_id]]) {"
            @" return float4(pos[vid].x, pos[vid].y, 0.0, 1.0); }"
            @" fragment float4 fmain() { return float4(1.0, 0.0, 0.0, 1.0); }"
                                              options:nil error:&err];
        if (!lib) { printf("library failed %s\n", err.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vmain"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!rps) { printf("pipeline failed %s\n", err.description.UTF8String); return 1; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:RW height:RH mipmapped:NO];
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        float verts[6] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
        id<MTLBuffer> vb = [dev newBufferWithBytes:verts length:sizeof verts options:MTLResourceStorageModeShared];
        if (!rt || !vb) { printf("texture/vbuf failed\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
        pd.colorAttachments[0].texture = rt;
        pd.colorAttachments[0].loadAction = MTLLoadActionClear;
        pd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
        pd.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pd];
        [re setRenderPipelineState:rps];
        [re setVertexBuffer:vb offset:0 atIndex:0];
        [re setViewport:(MTLViewport){0, 0, RW, RH, 0, 1}];
        [re setScissorRect:(MTLScissorRect){0, 0, RW, RH}];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        static uint32_t rp[RW * RH];
        [rt getBytes:rp bytesPerRow:RW * 4 fromRegion:MTLRegionMake2D(0, 0, RW, RH) mipmapLevel:0];
        int red = 0;
        for (int i = 0; i < RW * RH; i++) red += rp[i] == 0xFFFF0000u;
        printf("render: red=%d (want ~32768) center=%s corner=%s\n", red,
               rp[(RH / 2) * RW + RW / 2] == 0xFFFF0000u ? "red" : "WRONG",
               rp[0] == 0xFF0000FFu ? "blue" : "WRONG");
        if (red < 30000 || red > 36000 || rp[(RH / 2) * RW + RW / 2] != 0xFFFF0000u || rp[0] != 0xFF0000FFu) {
            printf("render FAIL\n");
            return 1;
        }
        // 2. Compose centered onto the 4K frame (opaque black letterbox).
        static uint32_t frame[DW * DH];
        for (int i = 0; i < DW * DH; i++) frame[i] = 0xFF000000u;
        const int ox = (DW - RW) / 2, oy = (DH - RH) / 2;
        for (int y = 0; y < RH; y++)
            memcpy(frame + (oy + y) * DW + ox, rp + y * RW, RW * 4);
        // 3. VRAM object + PRAMIN upload (mirror of nvrun present_obj_test).
        io_service_t sv = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVGspControl"));
        if (!sv) { printf("no NVGspControl\n"); return 1; }
        io_connect_t conn = IO_OBJECT_NULL;
        if (IOServiceOpen(sv, mach_task_self(), 0, &conn)) { printf("UC open failed\n"); return 1; }
        IOObjectRelease(sv);
        uint64_t in[2] = {36ull << 20, 0}, out[2];
        uint32_t cnt = 2;
        if (IOConnectCallMethod(conn, 22, in, 2, NULL, 0, out, &cnt, NULL, NULL)) {
            printf("VRAM alloc failed\n");
            return 1;
        }
        const uint64_t h = out[0], phys = out[1];
        printf("vram obj h=%llu phys=0x%llx, uploading %u bytes...\n", h, phys, DW * DH * 4);
        const double t0 = now_us();
        for (uint32_t y = 0; y < DH; y++) {
            for (uint32_t c = 0; c < DW;) {
                const uint32_t n = DW - c > 1024 ? 1024 : DW - c;
                uint64_t v[3] = {phys + (uint64_t)y * DPITCH + c * 4, n, 1};
                if (IOConnectCallMethod(conn, 11, v, 3, frame + y * DW + c, n * 4, NULL, NULL, NULL, NULL)) {
                    printf("vram write failed at row %u+%u\n", y, c);
                    return 1;
                }
                c += n;
            }
        }
        printf("upload %.1f MB/s\n", (DW * DH * 4 / 1e6) / ((now_us() - t0) / 1e6));
        // 4. Spot-check the VRAM bytes back (independent oracle: PRAMIN read).
        uint32_t chk[DW];
        size_t outsz;
        int bad = 0;
        for (int pass = 0; pass < 2; pass++) {
            const uint32_t y = pass ? (uint32_t)(oy + RH / 2) : 0;
            for (uint32_t c = 0; c < DW;) {
                const uint32_t n = DW - c > 1024 ? 1024 : DW - c;
                uint64_t v[3] = {phys + (uint64_t)y * DPITCH + c * 4, n, 0};
                outsz = n * 4;
                if (IOConnectCallMethod(conn, 11, v, 3, NULL, 0, NULL, NULL, chk + c, &outsz) || outsz != n * 4) {
                    printf("vram read failed at row %u+%u\n", y, c);
                    return 1;
                }
                c += n;
            }
            for (uint32_t x = 0; x < DW; x++) bad += chk[x] != frame[y * DW + x];
        }
        printf("vram readback: %s\n", bad ? "FAIL" : "2 rows match");
        if (bad) return 1;
        // 5. Present ~4 s, vsync-paced.
        uint64_t good[6] = {h, 0, DPITCH, DW, DH, 0xCF};
        if (IOConnectCallMethod(conn, 27, good, 6, NULL, 0, NULL, NULL, NULL, NULL)) {
            printf("present failed\n");
            return 1;
        }
        printf("PRESENTING 4 s -- look at the physical screen (red triangle on blue, centered)\n");
        const double tp = now_us();
        uint32_t flips = 0;
        while (now_us() - tp < 4e6) {
            if (IOConnectCallMethod(conn, 27, good, 6, NULL, 0, NULL, NULL, NULL, NULL)) break;
            flips++;
        }
        printf("re-presented %u times in %.1f s (%.1f flips/s)\n", flips, (now_us() - tp) / 1e6,
               flips / ((now_us() - tp) / 1e6));
        int rc = 0;
        if (IOConnectCallMethod(conn, 28, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL)) {
            printf("present stop FAILED\n");
            rc = 1;
        }
        if (IOConnectCallMethod(conn, 23, &out[0], 1, NULL, 0, NULL, NULL, NULL, NULL)) {
            printf("free FAILED\n");
            rc = 1;
        }
        IOServiceClose(conn);
        printf("metal_present: %s\n", rc ? "FAIL" : "PASS (photons need eyes)");
        return rc;
    }
}
