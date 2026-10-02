// New VRAM must never show a previous process's data (NVGspControl 0.147.1).
//   sudo vram_zero_test          parent: writes 0xAB over 256 MiB of private
//                                buffers, exits them, then runs the reader
//   vram_zero_test read          child: fresh private buffers, blit to shared, check zero
#import <Metal/Metal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
extern char **environ;

static const NSUInteger kBuf = 64u << 20, kCount = 4;

static id<MTLDevice> findNV(void) {
    for (id<MTLDevice> d in MTLCopyAllDevices())
        if ([d.name containsString:@"NVIDIA"]) return d;
    return nil;
}

static int writer(void) {
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) return 2;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSMutableArray *keep = [NSMutableArray new];
        for (NSUInteger i = 0; i < kCount; i++) {
            id<MTLBuffer> b = [dev newBufferWithLength:kBuf options:MTLResourceStorageModePrivate];
            if (!b) return 2;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be fillBuffer:b range:NSMakeRange(0, kBuf) value:0xAB];
            [be endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            [keep addObject:b];
        }
        printf("  writer pid %d: %lu MiB of VRAM filled with 0xAB\n", getpid(), (unsigned long)(kBuf * kCount >> 20));
    }
    return 0;   // exit frees the objects
}

static int reader(void) {
    @autoreleasepool {
        id<MTLDevice> dev = findNV();
        if (!dev) return 2;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> out = [dev newBufferWithLength:kBuf options:MTLResourceStorageModeShared];
        size_t stale = 0;
        for (NSUInteger i = 0; i < kCount; i++) {
            id<MTLBuffer> b = [dev newBufferWithLength:kBuf options:MTLResourceStorageModePrivate];
            memset(out.contents, 0x5A, kBuf);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromBuffer:b sourceOffset:0 toBuffer:out destinationOffset:0 size:kBuf];
            [be endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            const uint8_t *p = out.contents;
            for (NSUInteger k = 0; k < kBuf; k += 4096) if (p[k] != 0) stale++;
            if (i == 0) printf("  reader: first bytes %02x %02x %02x (0xAB = old data, 0x5A = copy never landed)\n",
                               p[0], p[kBuf / 2], p[kBuf - 1]);
        }
        printf("  reader pid %d: %zu of %lu sampled pages not zero\n", getpid(), stale,
               (unsigned long)(kBuf * kCount / 4096));
        return stale ? 1 : 0;
    }
}

static int run(const char *self, const char *mode) {
    char *argv[] = {(char *)self, (char *)mode, NULL};
    pid_t pid = 0;
    if (posix_spawn(&pid, self, NULL, NULL, argv, environ)) return -1;
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "write")) return writer();
    if (argc > 1 && !strcmp(argv[1], "read")) return reader();
    const int w = run(argv[0], "write");
    const int r = w == 0 ? run(argv[0], "read") : -1;
    printf("  %-60s %s\n", "fresh VRAM holds no data from an earlier process", r == 0 ? "PASS" : "FAIL");
    printf("vram_zero_test: %s\n", r == 0 ? "PASS" : "FAIL");
    return r == 0 ? 0 : 1;
}
