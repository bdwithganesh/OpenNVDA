// See NVMTLRender.m.
#import <Foundation/Foundation.h>
#import "NVMTLCompiler.h"

NVMTLKernel *nvRasterKernelForBGRA(bool bgra);

// vp = {x,y,w,h} doubles (MTLViewport, z ignored for now);
// sc = {x,y,w,h} NSUInteger (MTLScissorRect).
// RT staging (NVGSP_STAGE_DATA) must hold loaded/cleared bytes on entry.
// bgra selects the BGRA8 pack order (RGBA8 otherwise).
// Indexed draws pass idx/idxCount (uint32, NULL/0 = linear): the VS runs
// over vCount verts (max index + 1) and the raster remaps through idx.
bool nvRenderDraw(NVMTLKernel *vs, const float fsColor[4],
                  NSArray *vbufs,
                  const double *vp, const NSUInteger *sc,
                  uint32_t rtW, uint32_t rtH, uint32_t rtStride,
                  NSUInteger vStart, NSUInteger vCount, bool bgra,
                  const uint32_t *idx, NSUInteger idxCount);
