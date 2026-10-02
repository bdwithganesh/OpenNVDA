// Evidence for the GPU feature cross-check list: what the Metal device,
// OpenGL (CGL) and OpenCL expose on this machine.
//   feature_probe
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <OpenGL/OpenGL.h>
#import <OpenCL/opencl.h>
#include <dlfcn.h>
#import <objc/message.h>

int main(void) {
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        printf("metal: %s\n", d.name.UTF8String);
        printf("  msaa sample counts:");
        for (NSUInteger n = 1; n <= 16; n *= 2) if ([d supportsTextureSampleCount:n]) printf(" %lu", (unsigned long)n);
        printf("\n  rasterization rate maps (VRS): %d\n", [d supportsRasterizationRateMapWithLayerCount:1]);
        printf("  raytracing %d, function pointers %d, dynamic libraries %d, render dynamic libraries %d\n",
               d.supportsRaytracing, d.supportsFunctionPointers, d.supportsDynamicLibraries,
               d.supportsRenderDynamicLibraries);
        printf("  BC %d, 32-bit float filtering %d, query texture LOD %d, pull model interpolation %d\n",
               d.supportsBCTextureCompression, d.supports32BitFloatFiltering, d.supportsQueryTextureLOD,
               d.supportsPullModelInterpolation);
        printf("  shader barycentrics %d, primitive motion blur %d, counter sampling (stage) %d\n",
               d.supportsShaderBarycentricCoordinates, d.supportsPrimitiveMotionBlur,
               [d supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]);
        printf("  vertex amplification 2: %d, depth24stencil8 %d\n", [d supportsVertexAmplificationCount:2],
               d.depth24Stencil8PixelFormatSupported);
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.maxAnisotropy = 16;
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        printf("  anisotropic 16x sampler: %s\n", [d newSamplerStateWithDescriptor:sd] ? "made" : "refused");
        void *fx = dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY);
        Class sc = NSClassFromString(@"MTLFXSpatialScalerDescriptor");
        Class tc = NSClassFromString(@"MTLFXTemporalScalerDescriptor");
        SEL sup = NSSelectorFromString(@"supportsDevice:");
        printf("  MetalFX (upscaling) spatial %d temporal %d (framework %s)\n",
               sc && [sc respondsToSelector:sup] ? ((BOOL (*)(id, SEL, id))objc_msgSend)(sc, sup, d) : -1,
               tc && [tc respondsToSelector:sup] ? ((BOOL (*)(id, SEL, id))objc_msgSend)(tc, sup, d) : -1,
               fx ? "loaded" : "missing");
        // OpenGL renderers
        CGLRendererInfoObj ri;
        GLint nr = 0;
        if (CGLQueryRendererInfo(0xffffffff, &ri, &nr) == kCGLNoError) {
            for (GLint i = 0; i < nr; i++) {
                GLint id_ = 0, accel = 0, vram = 0, gl = 0;
                CGLDescribeRenderer(ri, i, kCGLRPRendererID, &id_);
                CGLDescribeRenderer(ri, i, kCGLRPAccelerated, &accel);
                CGLDescribeRenderer(ri, i, kCGLRPVideoMemoryMegabytes, &vram);
                CGLDescribeRenderer(ri, i, kCGLRPMajorGLVersion, &gl);
                printf("opengl renderer 0x%x accelerated %d vram %d MB GL major %d\n", id_, accel, vram, gl);
            }
            CGLDestroyRendererInfo(ri);
        } else printf("opengl: no renderer info\n");
        // OpenCL devices
        cl_platform_id p;
        cl_uint np = 0;
        if (clGetPlatformIDs(1, &p, &np) == CL_SUCCESS && np) {
            cl_device_id dv[8];
            cl_uint nd = 0;
            clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, 8, dv, &nd);
            for (cl_uint i = 0; i < nd; i++) {
                char name[128] = {0};
                cl_device_type t = 0;
                clGetDeviceInfo(dv[i], CL_DEVICE_NAME, sizeof name, name, NULL);
                clGetDeviceInfo(dv[i], CL_DEVICE_TYPE, sizeof t, &t, NULL);
                printf("opencl device: %s (%s)\n", name, t & CL_DEVICE_TYPE_GPU ? "GPU" : "CPU");
            }
        } else printf("opencl: no platform\n");
    }
    return 0;
}
