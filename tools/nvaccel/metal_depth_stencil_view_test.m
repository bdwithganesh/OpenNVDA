#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Render real depth/stencil data, then read every ragged-grid texel through
// shader views created before and after rendering. Repeat with cached views.
enum { W = 9, H = 3, N = W * H };
static float expectedDepth(unsigned x, unsigned phase) {
    return x < 4 ? (phase ? .25f : .125f) : phase == 1 ? .875f : phase == 2 ? .375f : .625f;
}
static uint32_t expectedStencil(unsigned x, unsigned phase) {
    return x < 4 ? (phase ? 38u : 37u) : phase == 1 ? 65u : phase == 2 ? 217u : 219u;
}
static NSString *const source =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
     "vertex float4 vs(uint i [[vertex_id]], constant float &z [[buffer(0)]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],z,1); }\n"
     "fragment void fs() {}\n"
     "kernel void rd(depth2d<float,access::read> t [[texture(0)]],device float *o [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { if(p.x<9&&p.y<3)o[p.y*9+p.x]=t.read(p); }\n"
     "kernel void rc(depth2d<float> t [[texture(0)]],sampler s [[sampler(0)]],device float *o [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { if(p.x<9&&p.y<3)o[p.y*9+p.x]=t.sample_compare(s,(float2(p)+0.5f)/float2(9,3),0.5f); }\n"
     "kernel void rl(depth2d<float> t [[texture(0)]],sampler s [[sampler(0)]],device float *o [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { if(p.x<9&&p.y<3)o[p.y*9+p.x]=t.sample_compare(s,(float2(p)+float2(1,0.5f))/float2(9,3),0.5f); }\n"
     "kernel void rs(texture2d<uint,access::read> t [[texture(0)]],device uint *o [[buffer(0)]],uint2 p [[thread_position_in_grid]]) { if(p.x<9&&p.y<3)o[p.y*9+p.x]=t.read(p).r; }\n";

static int readTexture(id<MTLCommandQueue> queue, id<MTLComputePipelineState> pso,
                       id<MTLTexture> texture, unsigned depth, unsigned phase,
                       const char *label) {
    if (!texture) { printf("  %s FAIL (nil texture/view)\n", label); return 1; }
    id<MTLBuffer> out = [queue.device newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    memset(out.contents, 0xa5, N * 4);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:pso]; [ce setTexture:texture atIndex:0];
    if (depth >= 2) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.compareFunction = MTLCompareFunctionLessEqual;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        if (depth == 3) sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        [ce setSamplerState:[queue.device newSamplerStateWithDescriptor:sd] atIndex:0];
    }
    [ce setBuffer:out offset:0 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(W,H,1) threadsPerThreadgroup:MTLSizeMake(W,H,1)];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int wrong = 0;
    for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
        unsigned i = y * W + x;
        if (depth) {
            float expected = expectedDepth(x, phase);
            if (depth >= 2) {
                expected = expected >= .5f ? 1.f : 0.f;
                if (depth == 3) expected = .5f * (expected + (expectedDepth(MIN(x+1,W-1),phase) >= .5f ? 1.f : 0.f));
            }
            float actual = ((float *)out.contents)[i];
            if (!isfinite(actual) || fabsf(actual - expected) > 2.f/65535.f) wrong++;
        } else if (((uint32_t *)out.contents)[i] != expectedStencil(x, phase)) wrong++;
    }
    BOOL ok = !wrong && cb.status == MTLCommandBufferStatusCompleted;
    printf("  %s phase%u %s (%d/%d wrong, first=%g)\n", label, phase,
           ok ? "PASS" : "FAIL", wrong, N,
           depth ? ((float *)out.contents)[0] : (double)((uint32_t *)out.contents)[0]);
    return !ok;
}

int main(int argc, const char **argv) {
    BOOL renderOnly = NO, stopOnFailure = NO;
    for (int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--render-only"))renderOnly=YES;
        else if(!strcmp(argv[i],"--stop-on-failure"))stopOnFailure=YES;
        else { fprintf(stderr,"usage: %s [--render-only] [--stop-on-failure]\n",argv[0]); return 2; }
    }
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *error = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:source options:nil error:&error];
        if (!lib) { fprintf(stderr,"shader compile: %s\n",error.description.UTF8String); return 1; }
        id<MTLComputePipelineState> rd = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rd"] error:&error];
        id<MTLComputePipelineState> rs = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rs"] error:&error];
        id<MTLComputePipelineState> rc = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rc"] error:&error];
        id<MTLComputePipelineState> rl = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rl"] error:&error];
        if (!rd || !rs || !rc || !rl) { fprintf(stderr,"compute pipeline: %s\n",error.description.UTF8String); return 1; }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        const struct { MTLPixelFormat format, view; BOOL depth, stencil; const char *name; } cases[] = {
            {MTLPixelFormatDepth16Unorm, MTLPixelFormatInvalid, YES, NO, "Depth16"},
            {MTLPixelFormatDepth32Float, MTLPixelFormatInvalid, YES, NO, "Depth32"},
            {MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatX32_Stencil8, YES, YES, "Depth32S8"},
            {MTLPixelFormatStencil8, MTLPixelFormatInvalid, NO, YES, "Stencil8"},
            {MTLPixelFormatDepth24Unorm_Stencil8, MTLPixelFormatX24_Stencil8, YES, YES, "Depth24S8"},
        };
        int failures = 0, checked = 0;
        printf("device: %s\n", dev.name.UTF8String);
        for (unsigned c = 0; c < sizeof cases/sizeof cases[0]; c++) {
            if (cases[c].format == MTLPixelFormatDepth24Unorm_Stencil8 && !dev.depth24Stencil8PixelFormatSupported) {
                printf("%s SKIP (device unsupported)\n",cases[c].name); continue;
            }
            printf("%s:\n", cases[c].name);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:cases[c].format width:W height:H mipmapped:NO];
            td.storageMode = MTLStorageModePrivate;
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView | (renderOnly ? 0 : MTLTextureUsageShaderRead);
            id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
            if (!tex) { failures++; printf("  FAIL (creation)\n"); continue; }
            id<MTLTexture> early = cases[c].view ? [tex newTextureViewWithPixelFormat:cases[c].view] : tex;
            if (early) (void)early.gpuResourceID; // exercise cached headers before render
            id<MTLTexture> late = nil;
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib newFunctionWithName:@"vs"];
            pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
            if (cases[c].depth) pd.depthAttachmentPixelFormat = cases[c].format;
            if (cases[c].stencil) pd.stencilAttachmentPixelFormat = cases[c].format;
            id<MTLRenderPipelineState> rpso = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
            if (!rpso) { failures++; printf("  FAIL (pipeline %s)\n",error.description.UTF8String); continue; }
            MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
            ds.depthCompareFunction = MTLCompareFunctionAlways; ds.depthWriteEnabled = cases[c].depth;
            ds.frontFaceStencil.stencilCompareFunction = MTLCompareFunctionAlways;
            ds.frontFaceStencil.depthStencilPassOperation = MTLStencilOperationReplace;
            ds.backFaceStencil = ds.frontFaceStencil;
            id<MTLDepthStencilState> state = [dev newDepthStencilStateWithDescriptor:ds];
            unsigned bpp = cases[c].format == MTLPixelFormatDepth16Unorm ? 2 :
                           cases[c].format == MTLPixelFormatStencil8 ? 1 :
                           cases[c].format == MTLPixelFormatDepth32Float_Stencil8 ? 8 : 4;
            id<MTLBuffer> packed = [dev newBufferWithLength:256*H+64 options:MTLResourceStorageModeShared];
            for (unsigned phase = 0; phase < 3; phase++) {
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                if (cases[c].depth) { pass.depthAttachment.texture=tex; pass.depthAttachment.loadAction=phase ? MTLLoadActionLoad : MTLLoadActionClear; pass.depthAttachment.storeAction=MTLStoreActionStore; pass.depthAttachment.clearDepth=1; }
                if (cases[c].stencil) { pass.stencilAttachment.texture=tex; pass.stencilAttachment.loadAction=phase ? MTLLoadActionLoad : MTLLoadActionClear; pass.stencilAttachment.storeAction=MTLStoreActionStore; pass.stencilAttachment.clearStencil=0; }
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLBuffer> preservedStencil=nil;
                if (phase == 1) {
                    // Change the backing through a blit, then load it in a render
                    // pass which only overwrites the left side. Cached views
                    // must see the new right side and the subsequent render.
                    memset(packed.contents,0,packed.length);
                    BOOL combined=cases[c].view!=MTLPixelFormatInvalid;
                    unsigned bytes=combined?4:bpp;
                    for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
                        uint8_t *p=(uint8_t *)packed.contents+16+y*256+x*bytes;
                        if (bpp==1) *p=65;
                        else if (bpp==2) { uint16_t z=(uint16_t)lroundf(.875f*65535.f); memcpy(p,&z,2); }
                        else if (cases[c].format==MTLPixelFormatDepth24Unorm_Stencil8) {
                            uint32_t z=(uint32_t)lroundf(.875f*16777215.f); memcpy(p,&z,4);
                        } else { float z=.875f; memcpy(p,&z,4); }
                    }
                    id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
                    [be copyFromBuffer:packed sourceOffset:16 sourceBytesPerRow:256 sourceBytesPerImage:256*H
                            sourceSize:MTLSizeMake(W,H,1) toTexture:tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)
                            options:combined?MTLBlitOptionDepthFromDepthStencil:MTLBlitOptionNone];
                    if(combined) {
                        preservedStencil=[dev newBufferWithLength:256*H+64 options:MTLResourceStorageModeShared];
                        memset(preservedStencil.contents,0xa5,preservedStencil.length);
                        [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                                 sourceSize:MTLSizeMake(W,H,1) toBuffer:preservedStencil destinationOffset:32 destinationBytesPerRow:256 destinationBytesPerImage:256*H
                                 options:MTLBlitOptionStencilFromDepthStencil];
                        id<MTLBuffer> stencil=[dev newBufferWithLength:256*H+64 options:MTLResourceStorageModeShared];
                        memset(stencil.contents,65,stencil.length);
                        [be copyFromBuffer:stencil sourceOffset:16 sourceBytesPerRow:256 sourceBytesPerImage:256*H
                                sourceSize:MTLSizeMake(W,H,1) toTexture:tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)
                                options:MTLBlitOptionStencilFromDepthStencil];
                    }
                    [be endEncoding];
                }
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
                [re setRenderPipelineState:rpso]; [re setDepthStencilState:state];
                float z = .125f * (1 + phase);
                [re setVertexBytes:&z length:sizeof z atIndex:0];
                [re setScissorRect:(MTLScissorRect){0,0,4,H}]; [re setStencilReferenceValue:37+phase];
                if (phase != 2) [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                z = .625f - .125f * phase; [re setVertexBytes:&z length:sizeof z atIndex:0];
                [re setScissorRect:(MTLScissorRect){4,0,W-4,H}]; [re setStencilReferenceValue:219-phase];
                if (phase != 1) [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                id<MTLBuffer> copy=[dev newBufferWithLength:256*H+64 options:MTLResourceStorageModeShared];
                memset(copy.contents,0xa5,copy.length);
                BOOL combined=cases[c].view!=MTLPixelFormatInvalid;
                id<MTLBuffer> stencilCopy=combined?[dev newBufferWithLength:256*H+64 options:MTLResourceStorageModeShared]:nil;
                if(stencilCopy)memset(stencilCopy.contents,0xa5,stencilCopy.length);
                id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
                [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                         sourceSize:MTLSizeMake(W,H,1) toBuffer:copy destinationOffset:32 destinationBytesPerRow:256 destinationBytesPerImage:256*H
                         options:combined?MTLBlitOptionDepthFromDepthStencil:MTLBlitOptionNone];
                if(combined)[be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                         sourceSize:MTLSizeMake(W,H,1) toBuffer:stencilCopy destinationOffset:32 destinationBytesPerRow:256 destinationBytesPerImage:256*H
                         options:MTLBlitOptionStencilFromDepthStencil];
                [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
                if (cb.status != MTLCommandBufferStatusCompleted) { printf("  render FAIL %s\n",cb.error.description.UTF8String); failures++; break; }
                if(preservedStencil) {
                    int bad=0;
                    for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++)
                        if(((uint8_t *)preservedStencil.contents)[32+y*256+x]!=expectedStencil(x,0))bad++;
                    printf("  depth upload preserves stencil %s (%d wrong)\n",bad?"FAIL":"PASS",bad);
                    failures += bad!=0; checked++;
                }
                int wrong=0;
                for(unsigned y=0;y<H;y++) for(unsigned x=0;x<W;x++) {
                    const uint8_t *p=(const uint8_t *)copy.contents+32+y*256+x*(combined?4:bpp);
                    if(cases[c].depth) {
                        float d;
                        if(bpp==2) { uint16_t raw; memcpy(&raw,p,2); d=raw/65535.f; }
                        else if(cases[c].format==MTLPixelFormatDepth24Unorm_Stencil8) {
                            uint32_t raw; memcpy(&raw,p,4); d=(raw&0xffffffu)/16777215.f;
                        } else memcpy(&d,p,4);
                        if(!isfinite(d)||fabsf(d-expectedDepth(x,phase))>2.f/65535.f)wrong++;
                    }
                    if(cases[c].stencil) {
                        uint8_t st=combined?((uint8_t *)stencilCopy.contents)[32+y*256+x]:p[0];
                        if(st!=expectedStencil(x,phase))wrong++;
                    }
                }
                for(unsigned i=0;i<copy.length;i++) {
                    BOOL texel=i>=32 && (i-32)/256<H && (i-32)%256<W*(combined?4:bpp);
                    if(!texel&&((uint8_t *)copy.contents)[i]!=0xa5)wrong++;
                    BOOL stencilTexel=i>=32 && (i-32)/256<H && (i-32)%256<W;
                    if(stencilCopy&&!stencilTexel&&((uint8_t *)stencilCopy.contents)[i]!=0xa5)wrong++;
                }
                printf("  aspect blit phase%u %s (%d wrong)\n",phase,wrong?"FAIL":"PASS",wrong);
                failures += wrong!=0; checked++;
                if(stopOnFailure&&failures)break;
                if (!renderOnly && cases[c].depth) { failures += readTexture(queue,rd,tex,YES,phase,"depth"); checked++; }
                if(stopOnFailure&&failures)break;
                if (!renderOnly && cases[c].depth) { failures += readTexture(queue,rc,tex,2,phase,"depth compare"); checked++; }
                if(stopOnFailure&&failures)break;
                if (!renderOnly && cases[c].depth) { failures += readTexture(queue,rl,tex,3,phase,"depth PCF"); checked++; }
                if(stopOnFailure&&failures)break;
                if (!renderOnly && cases[c].stencil) { failures += readTexture(queue,rs,early,NO,phase,"stencil early"); checked++; }
                if (!renderOnly && cases[c].view) {
                    if (!phase) { id<MTLTexture> view=[tex newTextureViewWithPixelFormat:cases[c].view]; late=[view newTextureViewWithPixelFormat:cases[c].view]; }
                    failures += readTexture(queue,rs,late,NO,phase,"stencil late/nested"); checked++;
                }
            }
            if(stopOnFailure&&failures)break;
            if(!renderOnly) {
                id<MTLTexture> clone=[dev newTextureWithDescriptor:td];
                id<MTLTexture> stencil=cases[c].view?[clone newTextureViewWithPixelFormat:cases[c].view]:clone;
                id<MTLCommandBuffer> cb=[queue commandBuffer];
                id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
                [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                         sourceSize:MTLSizeMake(W,H,1) toTexture:clone destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
                [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
                if(cb.status!=MTLCommandBufferStatusCompleted) { failures++; printf("  texture copy FAIL\n"); }
                if(cases[c].depth) { failures+=readTexture(queue,rd,clone,YES,2,"depth texture copy"); checked++; }
                if(cases[c].stencil) { failures+=readTexture(queue,rs,stencil,NO,2,"stencil texture copy"); checked++; }
            }
            if(stopOnFailure&&failures)break;
        }
        printf("metal_depth_stencil_view_test: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS",checked,failures);
        return failures ? 1 : 0;
    }
}
