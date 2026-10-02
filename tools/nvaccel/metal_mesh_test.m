// Mesh shader check (Metal 3): an object stage launches 2 mesh groups, each mesh group writes one coloured
// quad (4 vertices, 2 triangles) into a 64x64 target. Pixels are compared with the M1 (reference dump) or,
// without one, checked against the expected quad colours.
//   metal_mesh_test [ref.bin]      MESH_DUMP=out.bin writes this run's pixels
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *src = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float4 col; };\n"
"struct Payload { float x0[2]; };\n"
"[[object]] void objMain(object_data Payload &p [[payload]], mesh_grid_properties g,\n"
"                        uint tid [[thread_index_in_threadgroup]]) {\n"
"    if (tid < 2) p.x0[tid] = -0.9f + 0.95f * tid;\n"
"    if (tid == 0) g.set_threadgroups_per_grid(uint3(2, 1, 1));\n"
"}\n"
"using M = metal::mesh<VOut, void, 4, 2, topology::triangle>;\n"
"[[mesh]] void meshMain(M m, const object_data Payload &p [[payload]],\n"
"                       uint tid [[thread_index_in_threadgroup]], uint gid [[threadgroup_position_in_grid]]) {\n"
"    float x0 = p.x0[gid];\n"
"    if (tid < 4) {\n"
"        VOut v;\n"
"        float x = x0 + ((tid & 1) ? 0.8f : 0.0f), y = (tid & 2) ? 0.8f : -0.8f;\n"
"        v.pos = float4(x, y, 0, 1);\n"
"        v.col = gid == 0 ? float4(1, 0, 0, 1) : float4(0, float(tid) / 3.0f, 1, 1);\n"
"        m.set_vertex(tid, v);\n"
"    }\n"
"    if (tid < 6) m.set_index(tid, (uint[6]){0, 1, 2, 2, 1, 3}[tid]);\n"
"    if (tid == 0) m.set_primitive_count(2);\n"
"}\n"
"fragment float4 fragMain(VOut in [[stage_in]]) { return in.col; }\n";

int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("%s Metal3=%d\n", dev.name.UTF8String, [dev supportsFamily:MTLGPUFamilyMetal3]);
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) { printf("metal_mesh_test: FAIL library: %s\n", err.description.UTF8String); return 1; }
        MTLMeshRenderPipelineDescriptor *pd = [MTLMeshRenderPipelineDescriptor new];
        pd.objectFunction = [lib newFunctionWithName:@"objMain"];
        pd.meshFunction = [lib newFunctionWithName:@"meshMain"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fragMain"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithMeshDescriptor:pd options:0 reflection:nil error:&err];
        if (!ps) { printf("metal_mesh_test: FAIL pipeline: %s\n", err.description.UTF8String); return 1; }
        const int W = 64, H = 64;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:ps];
        [re drawMeshThreadgroups:MTLSizeMake(1, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(32, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
        [re endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be synchronizeResource:rt];
        [be endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) {
            printf("metal_mesh_test: FAIL status %ld %s\n", (long)cb.status, cb.error.description.UTF8String); return 1;
        }
        uint32_t *px = calloc(W * H, 4);
        [rt getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        const char *dump = getenv("MESH_DUMP");
        if (dump) { FILE *f = fopen(dump, "wb"); fwrite(px, 4, W * H, f); fclose(f); }
        int red = 0, blue = 0, black = 0;
        for (int i = 0; i < W * H; i++) {
            const uint32_t c = px[i];
            if (c == 0xff0000ffu) red++;
            else if ((c & 0xff0000ffu) == 0xff000000u && (c & 0x00ff0000u)) blue++;
            else if (c == 0xff000000u) black++;
        }
        printf("red %d blue %d black %d other %d\n", red, blue, black, W * H - red - blue - black);
        int bad = -1;
        if (argc > 1) {
            FILE *f = fopen(argv[1], "rb");
            uint32_t *ref = calloc(W * H, 4);
            if (f && fread(ref, 4, W * H, f) == (size_t)(W * H)) {
                bad = 0;
                for (int i = 0; i < W * H; i++) {
                    for (int k = 0; k < 32; k += 8) {
                        int d = (int)((px[i] >> k) & 0xff) - (int)((ref[i] >> k) & 0xff);
                        if (d > 1 || d < -1) { bad++; break; }
                    }
                }
            }
            if (f) fclose(f);
        }
        const bool ok = bad >= 0 ? bad == 0 : (red > 300 && blue > 300);
        printf("metal_mesh_test: %s (%d px differ from ref)\n", ok ? "PASS" : "FAIL", bad);
        return ok ? 0 : 1;
    }
}
