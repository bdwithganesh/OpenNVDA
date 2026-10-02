// metal_va0_test: 0.8.30 null-VA fallbacks (T4 Memoji VA-0 fault class).
// The TES patch-ID list and the index buffer reached the GPU as a literal VA 0
// when their VA lookup failed (memory pressure); VA 0 sits below the VAS base
// so even sparse VA does not cover it -> MMU fault. 0.8.30 feeds the TES list
// from the null page and skips a draw whose index VA is missing.
// Modes: "all" (default) = idx + tess on the normal path;
//        "forced" = idxforced + tessforced with NVMTL_FORCE_NULLVA=1, which makes
//        the driver run the fallback paths deterministically.
// Honest scope: a regression guard for the touched branches, not a repro of the
// live Memoji fault (that needs memory pressure + AvatarPickerMemojiPicker).
// On Apple's driver the hook is inert, so forced modes expect normal output.
// Build: clang -fobjc-arc -framework Metal -framework SceneKit -framework AppKit metal_va0_test.m -o metal_va0_test
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <SceneKit/SceneKit.h>
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "vertex float4 vtri(uint vid [[vertex_id]]) {\n"
     "  float2 p[3] = {float2(-0.9, -0.9), float2(0.9, -0.9), float2(-0.9, 0.9)}; return float4(p[vid], 0, 1); }\n"
     "fragment float4 forange() { return float4(1.0, 0.5, 0.0, 1.0); }\n";

// Indexed triangle to a 64x64 target. forced=YES (our driver + hook env):
// the draw is skipped (the skip returns before clears too, so the target is
// left undefined, not cleared), the buffer completes, nothing is drawn.
static int runIdx(id<MTLDevice> dev, BOOL forced) {
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
    MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction = [lib newFunctionWithName:@"vtri"];
    rd.fragmentFunction = [lib newFunctionWithName:@"forange"];
    rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
    if (!ps) { printf("  idx%s: no pipeline (%s)\n", forced ? "forced" : "", e.localizedDescription.UTF8String); return 1; }
    const NSUInteger W = 64, H = 64;
    MTLTextureDescriptor *ct = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:W height:H mipmapped:NO];
    ct.usage = MTLTextureUsageRenderTarget; ct.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:ct];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.12, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    uint16_t idx[3] = {0, 1, 2};
    id<MTLBuffer> ib = [dev newBufferWithBytes:idx length:sizeof idx options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    [re setRenderPipelineState:ps];
    [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16
                  indexBuffer:ib indexBufferOffset:0];
    [re endEncoding];
    id<MTLBuffer> rb = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
              toBuffer:rb destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [be endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint8_t *px = rb.contents;
    int orange = 0, clear = 0;
    for (NSUInteger i = 0; i < W * H; i++) {
        const uint8_t b = px[4 * i], g = px[4 * i + 1], r = px[4 * i + 2], a = px[4 * i + 3];
        if (r > 200 && g > 90 && g < 170 && b < 60 && a == 255) orange++;
        else if (abs((int)b - 26) <= 2 && abs((int)g - 26) <= 2 && abs((int)r - 31) <= 2 && a == 255) clear++;
    }
    const BOOL ok = forced ? (cb.status == MTLCommandBufferStatusCompleted && orange == 0)
                            : (cb.status == MTLCommandBufferStatusCompleted && orange > 500);
    printf("  idx%s: status %ld, orange %d, clear %d: %s\n", forced ? "forced" : "", (long)cb.status, orange, clear,
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// One SceneKit box with a hardware tessellator (Memoji-like), offscreen.
// forced=YES: patch-list VA fails on purpose; the frame must still complete
// (pixels unchecked: every patch reads id 0 from the null page).
// checkPixels=NO on NVIDIA: TES completes but draws black there (pre-existing
// gap, 1 Oct 2026: M1 non-bg 5539, RTX 0 with identical completion) — pixels
// are an Apple-driver assertion until the TES-output gap is fixed.
static int runTess(id<MTLDevice> dev, BOOL forced, BOOL checkPixels) {
    const NSUInteger W = 256, H = 256;
    SCNScene *scene = [SCNScene scene];
    SCNNode *camNode = [SCNNode node];
    camNode.camera = [SCNCamera camera];
    camNode.position = SCNVector3Make(0, 0, 6);
    [scene.rootNode addChildNode:camNode];
    SCNNode *light = [SCNNode node];
    light.light = [SCNLight light];
    light.light.type = SCNLightTypeOmni;
    light.position = SCNVector3Make(3, 3, 6);
    [scene.rootNode addChildNode:light];
    SCNGeometry *g = [SCNBox boxWithWidth:1.6 height:1.6 length:1.6 chamferRadius:0.1];
    SCNGeometryTessellator *t = [SCNGeometryTessellator new];
    t.edgeTessellationFactor = 6; t.insideTessellationFactor = 6;
    t.smoothingMode = SCNTessellationSmoothingModePNTriangles;
    g.tessellator = t;
    g.firstMaterial.diffuse.contents = [NSColor systemTealColor];
    SCNNode *n = [SCNNode nodeWithGeometry:g];
    n.eulerAngles = SCNVector3Make(0.5, 0.7, 0);
    [scene.rootNode addChildNode:n];
    SCNRenderer *r = [SCNRenderer rendererWithDevice:dev options:nil];
    r.scene = scene;
    r.pointOfView = camNode;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeManaged;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.12, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> last = nil;
    for (int f = 0; f < 2; f++) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        [r renderAtTime:f / 60.0 viewport:CGRectMake(0, 0, W, H) commandBuffer:cb passDescriptor:rp];
        if (f == 1) { id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder]; [b synchronizeResource:tex]; [b endEncoding]; }
        [cb commit];
        last = cb;
    }
    [last waitUntilCompleted];
    int nonBg = 0;
    if (last.status == MTLCommandBufferStatusCompleted) {
        uint8_t *px = malloc(W * H * 4);
        [tex getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        for (NSUInteger i = 0; i < W * H; i++)
            nonBg += abs((int)px[4 * i] - 26) > 8 || abs((int)px[4 * i + 1] - 26) > 8 || abs((int)px[4 * i + 2] - 31) > 8;
        free(px);
    }
    const BOOL ok = (forced || !checkPixels) ? last.status == MTLCommandBufferStatusCompleted
                                               : (last.status == MTLCommandBufferStatusCompleted && nonBg > 1000);
    printf("  tess%s: status %ld, non-bg %d: %s\n", forced ? "forced" : "", (long)last.status, nonBg, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("metal_va0_test: no device: FAIL\n"); return 1; }
        const BOOL nvidia = [dev.name containsString:@"NVIDIA"];
        const BOOL hook = getenv("NVMTL_FORCE_NULLVA") != NULL;
        const char *mode = argc > 1 ? argv[1] : "all";
        int bad = 0;
        if (!strcmp(mode, "forced")) {
            if (!hook) { printf("metal_va0_test forced: NVMTL_FORCE_NULLVA not set: FAIL\n"); return 1; }
            if (!nvidia) printf("  (hook inert on %s, expecting normal output)\n", dev.name.UTF8String);
            bad += runIdx(dev, nvidia);
            bad += runTess(dev, nvidia, !nvidia);
        } else {
            bad += runIdx(dev, NO);
            bad += runTess(dev, NO, !nvidia);
        }
        printf("metal_va0_test %s on %s: %s\n", mode, dev.name.UTF8String, bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
