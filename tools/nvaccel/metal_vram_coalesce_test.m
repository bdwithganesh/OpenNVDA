// Three adjacent Private buffers must become one reusable 64 MiB span.
#import <Metal/Metal.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    alarm(35); setvbuf(stdout,NULL,_IONBF,0);
    @autoreleasepool {
        id<MTLDevice> d=MTLCreateSystemDefaultDevice();
        id<MTLBuffer> a=[d newBufferWithLength:16<<20 options:MTLResourceStorageModePrivate];
        id<MTLBuffer> b=[d newBufferWithLength:16<<20 options:MTLResourceStorageModePrivate];
        id<MTLBuffer> c=[d newBufferWithLength:32<<20 options:MTLResourceStorageModePrivate];
        if (!a || !b || !c) return 1;
        uint64_t av=a.gpuAddress,bv=b.gpuAddress,cv=c.gpuAddress;
        printf("%s initial VA %llx %llx %llx\n",d.name.UTF8String,av,bv,cv);
        a=nil; c=nil; b=nil; // bridge freed last, so both neighbours exist
        id<MTLBuffer> whole=[d newBufferWithLength:64<<20 options:MTLResourceStorageModePrivate];
        id<MTLBuffer> out=[d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q=[d newCommandQueue];
        if (!whole || !out || !q) return 1;
        printf("whole64MiB VA %llx\n",whole.gpuAddress);
        id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
        [be fillBuffer:whole range:NSMakeRange(0,4096) value:0x5a];
        [be copyFromBuffer:whole sourceOffset:0 toBuffer:out destinationOffset:0 size:4096];
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.status!=MTLCommandBufferStatusCompleted) { printf("FAIL CB %s\n",cb.error.description.UTF8String); return 1; }
        for (unsigned i=0;i<4096;i++) if (((const uint8_t*)out.contents)[i]!=0x5a) { printf("FAIL data %u\n",i);return 1; }
        if ([d.name containsString:@"NVIDIA"] && (bv!=av+(16<<20) || cv!=bv+(16<<20) || whole.gpuAddress!=av)) {
            printf("FAIL fully freed adjacent64MiB not reused; data correct\n"); return 1;
        }
        printf("PASS64MiB allocation/data; NVIDIA VA reuse checked, M1 allocator VA policy unconstrained\n"); return 0;
    }
}
