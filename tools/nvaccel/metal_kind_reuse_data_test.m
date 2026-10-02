// Repeated kind-VA reuse must preserve texture contents, not only allocation.
#import <Metal/Metal.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    alarm(100); setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool {
        id<MTLDevice> d = nil;
        for (id<MTLDevice> x in MTLCopyAllDevices()) if ([x.name containsString:@"NVIDIA"]) d = x;
        if (!d) d = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> l = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; kernel void readtex(texture2d<float,access::read> t[[texture(0)]], device uint *b[[buffer(0)]], uint2 p[[thread_position_in_grid]]) { uint4 v=uint4(t.read(p)*255.0f+0.5f); b[p.y*32+p.x]=v.x|(v.y<<8)|(v.z<<16)|(v.w<<24); }" options:nil error:&e];
        id<MTLComputePipelineState> ps = l ? [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"readtex"] error:&e] : nil;
        id<MTLCommandQueue> q = [d newCommandQueue];
        id<MTLBuffer> b = [d newBufferWithLength:32*32*4 options:MTLResourceStorageModeShared];
        if (!ps || !q || !b) { printf("setup failed %s\n", e.description.UTF8String); return 1; }
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:YES];
        td.storageMode=MTLStorageModePrivate; td.usage=MTLTextureUsageShaderRead;
        printf("device=%s 4096 released mipmapped texture upload/read cycles\n", d.name.UTF8String);
        for (unsigned k=0;k<4096;k++) @autoreleasepool {
            id<MTLTexture> t=[d newTextureWithDescriptor:td];
            if (!t) { printf("FAIL nil at %u\n", k); return 1; }
            uint32_t pixels[1024];
            for (unsigned i=0;i<1024;i++) pixels[i]=0xff000000u|((k+i*17)&0xffffffu);
            [t replaceRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0 withBytes:pixels bytesPerRow:128];
            id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLComputeCommandEncoder> ce=[cb computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setTexture:t atIndex:0]; [ce setBuffer:b offset:0 atIndex:0];
            [ce dispatchThreads:MTLSizeMake(32,32,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)]; [ce endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) { printf("FAIL CB at %u error=%s\n",k,cb.error.description.UTF8String); return 1; }
            const uint32_t *got=b.contents;
            for (unsigned i=0;i<1024;i++) if (got[i]!=pixels[i]) { printf("FAIL data at cycle=%u index=%u want=%08x got=%08x\n",k,i,pixels[i],got[i]); return 1; }
            if ((k+1)%1024==0) printf("data-correct %u\n",k+1);
        }
        printf("PASS 4096 kind reuse upload/read cycles\n"); return 0;
    }
}
