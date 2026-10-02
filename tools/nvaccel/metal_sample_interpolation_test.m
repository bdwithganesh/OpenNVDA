// Sample-qualified interpolation must run at each MSAA sample. Squaring the
// fractional pixel coordinate distinguishes sample interpolation from center.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    @autoreleasepool {
        id<MTLDevice> d=MTLCreateSystemDefaultDevice();
        if (!d) return 1;
        NSError *err=nil;
        id<MTLLibrary> lib=[d newLibraryWithSource:
          @"#include <metal_stdlib>\nusing namespace metal;\n"
          @"struct V { float4 p [[position]]; float2 uv [[sample_perspective]]; };\n"
          @"vertex V vs(uint i [[vertex_id]]) { V v; v.p=float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1); v.uv=(v.p.xy+1.0)*0.5; return v; }\n"
          @"fragment float4 fs(V v [[stage_in]]) { float2 f=fract(v.uv*8.0); return float4(f*f,0,1); }\n"
            options:nil error:&err];
        if (!lib) { printf("compile FAIL: %s\n",err.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q=[d newCommandQueue]; unsigned failures=0;
        const unsigned counts[]={2,4,8}; const float expect[]={0.3125f,0.328125f,0.33203125f};
        for (unsigned k=0;k<3;k++) {
            unsigned n=counts[k];
            if (![d supportsTextureSampleCount:n]) { printf("sample interpolation %ux SKIP unsupported\n",n); continue; }
            MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
            pd.vertexFunction=[lib newFunctionWithName:@"vs"]; pd.fragmentFunction=[lib newFunctionWithName:@"fs"];
            pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA16Float; pd.rasterSampleCount=n;
            id<MTLRenderPipelineState> ps=[d newRenderPipelineStateWithDescriptor:pd error:&err];
            if (!ps) { printf("pipeline %ux FAIL: %s\n",n,err.localizedDescription.UTF8String); failures++; continue; }
            MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:8 height:8 mipmapped:NO];
            td.storageMode=d.hasUnifiedMemory?MTLStorageModeShared:MTLStorageModeManaged; td.usage=MTLTextureUsageRenderTarget;
            id<MTLTexture> out=[d newTextureWithDescriptor:td];
            td.storageMode=MTLStorageModePrivate; td.textureType=MTLTextureType2DMultisample; td.sampleCount=n;
            id<MTLTexture> ms=[d newTextureWithDescriptor:td];
            if (!ms || !out) { printf("texture %ux FAIL\n",n); failures++; continue; }
            MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture=ms; rp.colorAttachments[0].resolveTexture=out;
            rp.colorAttachments[0].loadAction=MTLLoadActionClear; rp.colorAttachments[0].storeAction=MTLStoreActionMultisampleResolve;
            id<MTLCommandBuffer> cb=[q commandBuffer]; id<MTLRenderCommandEncoder> re=[cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:ps]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
            if (out.storageMode==MTLStorageModeManaged) { id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder]; [bl synchronizeResource:out]; [bl endEncoding]; }
            [cb commit]; [cb waitUntilCompleted];
            __fp16 pixels[64*4]; [out getBytes:pixels bytesPerRow:8*8 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
            unsigned bad=0;
            for (unsigned i=0;i<64;i++) for (unsigned c=0;c<4;c++) {
                float want=c<2?expect[k]:c==3, got=(float)pixels[i*4+c];
                if (!isfinite(got) || fabsf(got-want)>0.0006f) bad++;
            }
            BOOL ok=!bad && cb.status==MTLCommandBufferStatusCompleted;
            printf("sample interpolation %ux bad=%u first=%.6f,%.6f want=%.6f status=%lu %s\n",n,bad,(float)pixels[0],(float)pixels[1],expect[k],(unsigned long)cb.status,ok?"PASS":"FAIL");
            failures+=!ok;
        }
        printf("metal_sample_interpolation_test on %s: %u failures\n",d.name.UTF8String,failures);
        return failures!=0;
    }
}
