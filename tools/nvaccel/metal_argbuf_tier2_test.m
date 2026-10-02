// Argument buffers, tier 2 features: 64 sampled textures in one argument
// buffer, a writable texture and device pointers in it, a sampler, argument
// buffers written directly (gpuResourceID / gpuAddress, Metal 3 style) and
// through MTLArgumentEncoder, and an argument buffer the GPU itself fills.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

static NSString *const kSrc =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "struct Args { array<texture2d<float>, 64> tex; texture2d<float, access::write> out; device float *vals; sampler s; };\n"
     "kernel void sum(constant Args &a [[buffer(0)]], device float *res [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
     "  if (i >= 64) return;\n"
     "  float4 c = a.tex[i].sample(a.s, float2(0.5));\n"
     "  res[i] = c.r + a.vals[i];\n"
     "  a.out.write(float4(c.r, 0, 0, 1), uint2(i, 0)); }\n"
     "struct Ids { array<texture2d<float>, 64> tex; };\n"
     "kernel void copyIds(constant Args &src [[buffer(0)]], device Ids &dst [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
     "  if (i < 64) dst.tex[63 - i] = src.tex[i]; }\n"
     "kernel void readIds(device Ids &ids [[buffer(0)]], device float *res [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
     "  if (i < 64) res[i] = ids.tex[i].read(uint2(0, 0)).r; }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("%s: argumentBuffersSupport tier %lu\n", dev.name.UTF8String, (unsigned long)dev.argumentBuffersSupport + 1);
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        if (!lib) { printf("compile: %s\nmetal_argbuf_tier2_test: FAIL\n", e.description.UTF8String); return 1; }
        id<MTLComputePipelineState> psSum = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"sum"] error:&e];
        id<MTLComputePipelineState> psCopy = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"copyIds"] error:&e];
        id<MTLComputePipelineState> psRead = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"readIds"] error:&e];
        if (!psSum || !psCopy || !psRead) { printf("pipeline: %s\nmetal_argbuf_tier2_test: FAIL\n", e.description.UTF8String); return 1; }
        // 64 1x1 R32Float textures, value i
        NSMutableArray<id<MTLTexture>> *tex = [NSMutableArray new];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        for (int i = 0; i < 64; i++) {
            id<MTLTexture> t = [dev newTextureWithDescriptor:td];
            float v = (float)i;
            [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&v bytesPerRow:4];
            [tex addObject:t];
        }
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:64 height:1 mipmapped:NO];
        od.usage = MTLTextureUsageShaderWrite; od.storageMode = MTLStorageModeManaged;
        id<MTLTexture> out = [dev newTextureWithDescriptor:od];
        id<MTLBuffer> vals = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
        for (int i = 0; i < 64; i++) ((float *)vals.contents)[i] = 1000.0f * i;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.supportArgumentBuffers = YES;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        int bad = 0;
        for (int mode = 0; mode < 2; mode++) {
            id<MTLArgumentEncoder> enc = [[lib newFunctionWithName:@"sum"] newArgumentEncoderWithBufferIndex:0];
            id<MTLBuffer> ab = [dev newBufferWithLength:enc.encodedLength options:MTLResourceStorageModeShared];
            if (mode == 0) {   // the encoder
                [enc setArgumentBuffer:ab offset:0];
                id<MTLTexture> __unsafe_unretained arr[64];
                for (int i = 0; i < 64; i++) arr[i] = tex[i];
                [enc setTextures:arr withRange:NSMakeRange(0, 64)];
                [enc setTexture:out atIndex:64];
                [enc setBuffer:vals offset:0 atIndex:65];
                [enc setSamplerState:smp atIndex:66];
            } else {           // Metal 3: write the values directly
                uint8_t *p = ab.contents;
                for (int i = 0; i < 64; i++) ((MTLResourceID *)p)[i] = tex[i].gpuResourceID;
                ((MTLResourceID *)p)[64] = out.gpuResourceID;
                *(uint64_t *)(p + 65 * 8) = vals.gpuAddress;
                ((MTLResourceID *)p)[66] = smp.gpuResourceID;
            }
            id<MTLBuffer> res = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> ids = [dev newBufferWithLength:64 * 8 options:MTLResourceStorageModeShared];
            id<MTLBuffer> res2 = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> q = [dev newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            for (id<MTLTexture> t in tex) [ce useResource:t usage:MTLResourceUsageRead];
            [ce useResource:out usage:MTLResourceUsageWrite];
            [ce useResource:vals usage:MTLResourceUsageRead];
            [ce setComputePipelineState:psSum];
            [ce setBuffer:ab offset:0 atIndex:0];
            [ce setBuffer:res offset:0 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce setComputePipelineState:psCopy];
            [ce setBuffer:ids offset:0 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
            [ce setComputePipelineState:psRead];
            [ce setBuffer:ids offset:0 atIndex:0];
            [ce setBuffer:res2 offset:0 atIndex:1];
            [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be synchronizeResource:out];
            [be endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            float row[64 * 4];
            [out getBytes:row bytesPerRow:64 * 16 fromRegion:MTLRegionMake2D(0, 0, 64, 1) mipmapLevel:0];
            int w1 = 0, w2 = 0, w3 = 0;
            for (int i = 0; i < 64; i++) {
                w1 += ((float *)res.contents)[i] != 1001.0f * i;
                w2 += row[i * 4] != (float)i;
                w3 += ((float *)res2.contents)[i] != (float)(63 - i);
            }
            printf("  %-8s sampled+pointer %d wrong, writable texture %d wrong, GPU-written ids %d wrong, status %ld\n",
                   mode ? "direct" : "encoder", w1, w2, w3, (long)cb.status);
            bad += w1 + w2 + w3 + (cb.status != MTLCommandBufferStatusCompleted);
        }
        printf("metal_argbuf_tier2_test: %s\n", bad ? "FAIL" : "PASS");
        return bad != 0;
    }
}
