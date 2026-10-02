// Driver failure injection: never submit a literal VA 0 when its fallback
// page cannot be allocated. Also check that fully bound work still succeeds.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    alarm(40);
    const BOOL inject = argc == 2 && !strcmp(argv[1], "--fail-null-page");
    if (argc > 2 || (argc == 2 && !inject)) return 2;
    if (inject) setenv("NVMTL_FORCE_NULLPAGE_FAILURE", "1", 1);
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (!d) return 1;
        const BOOL nv = [d.name containsString:@"NVIDIA"];
        if (inject && !nv) { puts("SKIP: NVIDIA-specific failure hook"); return 77; }
        NSError *e = nil;
        id<MTLLibrary> l = [d newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void copy_value(device const uint *a [[buffer(0)]], device uint *b [[buffer(1)]], uint i [[thread_position_in_grid]]) { b[i]=a[i]+7; }\n"
             "vertex float4 vert(uint i [[vertex_id]], device const float4 *p [[buffer(0)]]) { return p[i]; }\n"
             "fragment float4 frag() { return float4(1,0,0,1); }\n"
            options:nil error:&e];
        id<MTLComputePipelineState> cp = l ? [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"copy_value"] error:&e] : nil;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [l newFunctionWithName:@"vert"];
        pd.fragmentFunction = [l newFunctionWithName:@"frag"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> rp = [d newRenderPipelineStateWithDescriptor:pd error:&e];
        id<MTLCommandQueue> q = [d newCommandQueue];
        uint32_t values[64]; for (unsigned i=0;i<64;i++) values[i]=i+11;
        const float verts[12]={-.9,-.9,0,1, .9,-.9,0,1, 0,.9,0,1};
        id<MTLBuffer> in = [d newBufferWithBytes:values length:sizeof values options:MTLResourceStorageModeShared];
        id<MTLBuffer> out = [d newBufferWithLength:sizeof values options:MTLResourceStorageModeShared];
        id<MTLBuffer> vb = [d newBufferWithBytes:verts length:sizeof verts options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
        td.storageMode=MTLStorageModePrivate; td.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> tex = [d newTextureWithDescriptor:td];
        id<MTLBuffer> pixels = [d newBufferWithLength:256*32 options:MTLResourceStorageModeShared];
        if (!cp || !rp || !q || !in || !out || !vb || !tex || !pixels) { NSLog(@"setup failed %@", e); return 1; }
        int bad=0;
        for (int pass=0;pass<2;pass++) {
            const BOOL fail = inject && pass==0;
            memset(out.contents,0xa5,sizeof values);
            id<MTLCommandBuffer> cb=[q commandBuffer];
            id<MTLComputeCommandEncoder> ce=[cb computeCommandEncoder];
            [ce setComputePipelineState:cp];
            if (!fail) [ce setBuffer:in offset:0 atIndex:0];
            [ce setBuffer:out offset:0 atIndex:1];
            [ce dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
            [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
            unsigned wrong=0; for(unsigned i=0;i<64;i++) wrong+=((uint32_t *)out.contents)[i]!=(fail?0xa5a5a5a5u:values[i]+7);
            BOOL ok=cb.status==(fail?MTLCommandBufferStatusError:MTLCommandBufferStatusCompleted) && wrong==0 && (!fail || cb.error!=nil);
            printf("compute %s status=%ld wrong=%u %s\n",fail?"injected":"bound",(long)cb.status,wrong,ok?"PASS":"FAIL"); bad+=!ok;
            MTLRenderPassDescriptor *rd=[MTLRenderPassDescriptor renderPassDescriptor];
            rd.colorAttachments[0].texture=tex; rd.colorAttachments[0].loadAction=MTLLoadActionClear;
            rd.colorAttachments[0].storeAction=MTLStoreActionStore;
            cb=[q commandBuffer]; id<MTLRenderCommandEncoder> re=[cb renderCommandEncoderWithDescriptor:rd];
            [re setRenderPipelineState:rp]; if(!fail) [re setVertexBuffer:vb offset:0 atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            if (!fail) {
                id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
                [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,32,1)
                          toBuffer:pixels destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*32];
                [be endEncoding];
            }
            [cb commit]; [cb waitUntilCompleted];
            ok=cb.status==(fail?MTLCommandBufferStatusError:MTLCommandBufferStatusCompleted) && (!fail || cb.error!=nil);
            unsigned red=0;
            if (!fail) { const uint8_t *px=pixels.contents;
                for(unsigned y=0;y<32;y++) for(unsigned x=0;x<32;x++) {
                    const uint8_t *p=px+y*256+x*4; red+=p[0]>240 && p[1]<10 && p[2]<10 && p[3]>240;
                }
                ok=ok && red>100;
            }
            printf("render %s status=%ld red=%u %s\n",fail?"injected":"bound",(long)cb.status,red,ok?"PASS":"FAIL"); bad+=!ok;
        }
        printf("metal_null_failure_test: %s (%d failures)\n",bad?"FAIL":"PASS",bad);
        return bad!=0;
    }
}
