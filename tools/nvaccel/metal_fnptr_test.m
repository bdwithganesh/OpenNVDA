// Function pointers: [[visible]] functions linked into a compute pipeline,
// called through a visible function table (entries set from function
// handles), per thread a different entry; plus an entry left empty.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "[[visible]] float opAdd(float a, float b) { return a + b; }\n"
     "[[visible]] float opMul(float a, float b) { return a * b; }\n"
     "[[visible]] float opSub(float a, float b) { return a - b; }\n"
     "kernel void run(visible_function_table<float(float, float)> ops [[buffer(0)]],\n"
     "                device const uint *sel [[buffer(1)]], device float *out [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
     "  out[i] = ops[sel[i]](float(i), 2.0f); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("%s: supportsFunctionPointers %d\n", dev.name.UTF8String, dev.supportsFunctionPointers);
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        NSArray *names = @[@"opAdd", @"opMul", @"opSub"];
        NSMutableArray *fns = [NSMutableArray new];
        for (NSString *n in names) [fns addObject:[lib newFunctionWithName:n]];
        MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = [lib newFunctionWithName:@"run"];
        MTLLinkedFunctions *lf = [MTLLinkedFunctions new];
        lf.functions = fns;
        cd.linkedFunctions = lf;
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&e];
        if (!ps) { printf("pipeline: %s\nmetal_fnptr_test: FAIL\n", e.description.UTF8String); return 1; }
        MTLVisibleFunctionTableDescriptor *td = [MTLVisibleFunctionTableDescriptor new];
        td.functionCount = 4;   // entry 3 stays empty
        id<MTLVisibleFunctionTable> tab = [ps newVisibleFunctionTableWithDescriptor:td];
        if (!tab) { printf("no table\nmetal_fnptr_test: FAIL\n"); return 1; }
        // table order differs from link order on purpose
        [tab setFunction:[ps functionHandleWithFunction:fns[2]] atIndex:0];
        [tab setFunction:[ps functionHandleWithFunction:fns[0]] atIndex:1];
        [tab setFunction:[ps functionHandleWithFunction:fns[1]] atIndex:2];
        const NSUInteger n = 1024;
        id<MTLBuffer> sel = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> out = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
        for (NSUInteger i = 0; i < n; i++) ((uint32_t *)sel.contents)[i] = (uint32_t)(i % 3);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps];
        [ce setVisibleFunctionTable:tab atBufferIndex:0];
        [ce setBuffer:sel offset:0 atIndex:1];
        [ce setBuffer:out offset:0 atIndex:2];
        [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        int bad = 0;
        for (NSUInteger i = 0; i < n; i++) {
            const float x = (float)i, want = i % 3 == 0 ? x - 2 : i % 3 == 1 ? x + 2 : x * 2;
            bad += ((float *)out.contents)[i] != want;
        }
        printf("  %d wrong of %lu, status %ld\nmetal_fnptr_test: %s\n", bad, (unsigned long)n, (long)cb.status,
               bad || cb.status != MTLCommandBufferStatusCompleted ? "FAIL" : "PASS");
        return bad != 0;
    }
}
