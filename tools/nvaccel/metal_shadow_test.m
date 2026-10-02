// metal_shadow_test: SkyLight's window shadow blur on its own, the draw that
// hung GR right after login (28 Sep). Same pipeline, same arguments as the
// WindowServer fault dump: SimpleVertexShadow + ShadowHorizontalBlurFragment
// into a 209x209 RGBA8 target, fargs {rim 3, inner rim 2, offset 8, ...}.
//   metal_shadow_test [fragment] [name=0|1 ...]
// Function constants default to what the dump showed (is_horizontal,
// is_rim_state_soft, is_rim_state_inner, is_rim_radius_eq_3); name=0/1 on
// the command line changes one. Prints PASS, or HANG when the command
// buffer is still out after 5 s (then the GPU needs a reboot).
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) if ([d.name containsString:@"NVIDIA"] || [d.name containsString:@"RTX"]) dev = d;
        if (!dev) { printf("no NVIDIA MTLDevice\n"); return 1; }
        NSError *err = nil;
        NSURL *u = [NSURL fileURLWithPath:@"/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/SkyLightShaders.air64.metallib"];
        id<MTLLibrary> lib = [dev newLibraryWithURL:u error:&err];
        if (!lib) { printf("library: %s\n", err.description.UTF8String); return 1; }
        NSString *fname = @"ShadowHorizontalBlurFragment";
        NSMutableDictionary<NSString *, NSNumber *> *fc = [@{@"is_horizontal": @1, @"is_rim_state_soft": @1,
            @"is_rim_state_inner": @1, @"is_rim_radius_eq_3": @1} mutableCopy];
        for (int i = 1; i < argc; i++) {
            const char *eq = strchr(argv[i], '=');
            if (!eq) { fname = @(argv[i]); continue; }
            fc[[[NSString alloc] initWithBytes:argv[i] length:eq - argv[i] encoding:NSUTF8StringEncoding]] = @(atoi(eq + 1));
        }
        MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
        for (NSString *k in fc) { bool b = fc[k].intValue != 0; [cv setConstantValue:&b type:MTLDataTypeBool withName:k]; }
        id<MTLFunction> vf = [lib newFunctionWithName:@"SimpleVertexShadow"];
        id<MTLFunction> ff = [lib newFunctionWithName:fname constantValues:cv error:&err];
        if (!vf || !ff) { printf("functions: %s\n", err.description.UTF8String); return 1; }
        MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
        vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
        vd.layouts[0].stride = 16;
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = vf; rd.fragmentFunction = ff; rd.vertexDescriptor = vd;
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!ps) { printf("pipeline: %s\n", err.description.UTF8String); return 1; }

        // SHADOW_PRIV=1: source and target in VRAM (private), as WindowServer
        // has them; SHADOW_SRC / SHADOW_RT sizes (dump: 273 / 497), SHADOW_OFF
        // the tex coord offset (dump: 36)
        const int priv = getenv("SHADOW_PRIV") ? atoi(getenv("SHADOW_PRIV")) : 0;
        const NSUInteger S = getenv("SHADOW_SRC") ? (NSUInteger)atoi(getenv("SHADOW_SRC")) : 209;
        const NSUInteger N = getenv("SHADOW_RT") ? (NSUInteger)atoi(getenv("SHADOW_RT")) : 209;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:N height:N mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        if (priv) td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        MTLTextureDescriptor *sd = [td copy]; sd.width = S; sd.height = S;
        MTLTextureDescriptor *up = [sd copy]; up.storageMode = MTLStorageModeShared;
        id<MTLTexture> src = [dev newTextureWithDescriptor:sd], stage = [dev newTextureWithDescriptor:up];
        uint32_t *px = calloc(N * N > S * S ? N * N : S * S, 4);
        for (NSUInteger i = 0; i < S * S; i++) px[i] = ((i % S) > 20 && (i % S) < S - 20 && (i / S) > 20 && (i / S) < S - 20) ? 0xff000000u : 0;
        [stage replaceRegion:MTLRegionMake2D(0, 0, S, S) mipmapLevel:0 withBytes:px bytesPerRow:S * 4];
        id<MTLCommandQueue> q0 = [dev newCommandQueue];
        id<MTLCommandBuffer> ub = [q0 commandBuffer];
        id<MTLBlitCommandEncoder> be = [ub blitCommandEncoder];
        [be copyFromTexture:stage toTexture:src];
        [be endEncoding]; [ub commit]; [ub waitUntilCompleted];
        const float quad[] = {-1, -1, 0, 1,  1, -1, 1, 1,  -1, 1, 0, 0,  1, 1, 1, 0};
        const float mvp[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        // as in the dump: rim 3, inner rim 2, tex coord offset 8, densities, rim colours
        const uint32_t off = getenv("SHADOW_OFF") ? (uint32_t)atoi(getenv("SHADOW_OFF")) : 8;
        const uint32_t fargs[16] = {3, 2, off, 0x3e800000, 0x3f0ccccd, 0x3dcccccd, 0, 0,
                                    0, 0, 0, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000};
        id<MTLBuffer> zero = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:ps];
        [e setVertexBytes:quad length:sizeof quad atIndex:0];
        [e setVertexBytes:mvp length:sizeof mvp atIndex:1];
        [e setFragmentTexture:src atIndex:0];
        [e setFragmentBytes:fargs length:sizeof fargs atIndex:1];
        [e setFragmentBuffer:zero offset:0 atIndex:2];
        [e setFragmentBuffer:zero offset:0 atIndex:3];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { dispatch_semaphore_signal(done); }];
        [cb commit];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
            printf("metal_shadow_test %s: HANG (command buffer out after 5 s)\n", fname.UTF8String);
            return 3;
        }
        if (!priv) [rt getBytes:px bytesPerRow:N * 4 fromRegion:MTLRegionMake2D(0, 0, N, N) mipmapLevel:0];
        printf("metal_shadow_test %s: %s status %ld, centre %08x edge %08x corner %08x\n", fname.UTF8String,
               cb.status == MTLCommandBufferStatusCompleted ? "PASS" : "FAIL", (long)cb.status,
               px[N / 2 * N + N / 2], px[N / 2 * N + 21], px[0]);
        return cb.status == MTLCommandBufferStatusCompleted ? 0 : 1;
    }
}
