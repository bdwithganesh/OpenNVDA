// Core Image built-in filter sweep: each filter with its defaults on a test
// image, rendered by the Metal context and by the software renderer, and the
// difference printed. Run the same on an Apple GPU Mac for the baseline
// (some filters differ between GPU and software there too).
//   ci_sweep list          filter names, one per line
//   ci_sweep one <name>    "<name> <maxdiff> <meandiff> <status>"
//   ci_sweep dump <name> <prefix>   also writes <prefix>.gpu/.cpu (raw RGBA8, w h in the name line)
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static CIImage *testImage(int variant) {
    CIImage *a = [[CIFilter filterWithName:@"CICheckerboardGenerator" withInputParameters:@{
        @"inputCenter": [CIVector vectorWithX:3 Y:5], @"inputColor0": [CIColor colorWithRed:0.9 green:0.3 blue:0.1],
        @"inputColor1": [CIColor colorWithRed:0.1 green:0.4 blue:0.8], @"inputWidth": @(variant ? 5 : 9), @"inputSharpness": @0.8}] outputImage];
    CIImage *g = [[CIFilter filterWithName:@"CILinearGradient" withInputParameters:@{
        @"inputPoint0": [CIVector vectorWithX:0 Y:0], @"inputPoint1": [CIVector vectorWithX:64 Y:64],
        @"inputColor0": [CIColor colorWithRed:1 green:1 blue:1 alpha:variant ? 0.3 : 0.6],
        @"inputColor1": [CIColor colorWithRed:0 green:0.2 blue:0 alpha:0.9]}] outputImage];
    return [[g imageByCompositingOverImage:a] imageByCroppingToRect:CGRectMake(0, 0, 64, 64)];
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "list")) {
            for (NSString *n in [CIFilter filterNamesInCategory:kCICategoryBuiltIn]) printf("%s\n", n.UTF8String);
            return 0;
        }
        if (argc < 3) { fprintf(stderr, "usage: ci_sweep list | one <name> | dump <name> <prefix>\n"); return 2; }
        const char *dump = argc > 3 && !strcmp(argv[1], "dump") ? argv[3] : NULL;
        NSString *name = @(argv[2]);
        CIFilter *f = [CIFilter filterWithName:name];
        if (!f) { printf("%s - - nofilter\n", argv[2]); return 0; }
        [f setDefaults];
        CIImage *img = testImage(0), *img2 = testImage(1);
        for (NSString *k in f.inputKeys) {
            NSDictionary *attr = f.attributes[k];
            if ([attr[kCIAttributeClass] isEqual:@"CIImage"])
                [f setValue:[k isEqual:kCIInputImageKey] ? img : img2 forKey:k];
        }
        // the same region on every OS: Tahoe's default inputExtent is empty
        // (histograms give no output), macOS 27's is not
        if ([f.inputKeys containsObject:kCIInputExtentKey])
            [f setValue:[CIVector vectorWithCGRect:CGRectMake(0, 0, 64, 64)] forKey:kCIInputExtentKey];
        if ([f.inputKeys containsObject:@"inputText"]) [f setValue:@"Metal" forKey:@"inputText"];
        if ([f.inputKeys containsObject:@"inputMessage"]) [f setValue:[@"metal" dataUsingEncoding:NSUTF8StringEncoding] forKey:@"inputMessage"];
        CIImage *out = nil;
        @try { out = f.outputImage; } @catch (NSException *x) {
            printf("%s - - exception\n", argv[2]);
            fprintf(stderr, "exception: %s: %s\n", x.name.UTF8String, x.reason.UTF8String);
            return 0;
        }
        if (!out) { printf("%s - - nooutput\n", argv[2]); return 0; }
        CGRect r = CGRectIntersection(out.extent, CGRectMake(0, 0, 64, 64));
        if (CGRectIsEmpty(r) || CGRectIsInfinite(out.extent)) r = CGRectMake(0, 0, 64, 64);
        r = CGRectIntegral(r);
        const size_t w = (size_t)r.size.width, h = (size_t)r.size.height;
        if (!w || !h) { printf("%s - - empty\n", argv[2]); return 0; }
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        NSMutableData *g = [NSMutableData dataWithLength:w * h * 4], *s = [NSMutableData dataWithLength:w * h * 4];
        CIContext *gpu = [CIContext contextWithMTLDevice:MTLCreateSystemDefaultDevice() options:@{kCIContextCacheIntermediates: @NO}];
        CIContext *cpu = [CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer: @YES}];
        [gpu render:out toBitmap:g.mutableBytes rowBytes:w * 4 bounds:r format:kCIFormatRGBA8 colorSpace:cs];
        [cpu render:out toBitmap:s.mutableBytes rowBytes:w * 4 bounds:r format:kCIFormatRGBA8 colorSpace:cs];
        CGColorSpaceRelease(cs);
        if (dump) {
            [g writeToFile:[NSString stringWithFormat:@"%s.gpu", dump] atomically:NO];
            [s writeToFile:[NSString stringWithFormat:@"%s.cpu", dump] atomically:NO];
            printf("size %zu %zu\n", w, h);
        }
        const uint8_t *a = g.bytes, *b = s.bytes;
        int mx = 0;
        double sum = 0;
        for (size_t i = 0; i < w * h * 4; i++) { const int d = abs(a[i] - b[i]); if (d > mx) mx = d; sum += d; }
        printf("%s %d %.3f ok\n", argv[2], mx, sum / (w * h * 4));
        return 0;
    }
}
