// mkclip <out.mp4> <h264|hevc> [seconds]: 1080p30 moving-gradient clip with Apple's (software) encoder.
// clang -fobjc-arc mkclip.m -framework AVFoundation -framework CoreVideo -framework CoreMedia -framework Foundation -o mkclip
#import <AVFoundation/AVFoundation.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) { fprintf(stderr, "usage: mkclip out.mp4 h264|hevc [seconds]\n"); return 2; }
        NSURL *url = [NSURL fileURLWithPath:@(argv[1])];
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
        const int seconds = argc > 3 ? atoi(argv[3]) : 10, W = 1920, H = 1080;
        NSError *err = nil;
        AVAssetWriter *w = [AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeMPEG4 error:&err];
        NSDictionary *set = @{AVVideoCodecKey: strcmp(argv[2], "hevc") ? AVVideoCodecTypeH264 : AVVideoCodecTypeHEVC,
                              AVVideoWidthKey: @(W), AVVideoHeightKey: @(H),
                              AVVideoCompressionPropertiesKey: @{AVVideoAverageBitRateKey: @(8000000),
                                  AVVideoProfileLevelKey: strcmp(argv[2], "hevc") ? AVVideoProfileLevelH264HighAutoLevel :
                                      (argc > 4 && !strcmp(argv[4], "main10")) ? @"HEVC_Main10_AutoLevel" : @"HEVC_Main_AutoLevel"}};
        AVAssetWriterInput *in = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:set];
        AVAssetWriterInputPixelBufferAdaptor *ad = [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in
            sourcePixelBufferAttributes:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                          (id)kCVPixelBufferWidthKey: @(W), (id)kCVPixelBufferHeightKey: @(H)}];
        [w addInput:in];
        if (![w startWriting]) { fprintf(stderr, "startWriting: %s\n", w.error.description.UTF8String); return 1; }
        [w startSessionAtSourceTime:kCMTimeZero];
        for (int i = 0; i < seconds * 30; i++) {
            while (!in.readyForMoreMediaData) usleep(1000);
            CVPixelBufferRef pb = NULL;
            CVPixelBufferPoolCreatePixelBuffer(NULL, ad.pixelBufferPool, &pb);
            CVPixelBufferLockBaseAddress(pb, 0);
            uint8_t *p = CVPixelBufferGetBaseAddress(pb);
            const size_t bpr = CVPixelBufferGetBytesPerRow(pb);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    uint8_t *q = p + y * bpr + x * 4;
                    const bool box = x > (i * 13) % (W - 200) && x < (i * 13) % (W - 200) + 200 && y > 400 && y < 600;
                    q[0] = box ? 255 : (uint8_t)(x + i * 4); q[1] = box ? 255 : (uint8_t)(y + i * 2);
                    q[2] = box ? 255 : (uint8_t)(x ^ y); q[3] = 255;
                }
            CVPixelBufferUnlockBaseAddress(pb, 0);
            [ad appendPixelBuffer:pb withPresentationTime:CMTimeMake(i, 30)];
            CVPixelBufferRelease(pb);
        }
        [in markAsFinished];
        dispatch_semaphore_t s = dispatch_semaphore_create(0);
        [w finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(s); }];
        dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
        printf("%s: status %ld %s\n", argv[1], (long)w.status, w.error ? w.error.description.UTF8String : "");
        return w.status == AVAssetWriterStatusCompleted ? 0 : 1;
    }
}
