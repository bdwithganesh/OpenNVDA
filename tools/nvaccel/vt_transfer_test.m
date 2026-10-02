// VTPixelTransferSession YCbCr -> BGRA (what ImageIO's HEIC decode does on
// Intel/NVIDIA; VT runs its Metal transfer kernels on the GPU). Mid-grey
// and a colour patch in 8-bit ('420v') and 10-bit ('x420') sources;
// prints the BGRA result of each. Compare with Apple's GPU.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#include <stdlib.h>

static CVPixelBufferRef make(OSType fmt, size_t w, size_t h) {
    CVPixelBufferRef pb = NULL;
    NSDictionary *a = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}, (id)kCVPixelBufferMetalCompatibilityKey: @YES};
    CVPixelBufferCreate(NULL, w, h, fmt, (__bridge CFDictionaryRef)a, &pb);
    return pb;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        // optional: source size and destination size (scaled transfer)
        const size_t W = argc > 1 ? (size_t)atol(argv[1]) : 64, H = W;
        const size_t DW = argc > 2 ? (size_t)atol(argv[2]) : W, DH = DW;
        const OSType fmts[2] = {kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange};
        VTPixelTransferSessionRef s = NULL;
        VTPixelTransferSessionCreate(NULL, &s);
        int bad = 0;
        for (int f = 0; f < 2; f++) {
            CVPixelBufferRef src = make(fmts[f], W, H), dst = make(kCVPixelFormatType_32BGRA, DW, DH);
            CVPixelBufferLockBaseAddress(src, 0);
            // Y 180 (8-bit video range), Cb 100, Cr 160: a warm light colour
            for (size_t y = 0; y < H; y++) {
                uint8_t *yl = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(src, 0) + y * CVPixelBufferGetBytesPerRowOfPlane(src, 0);
                for (size_t x = 0; x < W; x++) { if (f) ((uint16_t *)yl)[x] = 180u << 8; else yl[x] = 180; }
            }
            for (size_t y = 0; y < H / 2; y++) {
                uint8_t *cl = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(src, 1) + y * CVPixelBufferGetBytesPerRowOfPlane(src, 1);
                for (size_t x = 0; x < W / 2; x++) {
                    if (f) { ((uint16_t *)cl)[2 * x] = 100u << 8; ((uint16_t *)cl)[2 * x + 1] = 160u << 8; }
                    else { cl[2 * x] = 100; cl[2 * x + 1] = 160; }
                }
            }
            CVPixelBufferUnlockBaseAddress(src, 0);
            const OSStatus st = VTPixelTransferSessionTransferImage(s, src, dst);
            CVPixelBufferLockBaseAddress(dst, kCVPixelBufferLock_ReadOnly);
            const uint8_t *p = (const uint8_t *)CVPixelBufferGetBaseAddress(dst) + (DH / 2) * CVPixelBufferGetBytesPerRow(dst) + (DW / 2) * 4;
            printf("  %s -> BGRA: status %d, pixel B %u G %u R %u A %u\n", f ? "x420 (10-bit)" : "420v (8-bit) ", (int)st, p[0], p[1], p[2], p[3]);
            // BT.601/709 of Y180 Cb100 Cr160 lands near R 220 G 190 B 150; flag anything far off
            bad += st != 0 || p[2] < 150 || p[1] < 120 || p[0] < 80;
            CVPixelBufferUnlockBaseAddress(dst, kCVPixelBufferLock_ReadOnly);
            CVPixelBufferRelease(src); CVPixelBufferRelease(dst);
        }
        printf("vt_transfer_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
