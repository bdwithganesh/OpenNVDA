// Signed/normalized render targets: actual fragment output, every ragged texel.
// Same probe on M1 and RTX; linear and mipped texture allocations, with controls.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { MTLPixelFormat fmt; const char *name; unsigned comps, bytes, kind; } Format;
enum { SNORM, SINT, UINT, UNORM };
#define F(n,c,b,k) {MTLPixelFormat##n, #n, c, b, k}
static const Format formats[] = {
    F(R8Snorm,1,1,SNORM), F(RG8Snorm,2,1,SNORM), F(RGBA8Snorm,4,1,SNORM),
    F(R16Snorm,1,2,SNORM), F(RG16Snorm,2,2,SNORM), F(RGBA16Snorm,4,2,SNORM),
    F(RG8Sint,2,1,SINT), F(RG16Sint,2,2,SINT), F(RG32Sint,2,4,SINT),
    F(RG8Uint,2,1,UINT), F(R8Sint,1,1,SINT), F(RGBA8Sint,4,1,SINT),
    F(R16Unorm,1,2,UNORM), F(RGBA16Unorm,4,2,UNORM),
};
static int32_t load(const uint8_t *p, const Format *f) {
    if (f->kind == UINT) return *p;
    if (f->kind == UNORM) { uint16_t v; memcpy(&v,p,2); return v; }
    if (f->bytes == 1) { int8_t v; memcpy(&v,p,1); return v; }
    if (f->bytes == 2) { int16_t v; memcpy(&v,p,2); return v; }
    int32_t v; memcpy(&v,p,4); return v;
}
int main(int argc, char **argv) {
    BOOL resolve=argc>=2 && !strcmp(argv[1],"--resolve");
    unsigned samples=argc==3 ? (unsigned)atoi(argv[2]) : 4;
    if (argc>3 || (argc>=2 && !resolve) || (samples!=2 && samples!=4 && samples!=8)) {
        fprintf(stderr,"usage: metal_signed_render_test [--resolve [2|4|8]]\n"); return 2;
    }
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (!d) return 1;
        if (resolve && ![d supportsTextureSampleCount:samples]) {
            printf("resolve%ux on %s: SKIP unsupported sample count\n",samples,d.name.UTF8String);
            return 0;
        }
        NSError *err = nil;
        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
          @"vertex float4 vs(uint i [[vertex_id]]) { return float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1); }\n"
          @"fragment float4 sn(float4 p [[position]]) { return float4(-0.75,0.25,-0.5,0.5)+floor(p.x)*0.03125+floor(p.y)*0.0625; }\n"
          @"fragment int4 si(float4 p [[position]]) { return int4(-37,17,-11,7)+int(p.x)+3*int(p.y); }\n"
          @"fragment uint4 ui(float4 p [[position]]) { return uint4(37,17,11,7)+uint(p.x)+3*uint(p.y); }\n";
        src=[src stringByAppendingString:
          @"fragment float4 un(float4 p [[position]]) { return float4(0.125,0.25,0.375,0.5)+floor(p.x)*0.03125+floor(p.y)*0.0625; }\n"
          @"fragment float4 snMS(float4 p [[position]], uint s [[sample_id]]) { return float4(-0.75,0.25,-0.5,0.25)+floor(p.x)*0.03125+floor(p.y)*0.0625+s*0.03125; }\n"
          @"fragment float4 unMS(float4 p [[position]], uint s [[sample_id]]) { return float4(0.125,0.25,0.375,0.25)+floor(p.x)*0.03125+floor(p.y)*0.0625+s*0.03125; }\n"];
        id<MTLLibrary> lib = [d newLibraryWithSource:src options:nil error:&err];
        if (!lib) { printf("library FAIL: %s\n",err.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        unsigned failed = 0;
        for (unsigned mode=resolve?2:0; mode<(resolve?3:2); mode++) for (unsigned k=0; k<sizeof formats/sizeof formats[0]; k++) {
            const Format *f=&formats[k];
            if (resolve && (f->kind==SINT || f->kind==UINT)) continue;
            char samplePath[32]; snprintf(samplePath,sizeof samplePath,"resolve%ux",samples);
            const char *path=resolve?samplePath:mode?"mipped":"linear";
            MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f->fmt width:9 height:3 mipmapped:mode==1];
            td.usage=MTLTextureUsageRenderTarget;
            td.storageMode=d.hasUnifiedMemory ? MTLStorageModeShared : mode ? MTLStorageModeManaged : MTLStorageModeShared;
            id<MTLTexture> resolved=nil;
            if (resolve) {
                resolved=[d newTextureWithDescriptor:td];
                td.textureType=MTLTextureType2DMultisample; td.sampleCount=samples; td.storageMode=MTLStorageModePrivate;
            }
            id<MTLTexture> t=[d newTextureWithDescriptor:td];
            MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
            pd.vertexFunction=[lib newFunctionWithName:@"vs"];
            pd.fragmentFunction=[lib newFunctionWithName:resolve ? (f->kind==SNORM ? @"snMS" : @"unMS")
                    : f->kind==SNORM ? @"sn" : f->kind==SINT ? @"si" : f->kind==UNORM ? @"un" : @"ui"];
            pd.rasterSampleCount=resolve?samples:1;
            pd.colorAttachments[0].pixelFormat=f->fmt;
            id<MTLRenderPipelineState> ps=[d newRenderPipelineStateWithDescriptor:pd error:&err];
            if (!t || !ps || (resolve && !resolved)) { printf("%s %s creation FAIL: %s\n",f->name,path,err.localizedDescription.UTF8String); failed++; continue; }
            MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture=t;
            rp.colorAttachments[0].loadAction=MTLLoadActionClear;
            rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);
            rp.colorAttachments[0].storeAction=resolve?MTLStoreActionMultisampleResolve:MTLStoreActionStore;
            if (resolve) rp.colorAttachments[0].resolveTexture=resolved;
            id<MTLCommandBuffer> cb=[q commandBuffer];
            id<MTLRenderCommandEncoder> re=[cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:ps]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
            id<MTLTexture> output=resolve?resolved:t;
            if (output.storageMode==MTLStorageModeManaged) { id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder]; [bl synchronizeResource:output]; [bl endEncoding]; }
            [cb commit]; [cb waitUntilCompleted];
            uint8_t out[27*16]; memset(out,0x5a,sizeof out);
            const unsigned bpp=f->comps*f->bytes;
            [output getBytes:out bytesPerRow:9*bpp fromRegion:MTLRegionMake2D(0,0,9,3) mipmapLevel:0];
            unsigned bad=0;
            const float sn[4]={-0.75f,0.25f,-0.5f,resolve?0.25f:0.5f};
            const float un[4]={0.125f,0.25f,0.375f,resolve?0.25f:0.5f};
            const int si[4]={-37,17,-11,7}, ui[4]={37,17,11,7};
            for (unsigned y=0;y<3;y++) for (unsigned x=0;x<9;x++) for (unsigned c=0;c<f->comps;c++) {
                const BOOL norm=f->kind==SNORM || f->kind==UNORM;
                int32_t want=norm ? (int32_t)lrintf(((f->kind==SNORM?sn[c]:un[c])+x*0.03125f+y*0.0625f+(resolve?(samples-1)*0.015625f:0))
                                                    *(f->kind==UNORM?65535:f->bytes==1?127:32767))
                                          : (f->kind==SINT?si[c]:ui[c])+(int)x+3*(int)y;
                int32_t got=load(out+((y*9+x)*f->comps+c)*f->bytes,f);
                if (llabs((long long)got-want)>(norm?1:0)) {
                    if (!bad) printf("  first (%u,%u).%u got=%d want=%d\n",x,y,c,got,want);
                    bad++;
                }
            }
            BOOL ok=!bad && cb.status==MTLCommandBufferStatusCompleted;
            printf("%s %s bad=%u status=%lu %s\n",f->name,path,bad,(unsigned long)cb.status,ok?"PASS":"FAIL");
            failed+=!ok;
        }
        printf("metal_signed_render_test on %s: %u failures\n",d.name.UTF8String,failed);
        return failed!=0;
    }
}
