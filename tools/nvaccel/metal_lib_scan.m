// Compiles every kernel function in a .metallib through the default Metal
// device and prints the ones that fail. Coverage check for system libraries
// (VFX, CoreImage, MPS...) against NVMTLDriver + nakc.
//   metal_lib_scan <lib.metallib> [name-filter]
// Vertex/fragment functions are only listed (no pipeline is built for them).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: %s lib.metallib [filter]\n", argv[0]); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        NSURL *url = [NSURL fileURLWithPath:@(argv[1])];
        id<MTLLibrary> lib = [dev newLibraryWithURL:url error:&e];
        if (!lib) { printf("library load failed: %s\n", e.localizedDescription.UTF8String); return 1; }
        NSString *filter = argc > 2 ? @(argv[2]) : nil;
        unsigned kernels = 0, ok = 0, fc = 0, other = 0;
        for (NSString *name in [lib.functionNames sortedArrayUsingSelector:@selector(compare:)]) {
            if (filter && ![name containsString:filter]) continue;
            id<MTLFunction> fn = [lib newFunctionWithName:name];
            if (!fn) {
                // needs function constants: not compiled here
                ++fc;
                continue;
            }
            if (fn.functionType != MTLFunctionTypeKernel) { ++other; continue; }
            ++kernels;
            NSDate *t0 = [NSDate date];
            id ps = [dev newComputePipelineStateWithFunction:fn error:&e];
            const double ms = -[t0 timeIntervalSinceNow] * 1000;
            if (ps) ++ok;
            else printf("FAIL %-60s %s\n", name.UTF8String, e.localizedDescription.UTF8String);
            if (ms > 2000) printf("SLOW %-60s %.0f ms\n", name.UTF8String, ms);
        }
        printf("%s: %u/%u kernels built, %u need function constants, %u vertex/fragment\n",
               argv[1], ok, kernels, fc, other);
        return ok == kernels ? 0 : 1;
    }
}
