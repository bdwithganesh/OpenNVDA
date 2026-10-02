// ca_compare: Core Animation scenes rendered by CARenderer on this Mac's GPU
// (the renderer WindowServer uses), one PNG per scene, to diff the RTX against
// the M1 (tools/nvaccel/ca_diff.py). Covers what the desktop is made of:
// shadows, rounded masks, gradients and colours, backdrop blur (glass), text,
// group opacity and an animation frozen at a fixed time.
// Build: clang -fobjc-arc -framework Metal -framework QuartzCore -framework AppKit
//        -framework CoreImage ca_compare.m -o ca_compare
// Use:   ./ca_compare OUTDIR
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

static const CGFloat W = 640, H = 400;

static CGColorRef rgb(double r, double g, double b, double a) {
    return CGColorCreateSRGB(r, g, b, a);
}

// a busy background (stripes + circles) so blur and shadows have something to act on
static CALayer *background(void) {
    CALayer *bg = [CALayer layer];
    bg.frame = CGRectMake(0, 0, W, H);
    bg.backgroundColor = rgb(0.95, 0.95, 0.9, 1);
    for (int i = 0; i < 16; i++) {
        CALayer *s = [CALayer layer];
        s.frame = CGRectMake(i * 40, 0, 20, H);
        s.backgroundColor = rgb(i / 16.0, 0.3 + 0.04 * i, 1 - i / 16.0, 1);
        [bg addSublayer:s];
    }
    for (int i = 0; i < 6; i++) {
        CALayer *c = [CALayer layer];
        c.frame = CGRectMake(40 + i * 95, 60 + (i % 2) * 150, 90, 90);
        c.cornerRadius = 45;
        c.backgroundColor = rgb(1, 0.8 - i * 0.1, 0.1 * i, 1);
        [bg addSublayer:c];
    }
    return bg;
}

static CALayer *sceneShadows(void) {
    CALayer *root = background();
    for (int i = 0; i < 3; i++) {
        CALayer *l = [CALayer layer];
        l.frame = CGRectMake(60 + i * 190, 110, 150, 180);
        l.backgroundColor = rgb(1, 1, 1, 1);
        l.cornerRadius = 12 + i * 8;
        l.shadowOpacity = 0.35 + 0.2 * i;
        l.shadowRadius = 6 + i * 10;
        l.shadowOffset = CGSizeMake(0, -4 - 4 * i);
        l.shadowColor = rgb(0, 0, 0, 1);
        [root addSublayer:l];
    }
    return root;
}

static CALayer *sceneMasks(void) {
    CALayer *root = background();
    for (int i = 0; i < 3; i++) {
        CALayer *clip = [CALayer layer];
        clip.frame = CGRectMake(40 + i * 200, 80, 170, 240);
        clip.cornerRadius = 30;
        clip.masksToBounds = YES;
        clip.borderWidth = 2 + i;
        clip.borderColor = rgb(0.1, 0.1, 0.1, 1);
        clip.backgroundColor = rgb(0.2, 0.6, 0.9, 1);
        CALayer *inner = [CALayer layer];
        inner.frame = CGRectMake(-20, 60, 210, 60);
        inner.backgroundColor = rgb(0.9, 0.2, 0.3, 1);
        inner.transform = CATransform3DMakeRotation(0.3 + i * 0.2, 0, 0, 1);
        [clip addSublayer:inner];
        if (i == 2) {                                         // shape mask
            CAShapeLayer *m = [CAShapeLayer layer];
            m.path = CGPathCreateWithEllipseInRect(CGRectMake(10, 10, 150, 220), NULL);
            clip.mask = m;
        }
        [root addSublayer:clip];
    }
    return root;
}

static CALayer *sceneColors(void) {
    CALayer *root = [CALayer layer];
    root.frame = CGRectMake(0, 0, W, H);
    root.backgroundColor = rgb(0, 0, 0, 1);
    CAGradientLayer *g = [CAGradientLayer layer];
    g.frame = CGRectMake(0, 200, W, 200);
    g.colors = @[(__bridge id)rgb(1, 0, 0, 1), (__bridge id)rgb(0, 1, 0, 1), (__bridge id)rgb(0, 0, 1, 1)];
    g.startPoint = CGPointMake(0, 0.5); g.endPoint = CGPointMake(1, 0.5);
    [root addSublayer:g];
    CAGradientLayer *gr = [CAGradientLayer layer];                 // grey ramp: banding / gamma
    gr.frame = CGRectMake(0, 140, W, 60);
    gr.colors = @[(__bridge id)rgb(0, 0, 0, 1), (__bridge id)rgb(1, 1, 1, 1)];
    gr.startPoint = CGPointMake(0, 0.5); gr.endPoint = CGPointMake(1, 0.5);
    [root addSublayer:gr];
    CGColorSpaceRef p3 = CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
    for (int i = 0; i < 8; i++) {                                  // sRGB and P3 swatches, alpha steps
        CALayer *s = [CALayer layer];
        s.frame = CGRectMake(10 + i * 78, 10, 70, 55);
        const CGFloat c[4] = {i & 1 ? 1.0 : 0.0, i & 2 ? 1.0 : 0.0, i & 4 ? 1.0 : 0.0, 1};
        s.backgroundColor = CGColorCreate(p3, c);
        [root addSublayer:s];
        CALayer *a = [CALayer layer];
        a.frame = CGRectMake(10 + i * 78, 75, 70, 55);
        a.backgroundColor = rgb(1, 1, 1, (i + 1) / 8.0);
        [root addSublayer:a];
    }
    return root;
}

static CALayer *sceneBlur(void) {
    CALayer *root = background();
    // the Dock/menu "glass": a backdrop layer blurring what is behind it
    Class bdc = NSClassFromString(@"CABackdropLayer");
    Class fc = NSClassFromString(@"CAFilter");
    for (int i = 0; i < 2; i++) {
        CALayer *b = bdc ? [bdc layer] : [CALayer layer];
        b.frame = CGRectMake(40 + i * 300, 100, 260, 200);
        b.cornerRadius = 24;
        b.masksToBounds = YES;
        if (fc) {
            id blur = [fc performSelector:@selector(filterWithType:) withObject:@"gaussianBlur"];
            [blur setValue:@(i ? 30 : 10) forKey:@"inputRadius"];
            id sat = [fc performSelector:@selector(filterWithType:) withObject:@"colorSaturate"];
            [sat setValue:@1.8 forKey:@"inputAmount"];
            b.filters = @[blur, sat];
        }
        CALayer *tint = [CALayer layer];
        tint.frame = b.bounds;
        tint.backgroundColor = rgb(1, 1, 1, 0.25);
        [b addSublayer:tint];
        [root addSublayer:b];
    }
    return root;
}

static CALayer *sceneText(void) {
    CALayer *root = [CALayer layer];
    root.frame = CGRectMake(0, 0, W, H);
    root.backgroundColor = rgb(1, 1, 1, 1);
    NSArray *sizes = @[@11, @13, @17, @24, @36];
    for (NSUInteger i = 0; i < sizes.count; i++) {
        CATextLayer *t = [CATextLayer layer];
        t.frame = CGRectMake(10, H - 60 - i * 70, W - 20, 60);
        t.string = @"The quick brown fox 0123456789 Finder Safari";
        t.font = (__bridge CFTypeRef)[NSFont systemFontOfSize:[sizes[i] doubleValue]];
        t.fontSize = [sizes[i] doubleValue];
        t.foregroundColor = rgb(0.1, 0.1, 0.1, 1);
        t.contentsScale = 2;
        [root addSublayer:t];
    }
    return root;
}

static CALayer *sceneAnim(void) {
    CALayer *root = background();
    CALayer *group = [CALayer layer];
    group.frame = CGRectMake(120, 80, 400, 240);
    group.opacity = 0.6;
    group.allowsGroupOpacity = YES;
    for (int i = 0; i < 3; i++) {
        CALayer *l = [CALayer layer];
        l.frame = CGRectMake(20 + i * 110, 40, 140, 160);
        l.cornerRadius = 20;
        l.backgroundColor = rgb(0.1 + 0.3 * i, 0.5, 0.9 - 0.3 * i, 1);
        [group addSublayer:l];
    }
    [root addSublayer:group];
    // a window-like zoom + fade, frozen half way (beginFrameAtTime below)
    CALayer *win = [CALayer layer];
    win.frame = CGRectMake(200, 120, 240, 160);
    win.backgroundColor = rgb(1, 1, 1, 1);
    win.cornerRadius = 14;
    win.shadowOpacity = 0.5; win.shadowRadius = 20;
    CABasicAnimation *s = [CABasicAnimation animationWithKeyPath:@"transform.scale"];
    s.fromValue = @0.2; s.toValue = @1.0; s.duration = 1.0; s.beginTime = 1.0;   // absolute layer time
    CABasicAnimation *o = [CABasicAnimation animationWithKeyPath:@"opacity"];
    o.fromValue = @0.0; o.toValue = @1.0; o.duration = 1.0; o.beginTime = 1.0;
    s.fillMode = o.fillMode = kCAFillModeBoth;
    [win addAnimation:s forKey:@"s"];
    [win addAnimation:o forKey:@"o"];
    [root addSublayer:win];
    return root;
}


// compositing filters (blend modes) and vibrancy: menus, sidebars, the lock-screen clock
static CALayer *sceneBlend(void) {
    CALayer *root = background();
    Class fc = NSClassFromString(@"CAFilter");
    NSArray *modes = @[@"multiplyBlendMode", @"screenBlendMode", @"overlayBlendMode", @"darkenBlendMode",
                       @"lightenBlendMode", @"colorDodgeBlendMode", @"colorBurnBlendMode", @"softLightBlendMode",
                       @"hardLightBlendMode", @"differenceBlendMode", @"plusD", @"plusL",
                       @"subtractS", @"luminosityBlendMode", @"vibrantDark", @"vibrantLight"];
    for (NSUInteger i = 0; i < modes.count; i++) {
        CALayer *l = [CALayer layer];
        l.frame = CGRectMake(10 + (i % 8) * 78, 40 + (i / 8) * 180, 70, 150);
        l.backgroundColor = rgb(0.5, 0.35 + 0.03 * i, 0.8 - 0.04 * i, 0.85);
        l.cornerRadius = 10;
        if (fc) l.compositingFilter = [fc performSelector:@selector(filterWithType:) withObject:modes[i]];
        CATextLayer *t = [CATextLayer layer];                  // vibrant text on it, like the clock
        t.frame = CGRectMake(0, 50, 70, 50);
        t.string = @"12";
        t.fontSize = 40;
        t.alignmentMode = kCAAlignmentCenter;
        t.foregroundColor = rgb(1, 1, 1, 0.9);
        t.contentsScale = 2;
        if (fc) t.compositingFilter = [fc performSelector:@selector(filterWithType:) withObject:@"plusL"];
        [l addSublayer:t];
        [root addSublayer:l];
    }
    return root;
}


// Liquid Glass: backdrop layers with the glass filters WindowServer uses
// (variable_blur_vert / glass_background shaders), Dock- and alert-shaped
static CALayer *sceneGlass(void) {
    CALayer *root = background();
    Class bdc = NSClassFromString(@"CABackdropLayer"), fc = NSClassFromString(@"CAFilter");
    const CGRect frames[3] = {CGRectMake(30, 20, 580, 70), CGRectMake(200, 130, 240, 240), CGRectMake(40, 150, 120, 200)};
    const CGFloat radii[3] = {24, 40, 60};
    for (int i = 0; i < 3; i++) {
        CALayer *b = bdc ? [bdc layer] : [CALayer layer];
        b.frame = frames[i];
        b.cornerRadius = radii[i];
        b.cornerCurve = kCACornerCurveContinuous;
        b.masksToBounds = YES;
        if (fc) {
            id g = [fc performSelector:@selector(filterWithType:) withObject:@"glassBackground"];
            id f = [fc performSelector:@selector(filterWithType:) withObject:@"glassForeground"];
            b.filters = i == 2 ? @[g, f] : @[g];
        }
        [root addSublayer:b];
    }
    return root;
}

static BOOL render(id<MTLDevice> dev, id<MTLCommandQueue> q, CALayer *root, NSString *path) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:(NSUInteger)(W * 2)
                                                                                 height:(NSUInteger)(H * 2) mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModeManaged;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    CARenderer *r = [CARenderer rendererWithMTLTexture:t options:@{kCARendererMetalCommandQueue: q}];
    root.transform = CATransform3DIdentity;
    CALayer *top = [CALayer layer];                   // 2x, like a Retina / HiDPI desktop
    top.frame = CGRectMake(0, 0, W * 2, H * 2);
    root.anchorPoint = CGPointZero;
    root.position = CGPointZero;
    root.transform = CATransform3DMakeScale(2, 2, 1);
    [top addSublayer:root];
    r.layer = top;
    r.bounds = top.frame;
    top.beginTime = 0;
    [CATransaction flush];
    [r beginFrameAtTime:1.5 timeStamp:NULL];          // the animation scene is half way at t = 1.5
    [r addUpdateRect:r.bounds];
    [r render];
    [r endFrame];
    id<MTLCommandBuffer> cb = [q commandBuffer];      // same queue: runs after CA's work
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b synchronizeResource:t];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const size_t w = t.width, h = t.height;
    uint8_t *px = malloc(w * h * 4);
    [t getBytes:px bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
    const BOOL ok = [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
    free(px);
    return ok;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *out = argc > 1 ? @(argv[1]) : @".";
        [[NSFileManager defaultManager] createDirectoryAtPath:out withIntermediateDirectories:YES attributes:nil error:nil];
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        struct { const char *name; CALayer *(*fn)(void); } list[] = {
            {"shadows", sceneShadows}, {"masks", sceneMasks}, {"colors", sceneColors},
            {"blur", sceneBlur}, {"text", sceneText}, {"anim", sceneAnim}, {"blend", sceneBlend}, {"glass", sceneGlass},
        };
        printf("ca_compare on %s\n", dev.name.UTF8String);
        for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) {
            NSString *p = [out stringByAppendingFormat:@"/%s.png", list[i].name];
            printf("  %-8s %s\n", list[i].name, render(dev, q, list[i].fn(), p) ? "ok" : "FAILED");
        }
    }
    return 0;
}
