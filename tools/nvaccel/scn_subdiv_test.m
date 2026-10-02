// scn_subdiv_test: SceneKit subdivision (OpenSubdiv GPU: eval_stencils,
// compute_opensubdiv) and hardware tessellation (SCNGeometryTessellator), the
// paths the Memoji picker runs, rendered offscreen with SCNRenderer. Saves a
// PNG for a pixel comparison against the M1 (ca_diff.py) and reports whether
// the frame finished.
// Build: clang -fobjc-arc -framework Metal -framework SceneKit -framework AppKit scn_subdiv_test.m -o scn_subdiv_test
// Use:   ./scn_subdiv_test out.png [mode]   mode 0 subdivision, 1 tessellator, 2 both (default)
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <SceneKit/SceneKit.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        const int mode = argc > 2 ? atoi(argv[2]) : 2;
        const NSUInteger W = 512, H = 512;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        SCNScene *scene = [SCNScene scene];
        SCNCamera *cam = [SCNCamera camera];
        SCNNode *camNode = [SCNNode node];
        camNode.camera = cam;
        camNode.position = SCNVector3Make(0, 0, 6);
        [scene.rootNode addChildNode:camNode];
        SCNNode *light = [SCNNode node];
        light.light = [SCNLight light];
        light.light.type = SCNLightTypeOmni;
        light.position = SCNVector3Make(3, 3, 6);
        [scene.rootNode addChildNode:light];
        for (int i = 0; i < 2; i++) {
            SCNGeometry *g = [SCNBox boxWithWidth:1.6 height:1.6 length:1.6 chamferRadius:0.1];
            if (mode == 0 || mode == 2) g.subdivisionLevel = 2;            // OpenSubdiv
            if ((mode == 1 || mode == 2) && i == 1) {                       // hardware tessellation
                SCNGeometryTessellator *t = [SCNGeometryTessellator new];
                t.edgeTessellationFactor = 6; t.insideTessellationFactor = 6;
                t.smoothingMode = SCNTessellationSmoothingModePNTriangles;
                g.tessellator = t;
            }
            g.firstMaterial.diffuse.contents = i ? [NSColor systemOrangeColor] : [NSColor systemTealColor];
            SCNNode *n = [SCNNode nodeWithGeometry:g];
            n.position = SCNVector3Make(i ? 1.1 : -1.1, 0, 0);
            n.eulerAngles = SCNVector3Make(0.5, 0.7, 0);
            [scene.rootNode addChildNode:n];
        }
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
        for (int f = 0; f < 3; f++) {                                       // a few frames: buffers settle
            id<MTLCommandBuffer> cb = [q commandBuffer];
            [r renderAtTime:f / 60.0 viewport:CGRectMake(0, 0, W, H) commandBuffer:cb passDescriptor:rp];
            if (f == 2) { id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder]; [b synchronizeResource:tex]; [b endEncoding]; }
            [cb commit];
            last = cb;
        }
        [last waitUntilCompleted];
        printf("scn_subdiv_test mode %d on %s: command buffer status %ld error %s\n", mode, dev.name.UTF8String,
               (long)last.status, last.error ? last.error.localizedDescription.UTF8String : "none");
        uint8_t *px = malloc(W * H * 4);
        [tex getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(px, W, H, 8, W * 4, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:CGBitmapContextCreateImage(ctx)];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argc > 1 ? argv[1] : "scn.png") atomically:YES];
        return last.status == MTLCommandBufferStatusCompleted ? 0 : 1;
    }
}
