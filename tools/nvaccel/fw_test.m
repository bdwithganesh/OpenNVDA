// Apple frameworks on the default Metal device, each checked against a CPU
// reference, or saved as raw RGBA so the same run on another Mac (the M1)
// can be compared pixel by pixel (fw_compare.py).
//   fw_test <out dir>
// Covers: Core Image (Metal vs software renderer), MPS matrix multiply and
// Gaussian blur, MPSGraph (matmul + bias + ReLU + softmax, conv2d), SceneKit
// and SpriteKit offscreen renderers, CoreVideo Metal texture cache (BGRA and
// 4:2:0 biplanar), Core Animation's CARenderer into a Metal texture.
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>
#import <CoreImage/CoreImage.h>
#import <SceneKit/SceneKit.h>
#import <SpriteKit/SpriteKit.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include <dlfcn.h>

static id<MTLDevice> dev;
static id<MTLCommandQueue> q;
static NSString *outDir;
static int fails;

static void result(const char *name, BOOL ok, NSString *detail) {
    printf("  %-30s %-60s %s\n", name, detail.UTF8String, ok ? "PASS" : "FAIL");
    fflush(stdout);
    if (!ok) fails++;
}

static NSData *readTex(id<MTLTexture> t) {
    const NSUInteger w = t.width, h = t.height;
    id<MTLBuffer> b = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
               toBuffer:b destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [be endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return [NSData dataWithBytes:b.contents length:w * h * 4];
}

static void save(NSString *name, NSData *px, NSUInteger w, NSUInteger h, const char *fmt) {
    NSMutableData *d = [[NSString stringWithFormat:@"%lu %lu %s\n", (unsigned long)w, (unsigned long)h, fmt]
                           dataUsingEncoding:NSASCIIStringEncoding].mutableCopy;
    [d appendData:px];
    [d writeToFile:[outDir stringByAppendingPathComponent:[name stringByAppendingString:@".rgba"]] atomically:YES];
}

static id<MTLTexture> renderTarget(NSUInteger w, NSUInteger h, MTLPixelFormat f) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:td];
}

// ---------------------------------------------------------------- Core Image
static void testCoreImage(void) {
    CIImage *img = [[CIFilter filterWithName:@"CICheckerboardGenerator" withInputParameters:@{
        @"inputCenter": [CIVector vectorWithX:0 Y:0], @"inputColor0": [CIColor colorWithRed:0.9 green:0.2 blue:0.1],
        @"inputColor1": [CIColor colorWithRed:0.1 green:0.3 blue:0.8], @"inputWidth": @16, @"inputSharpness": @1}] outputImage];
    img = [img imageByApplyingFilter:@"CIGaussianBlur" withInputParameters:@{@"inputRadius": @4}];
    img = [img imageByApplyingFilter:@"CIColorControls" withInputParameters:@{@"inputSaturation": @1.4,
                                                                                @"inputBrightness": @0.05, @"inputContrast": @1.1}];
    img = [img imageByApplyingFilter:@"CISepiaTone" withInputParameters:@{@"inputIntensity": @0.3}];
    img = [img imageByApplyingFilter:@"CIVignette" withInputParameters:@{@"inputRadius": @1.5, @"inputIntensity": @0.8}];
    const CGRect r = CGRectMake(0, 0, 128, 128);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    NSMutableData *g = [NSMutableData dataWithLength:128 * 128 * 4], *c = [NSMutableData dataWithLength:128 * 128 * 4];
    CIContext *gpu = [CIContext contextWithMTLDevice:dev];
    CIContext *cpu = [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer: @YES}];
    [gpu render:img toBitmap:g.mutableBytes rowBytes:512 bounds:r format:kCIFormatRGBA8 colorSpace:cs];
    [cpu render:img toBitmap:c.mutableBytes rowBytes:512 bounds:r format:kCIFormatRGBA8 colorSpace:cs];
    CGColorSpaceRelease(cs);
    const uint8_t *a = g.bytes, *b = c.bytes;
    int maxd = 0;
    double sum = 0;
    for (int i = 0; i < 128 * 128 * 4; i++) { const int d = abs(a[i] - b[i]); if (d > maxd) maxd = d; sum += d; }
    save(@"coreimage", g, 128, 128, "rgba8");
    result("Core Image vs software", maxd <= 12 && sum / (128 * 128 * 4) < 1.5,
           [NSString stringWithFormat:@"max diff %d, mean %.3f (gpu px0 %d %d %d)", maxd, sum / (128 * 128 * 4), a[0], a[1], a[2]]);
}

// ---------------------------------------------------------------- MPS
static void testMPSMatrix(void) {
    const NSUInteger M = 128, K = 64, N = 96;
    float *A = malloc(M * K * 4), *B = malloc(K * N * 4), *R = calloc(M * N, 4);
    for (NSUInteger i = 0; i < M * K; i++) A[i] = (float)((i * 37) % 17) / 8.0f - 1.0f;
    for (NSUInteger i = 0; i < K * N; i++) B[i] = (float)((i * 11) % 13) / 6.0f - 1.0f;
    for (NSUInteger i = 0; i < M; i++)
        for (NSUInteger j = 0; j < N; j++) {
            float s = 0;
            for (NSUInteger k = 0; k < K; k++) s += A[i * K + k] * B[k * N + j];
            R[i * N + j] = s;
        }
    id<MTLBuffer> ba = [dev newBufferWithBytes:A length:M * K * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bb = [dev newBufferWithBytes:B length:K * N * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bc = [dev newBufferWithLength:M * N * 4 options:MTLResourceStorageModeShared];
    MPSMatrix *ma = [[MPSMatrix alloc] initWithBuffer:ba descriptor:[MPSMatrixDescriptor matrixDescriptorWithRows:M columns:K
                                                                     rowBytes:K * 4 dataType:MPSDataTypeFloat32]];
    MPSMatrix *mb = [[MPSMatrix alloc] initWithBuffer:bb descriptor:[MPSMatrixDescriptor matrixDescriptorWithRows:K columns:N
                                                                     rowBytes:N * 4 dataType:MPSDataTypeFloat32]];
    MPSMatrix *mc = [[MPSMatrix alloc] initWithBuffer:bc descriptor:[MPSMatrixDescriptor matrixDescriptorWithRows:M columns:N
                                                                     rowBytes:N * 4 dataType:MPSDataTypeFloat32]];
    MPSMatrixMultiplication *mm = [[MPSMatrixMultiplication alloc] initWithDevice:dev resultRows:M resultColumns:N
                                                                  interiorColumns:K];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    [mm encodeToCommandBuffer:cb leftMatrix:ma rightMatrix:mb resultMatrix:mc];
    [cb commit];
    [cb waitUntilCompleted];
    const float *o = bc.contents;
    double maxe = 0;
    for (NSUInteger i = 0; i < M * N; i++) maxe = fmax(maxe, fabs(o[i] - R[i]));
    result("MPS matrix multiply 128x64x96", maxe < 1e-3, [NSString stringWithFormat:@"max error %.2g, status %ld", maxe, (long)cb.status]);
    free(A); free(B); free(R);
}

static void testMPSBlur(void) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                  width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> src = [dev newTextureWithDescriptor:td], dst = [dev newTextureWithDescriptor:td];
    float *px = calloc(64 * 64 * 4, 4);
    px[(32 * 64 + 32) * 4] = 1.0f;
    [src replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:px bytesPerRow:64 * 16];
    MPSImageGaussianBlur *bl = [[MPSImageGaussianBlur alloc] initWithDevice:dev sigma:2.0f];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    [bl encodeToCommandBuffer:cb sourceTexture:src destinationTexture:dst];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    [dst getBytes:px bytesPerRow:64 * 16 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
    double sum = 0, asym = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) sum += px[(y * 64 + x) * 4];
    for (int d = 1; d < 8; d++) asym = fmax(asym, fabs(px[(32 * 64 + 32 + d) * 4] - px[(32 * 64 + 32 - d) * 4]));
    const float peak = px[(32 * 64 + 32) * 4], want = 1.0f / (2 * M_PI * 4.0f);
    result("MPS Gaussian blur (sigma 2)", fabs(sum - 1) < 0.02 && asym < 1e-3 && fabs(peak - want) < 0.01,
           [NSString stringWithFormat:@"sum %.4f, peak %.4f (ideal %.4f), asym %.2g", sum, peak, want, asym]);
    free(px);
}

// ---------------------------------------------------------------- MPSGraph
static void testMPSGraph(void) {
    const int N = 64;
    MPSGraph *g = [MPSGraph new];
    MPSGraphTensor *a = [g placeholderWithShape:@[@(N), @(N)] dataType:MPSDataTypeFloat32 name:@"a"];
    MPSGraphTensor *b = [g placeholderWithShape:@[@(N), @(N)] dataType:MPSDataTypeFloat32 name:@"b"];
    MPSGraphTensor *bias = [g constantWithScalar:0.25 shape:@[@1, @(N)] dataType:MPSDataTypeFloat32];
    MPSGraphTensor *mm = [g matrixMultiplicationWithPrimaryTensor:a secondaryTensor:b name:nil];
    MPSGraphTensor *re = [g reLUWithTensor:[g additionWithPrimaryTensor:mm secondaryTensor:bias name:nil] name:nil];
    MPSGraphTensor *sm = [g softMaxWithTensor:re axis:1 name:nil];
    float *A = malloc(N * N * 4), *B = malloc(N * N * 4), *R = malloc(N * N * 4);
    for (int i = 0; i < N * N; i++) { A[i] = (float)((i * 7) % 11) / 10.0f - 0.5f; B[i] = (float)((i * 5) % 9) / 8.0f - 0.5f; }
    for (int i = 0; i < N; i++) {
        float row[64], mx = -1e30f, s = 0;
        for (int j = 0; j < N; j++) {
            float v = 0.25f;
            for (int k = 0; k < N; k++) v += A[i * N + k] * B[k * N + j];
            row[j] = v > 0 ? v : 0;
            mx = fmaxf(mx, row[j]);
        }
        for (int j = 0; j < N; j++) { row[j] = expf(row[j] - mx); s += row[j]; }
        for (int j = 0; j < N; j++) R[i * N + j] = row[j] / s;
    }
    MPSGraphDevice *gd = [MPSGraphDevice deviceWithMTLDevice:dev];
    MPSGraphTensorData *ta = [[MPSGraphTensorData alloc] initWithDevice:gd data:[NSData dataWithBytes:A length:N * N * 4]
                                                                   shape:@[@(N), @(N)] dataType:MPSDataTypeFloat32];
    MPSGraphTensorData *tb = [[MPSGraphTensorData alloc] initWithDevice:gd data:[NSData dataWithBytes:B length:N * N * 4]
                                                                   shape:@[@(N), @(N)] dataType:MPSDataTypeFloat32];
    NSDictionary *res = [g runWithMTLCommandQueue:q feeds:@{a: ta, b: tb} targetTensors:@[sm] targetOperations:nil];
    float *O = calloc(N * N, 4);
    [[res[sm] mpsndarray] readBytes:O strideBytes:nil];
    double maxe = 0;
    for (int i = 0; i < N * N; i++) maxe = fmax(maxe, fabs(O[i] - R[i]));
    result("MPSGraph matmul+ReLU+softmax", maxe < 1e-4, [NSString stringWithFormat:@"max error %.2g (out[0] %.5f)", maxe, O[0]]);
    free(A); free(B); free(R); free(O);

    // conv2d NCHW 1x8x16x16, weights OIHW 4x8x3x3, same padding
    const int C = 8, H = 16, W = 16, O2 = 4;
    MPSGraph *g2 = [MPSGraph new];
    MPSGraphTensor *x = [g2 placeholderWithShape:@[@1, @(C), @(H), @(W)] dataType:MPSDataTypeFloat32 name:@"x"];
    MPSGraphTensor *w = [g2 placeholderWithShape:@[@(O2), @(C), @3, @3] dataType:MPSDataTypeFloat32 name:@"w"];
    MPSGraphConvolution2DOpDescriptor *cd = [MPSGraphConvolution2DOpDescriptor
        descriptorWithStrideInX:1 strideInY:1 dilationRateInX:1 dilationRateInY:1 groups:1 paddingLeft:1 paddingRight:1
                     paddingTop:1 paddingBottom:1 paddingStyle:MPSGraphPaddingStyleExplicit
                     dataLayout:MPSGraphTensorNamedDataLayoutNCHW weightsLayout:MPSGraphTensorNamedDataLayoutOIHW];
    MPSGraphTensor *y = [g2 convolution2DWithSourceTensor:x weightsTensor:w descriptor:cd name:nil];
    float *X = malloc(C * H * W * 4), *Wt = malloc(O2 * C * 9 * 4), *Y = calloc(O2 * H * W, 4), *Yr = calloc(O2 * H * W, 4);
    for (int i = 0; i < C * H * W; i++) X[i] = (float)((i * 13) % 7) / 6.0f - 0.5f;
    for (int i = 0; i < O2 * C * 9; i++) Wt[i] = (float)((i * 3) % 5) / 4.0f - 0.5f;
    for (int o = 0; o < O2; o++)
        for (int yy = 0; yy < H; yy++)
            for (int xx = 0; xx < W; xx++) {
                float s = 0;
                for (int c = 0; c < C; c++)
                    for (int ky = 0; ky < 3; ky++)
                        for (int kx = 0; kx < 3; kx++) {
                            const int iy = yy + ky - 1, ix = xx + kx - 1;
                            if (iy < 0 || ix < 0 || iy >= H || ix >= W) continue;
                            s += X[(c * H + iy) * W + ix] * Wt[((o * C + c) * 3 + ky) * 3 + kx];
                        }
                Yr[(o * H + yy) * W + xx] = s;
            }
    MPSGraphTensorData *tx = [[MPSGraphTensorData alloc] initWithDevice:gd data:[NSData dataWithBytes:X length:C * H * W * 4]
                                                                   shape:@[@1, @(C), @(H), @(W)] dataType:MPSDataTypeFloat32];
    MPSGraphTensorData *tw = [[MPSGraphTensorData alloc] initWithDevice:gd data:[NSData dataWithBytes:Wt length:O2 * C * 36]
                                                                   shape:@[@(O2), @(C), @3, @3] dataType:MPSDataTypeFloat32];
    NSDictionary *r2 = [g2 runWithMTLCommandQueue:q feeds:@{x: tx, w: tw} targetTensors:@[y] targetOperations:nil];
    [[r2[y] mpsndarray] readBytes:Y strideBytes:nil];
    maxe = 0;
    for (int i = 0; i < O2 * H * W; i++) maxe = fmax(maxe, fabs(Y[i] - Yr[i]));
    result("MPSGraph conv2d 3x3", maxe < 1e-3, [NSString stringWithFormat:@"max error %.2g", maxe]);
    free(X); free(Wt); free(Y); free(Yr);
}

// ---------------------------------------------------------------- SceneKit / SpriteKit
static void testSceneKit(void) {
    SCNScene *scene = [SCNScene scene];
    SCNNode *box = [SCNNode nodeWithGeometry:[SCNBox boxWithWidth:1 height:1 length:1 chamferRadius:0.1]];
    box.geometry.firstMaterial.diffuse.contents = [NSColor colorWithRed:0.9 green:0.2 blue:0.1 alpha:1];
    box.eulerAngles = SCNVector3Make(0.5, 0.7, 0);
    [scene.rootNode addChildNode:box];
    SCNNode *cam = [SCNNode node];
    cam.camera = [SCNCamera camera];
    cam.position = SCNVector3Make(0, 0, 3);
    [scene.rootNode addChildNode:cam];
    SCNNode *light = [SCNNode node];
    light.light = [SCNLight light];
    light.light.type = SCNLightTypeOmni;
    light.position = SCNVector3Make(2, 2, 4);
    [scene.rootNode addChildNode:light];
    scene.background.contents = [NSColor colorWithRed:0 green:0 blue:0.2 alpha:1];
    SCNRenderer *r = [SCNRenderer rendererWithDevice:dev options:nil];
    r.scene = scene;
    r.pointOfView = cam;
    id<MTLTexture> t = renderTarget(256, 256, MTLPixelFormatBGRA8Unorm);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.5, 0.5, 1);   // grey: coverage shows even for a black box
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    [r renderAtTime:0 viewport:CGRectMake(0, 0, 256, 256) commandBuffer:cb passDescriptor:rp];
    [cb commit];
    [cb waitUntilCompleted];
    NSData *px = readTex(t);
    const uint8_t *p = px.bytes;
    int boxPx = 0, covered = 0;
    for (int i = 0; i < 256 * 256; i++) {
        if (p[i * 4 + 2] > 60 && p[i * 4 + 2] > p[i * 4] + 20) boxPx++;   // BGRA: red-ish
        if (abs(p[i * 4] - 128) > 3 || abs(p[i * 4 + 1] - 128) > 3 || abs(p[i * 4 + 2] - 128) > 3) covered++;
    }
    save(@"scenekit", px, 256, 256, "bgra8");
    result("SceneKit offscreen render", boxPx > 3000 && boxPx < 40000 && cb.status == MTLCommandBufferStatusCompleted,
           [NSString stringWithFormat:@"%d box px, %d covered, centre %d %d %d", boxPx, covered, p[(128 * 256 + 128) * 4 + 2],
                                      p[(128 * 256 + 128) * 4 + 1], p[(128 * 256 + 128) * 4]]);
}

static void testSpriteKit(void) {
    SKScene *scene = [SKScene sceneWithSize:CGSizeMake(256, 256)];
    scene.backgroundColor = [NSColor colorWithRed:0 green:0.5 blue:0 alpha:1];
    SKSpriteNode *s = [SKSpriteNode spriteNodeWithColor:[NSColor colorWithRed:1 green:0 blue:0 alpha:1] size:CGSizeMake(100, 100)];
    s.position = CGPointMake(128, 128);
    [scene addChild:s];
    SKShapeNode *c = [SKShapeNode shapeNodeWithCircleOfRadius:30];
    c.fillColor = [NSColor blueColor];
    c.position = CGPointMake(40, 40);
    [scene addChild:c];
    SKRenderer *r = [SKRenderer rendererWithDevice:dev];
    r.scene = scene;
    [r updateAtTime:0];
    id<MTLTexture> t = renderTarget(256, 256, MTLPixelFormatBGRA8Unorm);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    [r renderWithViewport:CGRectMake(0, 0, 256, 256) commandBuffer:cb renderPassDescriptor:rp];
    [cb commit];
    [cb waitUntilCompleted];
    NSData *px = readTex(t);
    const uint8_t *p = px.bytes;
    // SKRenderer leaves the scene background to the pass (cleared to black);
    // the circle at scene (40, 40) is texture (40, 216): y points up in the scene
    const uint8_t *mid = p + (128 * 256 + 128) * 4, *circ = p + (216 * 256 + 40) * 4;
    save(@"spritekit", px, 256, 256, "bgra8");
    result("SpriteKit offscreen render", mid[2] > 200 && mid[1] < 40 && circ[0] > 200 && circ[2] < 40,
           [NSString stringWithFormat:@"sprite %d %d %d, circle %d %d %d", mid[2], mid[1], mid[0], circ[2], circ[1], circ[0]]);
}

// ---------------------------------------------------------------- CoreVideo texture cache
static void testCVTextureCache(void) {
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
                     "kernel void rd(texture2d<float, access::read> t [[texture(0)]], device float4 *o [[buffer(0)]],"
                     " uint2 p [[thread_position_in_grid]]) { o[p.y * 64 + p.x] = t.read(p); }\n";
    NSError *e = nil;
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:
        [[dev newLibraryWithSource:src options:nil error:&e] newFunctionWithName:@"rd"] error:&e];
    CVMetalTextureCacheRef cache = NULL;
    CVMetalTextureCacheCreate(NULL, NULL, dev, NULL, &cache);
    NSDictionary *attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES};
    // BGRA
    CVPixelBufferRef pb = NULL;
    CVPixelBufferCreate(NULL, 64, 64, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs, &pb);
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *base = CVPixelBufferGetBaseAddress(pb);
    const size_t bpr = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        uint8_t *q8 = base + y * bpr + x * 4;
        q8[0] = (uint8_t)(x * 4); q8[1] = (uint8_t)(y * 4); q8[2] = 200; q8[3] = 255;
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    CVMetalTextureRef mt = NULL;
    CVMetalTextureCacheCreateTextureFromImage(NULL, cache, pb, NULL, MTLPixelFormatBGRA8Unorm, 64, 64, 0, &mt);
    id<MTLTexture> t = mt ? CVMetalTextureGetTexture(mt) : nil;
    id<MTLBuffer> o = [dev newBufferWithLength:64 * 64 * 16 options:MTLResourceStorageModeShared];
    int wrong = -1;
    if (t && ps) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps];
        [ce setTexture:t atIndex:0];
        [ce setBuffer:o offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(64, 64, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const float *f = o.contents;
        wrong = 0;
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            const float *v = f + (y * 64 + x) * 4;   // read() returns RGBA
            if (fabsf(v[0] - 200 / 255.0f) > 0.01f || fabsf(v[1] - y * 4 / 255.0f) > 0.01f || fabsf(v[2] - x * 4 / 255.0f) > 0.01f) wrong++;
        }
    }
    result("CVMetalTextureCache BGRA", wrong == 0, [NSString stringWithFormat:@"texture %s, %d wrong", t ? "made" : "nil", wrong]);
    if (mt) CFRelease(mt);
    CVPixelBufferRelease(pb);
    // 4:2:0 biplanar (what video decode hands out): Y plane R8, CbCr plane RG8
    pb = NULL;
    CVPixelBufferCreate(NULL, 64, 64, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, (__bridge CFDictionaryRef)attrs, &pb);
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *yp = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *cp = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t ybpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), cbpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) yp[y * ybpr + x] = (uint8_t)(16 + x * 3);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { cp[y * cbpr + x * 2] = (uint8_t)(64 + y); cp[y * cbpr + x * 2 + 1] = (uint8_t)(192 - x); }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    CVMetalTextureRef my = NULL, mc = NULL;
    CVMetalTextureCacheCreateTextureFromImage(NULL, cache, pb, NULL, MTLPixelFormatR8Unorm, 64, 64, 0, &my);
    CVMetalTextureCacheCreateTextureFromImage(NULL, cache, pb, NULL, MTLPixelFormatRG8Unorm, 32, 32, 1, &mc);
    id<MTLTexture> ty = my ? CVMetalTextureGetTexture(my) : nil, tc = mc ? CVMetalTextureGetTexture(mc) : nil;
    int wy = -1, wc = -1;
    if (ty && tc && ps) {
        for (int plane = 0; plane < 2; plane++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            [ce setTexture:plane ? tc : ty atIndex:0];
            [ce setBuffer:o offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(plane ? 32 : 64, plane ? 32 : 64, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const float *f = o.contents;
            int w = 0;
            if (!plane) { for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) if (fabsf(f[(y * 64 + x) * 4] - (16 + x * 3) / 255.0f) > 0.01f) w++; wy = w; }
            else {
                for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
                    const float *v = f + (y * 64 + x) * 4;
                    if (fabsf(v[0] - (64 + y) / 255.0f) > 0.01f || fabsf(v[1] - (192 - x) / 255.0f) > 0.01f) w++;
                }
                wc = w;
            }
        }
    }
    result("CVMetalTextureCache 420v", wy == 0 && wc == 0,
           [NSString stringWithFormat:@"Y %s %d wrong, CbCr %s %d wrong", ty ? "made" : "nil", wy, tc ? "made" : "nil", wc]);
    if (my) CFRelease(my);
    if (mc) CFRelease(mc);
    CVPixelBufferRelease(pb);
    if (cache) CFRelease(cache);
}

// ---------------------------------------------------------------- Core Animation
static void testCARenderer(void) {
    CALayer *root = [CALayer layer];
    root.frame = CGRectMake(0, 0, 256, 256);
    root.backgroundColor = CGColorCreateGenericRGB(1, 1, 1, 1);
    CALayer *card = [CALayer layer];
    card.frame = CGRectMake(60, 60, 120, 100);
    card.backgroundColor = CGColorCreateGenericRGB(0.1, 0.4, 0.9, 1);
    card.cornerRadius = 20;
    card.shadowOpacity = 0.8;
    card.shadowRadius = 12;
    card.shadowOffset = CGSizeMake(0, -6);
    [root addSublayer:card];
    CAGradientLayer *grad = [CAGradientLayer layer];
    grad.frame = CGRectMake(10, 200, 236, 40);
    grad.colors = @[(__bridge id)CGColorCreateGenericRGB(1, 0, 0, 1), (__bridge id)CGColorCreateGenericRGB(0, 1, 0, 1)];
    grad.startPoint = CGPointMake(0, 0.5);
    grad.endPoint = CGPointMake(1, 0.5);
    [root addSublayer:grad];
    id<MTLTexture> t = renderTarget(256, 256, MTLPixelFormatBGRA8Unorm);
    CARenderer *r = [CARenderer rendererWithMTLTexture:t options:nil];
    r.layer = root;
    r.bounds = CGRectMake(0, 0, 256, 256);
    [CATransaction flush];
    [r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
    [r addUpdateRect:r.bounds];
    [r render];
    [r endFrame];
    NSData *px = readTex(t);
    const uint8_t *p = px.bytes;
    // card centre blue, shadow darkens below the card, corners outside stay white
    const uint8_t *cc = p + ((256 - 110) * 256 + 120) * 4, *sh = p + ((256 - 52) * 256 + 120) * 4, *wh = p + (5 * 256 + 5) * 4;
    save(@"carenderer", px, 256, 256, "bgra8");
    result("CARenderer layers (shadow, radius)", cc[0] > 200 && cc[2] < 60 && sh[1] < 245 && wh[0] > 250 && wh[1] > 250,
           [NSString stringWithFormat:@"card %d %d %d, shadow %d, white %d %d %d", cc[2], cc[1], cc[0], sh[1], wh[2], wh[1], wh[0]]);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        outDir = argc > 1 ? @(argv[1]) : @"/tmp/fw";
        [[NSFileManager defaultManager] createDirectoryAtPath:outDir withIntermediateDirectories:YES attributes:nil error:nil];
        dev = MTLCreateSystemDefaultDevice();
        q = [dev newCommandQueue];
        printf("fw_test on %s\n", dev.name.UTF8String);
        const char *only = getenv("FW_ONLY");
        struct { const char *n; void (*f)(void); } tests[] = {
            {"coreimage", testCoreImage}, {"mpsmatrix", testMPSMatrix}, {"mpsblur", testMPSBlur}, {"mpsgraph", testMPSGraph},
            {"scenekit", testSceneKit}, {"spritekit", testSpriteKit}, {"cvcache", testCVTextureCache}, {"carenderer", testCARenderer},
        };
        // FW_MSGLOG=1: every Objective-C message of the tests goes to
        // /tmp/msgSends-<pid> (libobjc's instrumentObjcMessageSends)
        void (*msglog)(BOOL) = getenv("FW_MSGLOG") ? (void (*)(BOOL))dlsym(RTLD_DEFAULT, "instrumentObjcMessageSends") : NULL;
        for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
            if (only && !strstr(only, tests[i].n)) continue;
            if (msglog) msglog(YES);
            @autoreleasepool { tests[i].f(); }
            if (msglog) msglog(NO);
        }
        printf("fw_test: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
        return fails != 0;
    }
}
