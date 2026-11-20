// avdecode <file> [10]: decode the first video track with AVAssetReader (NV12) and print one hash per frame
// plus totals. Run once with NVMTL_NVVT=0 (software) and once without (NVDEC via NVMTL 0.9.93), then diff.
// clang -fobjc-arc avdecode.m -framework AVFoundation -framework CoreVideo -framework CoreMedia -framework Metal -framework Foundation -o avdecode
#import <AVFoundation/AVFoundation.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) return 2;
        const bool ten = argc > 2 && !strcmp(argv[2], "10");   // optional: P010-style 10-bit output
        const size_t bps = ten ? 2 : 1;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();   // loads NVMTL, which registers the NVDEC decoders
        (void)dev; usleep(300000);
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(argv[1])] options:nil];
        dispatch_semaphore_t s = dispatch_semaphore_create(0);
        __block NSArray *tracks = nil;
        [asset loadTracksWithMediaType:AVMediaTypeVideo completionHandler:^(NSArray *t, NSError *e) { tracks = t; dispatch_semaphore_signal(s); }];
        dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
        if (!tracks.count) { printf("no video track\n"); return 1; }
        NSError *err = nil;
        AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:&err];
        AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tracks[0]
            outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(ten ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)}];
        [r addOutput:o];
        if (![r startReading]) { printf("startReading: %s\n", r.error.description.UTF8String); return 1; }
        int frames = 0;
        NSDate *t0 = [NSDate date];
        CMSampleBufferRef sb;
        const bool quiet = getenv("AVDECODE_QUIET") != NULL;
        while ((sb = [o copyNextSampleBuffer])) {
            CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
            if (pb) {
                CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
                CC_MD5_CTX m; CC_MD5_Init(&m);
                for (size_t p = 0; p < CVPixelBufferGetPlaneCount(pb); p++) {
                    const uint8_t *b = CVPixelBufferGetBaseAddressOfPlane(pb, p);
                    const size_t bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, p), w = CVPixelBufferGetWidthOfPlane(pb, p) * (p ? 2 : 1) * bps;
                    for (size_t y = 0; y < CVPixelBufferGetHeightOfPlane(pb, p); y++) CC_MD5_Update(&m, b + y * bpr, (CC_LONG)w);
                }
                uint8_t d[16]; CC_MD5_Final(d, &m);
                if (getenv("AVDECODE_DUMP") && atoi(getenv("AVDECODE_DUMP")) == frames) {   // raw NV12, cropped rows
                    FILE *f = fopen(getenv("AVDECODE_OUT") ? getenv("AVDECODE_OUT") : "/tmp/frame.nv12", "wb");
                    for (size_t p = 0; p < CVPixelBufferGetPlaneCount(pb); p++) {
                        const uint8_t *b = CVPixelBufferGetBaseAddressOfPlane(pb, p);
                        const size_t bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, p), w = CVPixelBufferGetWidthOfPlane(pb, p) * (p ? 2 : 1) * bps;
                        for (size_t y = 0; y < CVPixelBufferGetHeightOfPlane(pb, p); y++) fwrite(b + y * bpr, 1, w, f);
                    }
                    fclose(f);
                }
                CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
                if (!quiet) printf("%d %02x%02x%02x%02x%02x%02x%02x%02x\n", frames, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
                frames++;
            }
            CFRelease(sb);
        }
        printf("frames %d status %ld %s in %.2f s\n", frames, (long)r.status,
               r.error ? r.error.description.UTF8String : "", -[t0 timeIntervalSinceNow]);
        return r.status == AVAssetReaderStatusCompleted ? 0 : 1;
    }
}
