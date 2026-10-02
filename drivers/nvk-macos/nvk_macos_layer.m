/*
 * nvk_macos_layer.m: the CALayer side of nvk_macos_layer_wsi.c (no Metal
 * device needed). The layer is whatever the app passed as
 * VkMetalSurfaceCreateInfoEXT::pLayer (normally a CAMetalLayer); only its
 * CALayer contents are used.
 */
#import <QuartzCore/QuartzCore.h>
#include <IOSurface/IOSurfaceRef.h>
#include <stdint.h>

void nvk_macos_layer_size(const void *layer, uint32_t *width, uint32_t *height);
void nvk_macos_layer_set_contents(const void *layer, IOSurfaceRef surface);

void
nvk_macos_layer_size(const void *layer, uint32_t *width, uint32_t *height)
{
   @autoreleasepool {
      CALayer *l = (__bridge CALayer *)layer;
      /* bounds in points × backing scale = pixels (like Mesa's Metal WSI) */
      CGSize size = l.bounds.size;
      const CGFloat scale = l.contentsScale;
      *width = (uint32_t)(size.width * scale);
      *height = (uint32_t)(size.height * scale);
   }
}

void
nvk_macos_layer_set_contents(const void *layer, IOSurfaceRef surface)
{
   @autoreleasepool {
      CALayer *l = (__bridge CALayer *)layer;
      /* Explicit transaction: presents usually come from the app's render
       * thread, which has no run loop committing implicit transactions. */
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      l.contentsGravity = kCAGravityResize;
      /* nvk_macos_layer_wsi rotates IOSurfaces, so every present hands CA a
       * different contents object and it re-reads the pixels. */
      l.contents = (__bridge id)surface;
      [CATransaction commit];
      [CATransaction flush];
   }
}
