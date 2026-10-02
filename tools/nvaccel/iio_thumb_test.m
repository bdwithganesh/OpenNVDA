// ImageIO thumbnail of an HEIC (what QuickLook does; on Intel/NVIDIA it goes
// through VideoToolbox's Metal transfer kernels): mean RGB of a 256 px
// thumbnail, and of the full decode scaled on the CPU for reference.
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>

static void meanOf(CGImageRef img, double m[3]) {
    const size_t w = 64, h = 64;
    uint8_t *p = calloc(w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(p, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    m[0] = m[1] = m[2] = 0;
    for (size_t i = 0; i < w * h; i++) for (int c = 0; c < 3; c++) m[c] += p[i * 4 + c];
    for (int c = 0; c < 3; c++) m[c] /= (double)(w * h);
    CGContextRelease(ctx); CGColorSpaceRelease(cs); free(p);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *path = argc > 1 ? @(argv[1]) : @"/System/Library/Desktop Pictures/iMac Blue.heic";
        CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
        if (!src) { printf("cannot open\n"); return 1; }
        CGImageRef th = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)@{
            (id)kCGImageSourceCreateThumbnailFromImageAlways: @YES, (id)kCGImageSourceThumbnailMaxPixelSize: @256,
            (id)kCGImageSourceCreateThumbnailWithTransform: @YES});
        CGImageRef full = CGImageSourceCreateImageAtIndex(src, 0, NULL);
        double a[3] = {0}, b[3] = {0};
        if (th) meanOf(th, a);
        if (full) meanOf(full, b);
        printf("  thumbnail %zux%zu mean %.1f %.1f %.1f | full decode mean %.1f %.1f %.1f\n", th ? CGImageGetWidth(th) : 0, th ? CGImageGetHeight(th) : 0,
               a[0], a[1], a[2], b[0], b[1], b[2]);
        int bad = 0;
        for (int c = 0; c < 3; c++) bad += fabs(a[c] - b[c]) > 12;
        printf("iio_thumb_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
