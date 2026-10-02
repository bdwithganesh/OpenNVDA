// An HEIC (desktop picture, may carry an HDR gain map) through Core Image
// on the GPU and on the CPU, plain and with the gain map applied
// (kCIImageExpandToHDR), downscaled to 256; prints mean RGB of each and the
// difference. QuickLook's thumbnail of such a file came out white.
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>

static void mean(CIContext *ctx, CIImage *img, double m[4], NSMutableData **out) {
    CGRect e = img.extent;
    const CGFloat s = 256.0 / MAX(e.size.width, e.size.height);
    CIImage *small = [img imageByApplyingTransform:CGAffineTransformMakeScale(s, s)];
    CGRect r = CGRectIntegral(small.extent);
    const size_t w = (size_t)r.size.width, h = (size_t)r.size.height;
    NSMutableData *d = [NSMutableData dataWithLength:w * h * 4];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    [ctx render:small toBitmap:d.mutableBytes rowBytes:w * 4 bounds:r format:kCIFormatRGBA8 colorSpace:cs];
    CGColorSpaceRelease(cs);
    const uint8_t *p = d.bytes;
    for (int c = 0; c < 4; c++) m[c] = 0;
    for (size_t i = 0; i < w * h; i++) for (int c = 0; c < 4; c++) m[c] += p[i * 4 + c];
    for (int c = 0; c < 4; c++) m[c] /= (double)(w * h);
    *out = d;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *path = argc > 1 ? @(argv[1]) : @"/System/Library/Desktop Pictures/iMac Blue.heic";
        CIContext *gpu = [CIContext contextWithMTLDevice:MTLCreateSystemDefaultDevice() options:@{kCIContextCacheIntermediates: @NO}];
        CIContext *cpu = [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer: @YES}];
        int bad = 0;
        for (int hdr = 0; hdr < 2; hdr++) {
            NSDictionary *o = hdr ? @{@"kCIImageExpandToHDR": @YES} : @{};
            CIImage *img = [CIImage imageWithContentsOfURL:[NSURL fileURLWithPath:path] options:o];
            if (!img) { printf("  cannot load %s\n", path.UTF8String); return 1; }
            double g[4], c[4];
            NSMutableData *gd, *cd;
            mean(gpu, img, g, &gd);
            mean(cpu, img, c, &cd);
            const uint8_t *a = gd.bytes, *b = cd.bytes;
            int mx = 0;
            for (NSUInteger i = 0; i < gd.length; i++) { int d = abs(a[i] - b[i]); if (d > mx) mx = d; }
            printf("  %-12s gpu mean %.1f %.1f %.1f  cpu mean %.1f %.1f %.1f  max diff %d\n", hdr ? "gain map" : "plain",
                   g[0], g[1], g[2], c[0], c[1], c[2], mx);
            bad += mx > 16;
        }
        printf("ci_heic_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
