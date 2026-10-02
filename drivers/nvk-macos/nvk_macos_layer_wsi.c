/*
 * nvk_macos_layer_wsi: windowed Vulkan presentation (VK_EXT_metal_surface)
 * for NVK on macOS without a Metal device.
 *
 * The target has no macOS-supported GPU, so MTLCreateSystemDefaultDevice()
 * is nil and Mesa's own Metal WSI (a Metal blit into a CAMetalLayer
 * drawable) cannot run. This backend takes over the METAL surface slot:
 * swapchain images are linear host-visible images (NVK renders them through
 * its tiled shadow into system memory). vkQueuePresentKHR only queues the
 * image; a per-swapchain present thread waits for the image's fence, copies
 * it into an IOSurface, sets that IOSurface as the layer's contents
 * (WindowServer composites it like any other layer) and hands the image back
 * to vkAcquireNextImageKHR, so the app's next frame overlaps the GPU work
 * and the copy (the wsi_common_x11 queue pattern).
 *
 * FIFO pacing is emulated at the display rate (60 Hz) on the present thread.
 * Window resizes return VK_SUBOPTIMAL_KHR so the app recreates its swapchain.
 */
#ifdef VK_USE_PLATFORM_METAL_EXT

#include "vk_device.h"
#include "vk_instance.h"
#include "vk_log.h"
#include "vk_util.h"
#include "wsi_common_private.h"
#include "util/timespec.h"
#include "wsi_common_queue.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/u_thread.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <string.h>

VkResult nvk_macos_layer_wsi_init(struct wsi_device *wsi_device,
                                  const VkAllocationCallbacks *alloc);
void nvk_macos_layer_wsi_finish(struct wsi_device *wsi_device,
                                const VkAllocationCallbacks *alloc);

/* nvk_macos_layer.m */
void nvk_macos_layer_size(const void *layer, uint32_t *width, uint32_t *height);
void nvk_macos_layer_set_contents(const void *layer, IOSurfaceRef surface);

#define NVK_MACOS_LAYER_FRAME_NS 16666667ull   /* 60 Hz */
#define NVK_MACOS_LAYER_MAX_IMAGES 8

struct nvk_macos_layer_wsi {
   struct wsi_interface base;
   struct wsi_interface *mesa_metal;   /* restored at finish */
};

static const VkFormat layer_formats[] = {
   VK_FORMAT_B8G8R8A8_UNORM,
   VK_FORMAT_B8G8R8A8_SRGB,
};

static bool
layer_format_supported(VkFormat format)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(layer_formats); i++) {
      if (layer_formats[i] == format)
         return true;
   }
   return false;
}

static VkExtent2D
layer_extent(VkIcdSurfaceBase *icd_surface)
{
   VkIcdSurfaceMetal *surface = (VkIcdSurfaceMetal *)icd_surface;
   uint32_t w = 0, h = 0;
   nvk_macos_layer_size(surface->pLayer, &w, &h);
   return (VkExtent2D) { MAX2(w, 1), MAX2(h, 1) };
}

/* ------------------------------------------------ surface queries */

static VkResult
layer_get_support(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                  uint32_t queueFamilyIndex, VkBool32 *pSupported)
{
   *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

static VkResult
layer_get_capabilities2(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                        const void *info_next, VkSurfaceCapabilities2KHR *caps2)
{
   VkSurfaceCapabilitiesKHR *caps = &caps2->surfaceCapabilities;
   const VkExtent2D ext = layer_extent(surface);

   caps->minImageCount = 2;
   caps->maxImageCount = NVK_MACOS_LAYER_MAX_IMAGES;
   caps->currentExtent = ext;
   caps->minImageExtent = ext;
   caps->maxImageExtent = ext;
   caps->maxImageArrayLayers = 1;
   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   caps->supportedUsageFlags = wsi_caps_get_image_usage() |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   const VkSurfacePresentModeKHR *present_mode =
      vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_EXT);
   vk_foreach_struct(ext_s, caps2->pNext) {
      switch (ext_s->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *prot = (void *)ext_s;
         prot->supportsProtected = VK_FALSE;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         VkSurfacePresentScalingCapabilitiesKHR *scaling = (void *)ext_s;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent = ext;
         scaling->maxScaledImageExtent = ext;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext_s;
         if (compat->pPresentModes == NULL) {
            compat->presentModeCount = 1;
         } else if (compat->presentModeCount) {
            compat->presentModeCount = 1;
            compat->pPresentModes[0] = present_mode ? present_mode->presentMode
                                                    : VK_PRESENT_MODE_FIFO_KHR;
         }
         break;
      }
      default:
         break;
      }
   }
   return VK_SUCCESS;
}

static VkResult
layer_get_formats(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                  uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(layer_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = layer_formats[i];
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
layer_get_formats2(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                   const void *info_next, uint32_t *pSurfaceFormatCount,
                   VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(layer_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         f->surfaceFormat.format = layer_formats[i];
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static const VkPresentModeKHR layer_present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_IMMEDIATE_KHR,
};

static VkResult
layer_get_present_modes(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                        uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
   VK_OUTARRAY_MAKE_TYPED(VkPresentModeKHR, out, pPresentModes, pPresentModeCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(layer_present_modes); i++) {
      vk_outarray_append_typed(VkPresentModeKHR, &out, m)
         *m = layer_present_modes[i];
   }
   return vk_outarray_status(&out);
}

static VkResult
layer_get_present_rectangles(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                             uint32_t *pRectCount, VkRect2D *pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);
   vk_outarray_append_typed(VkRect2D, &out, r) {
      *r = (VkRect2D) { .offset = { 0, 0 }, .extent = layer_extent(surface) };
   }
   return vk_outarray_status(&out);
}

/* ------------------------------------------------------ swapchain */

struct layer_image {
   struct wsi_image base;
};

struct layer_swapchain {
   struct wsi_swapchain base;
   const void *layer;
   VkExtent2D extent;
   bool fifo;
   uint64_t last_present_ns;
   /* image_count + 1 IOSurfaces so a new frame never overwrites the one
    * WindowServer may still be compositing */
   uint32_t surface_count, next_surface;
   IOSurfaceRef surfaces[NVK_MACOS_LAYER_MAX_IMAGES + 1];
   /* present thread: present_queue = images to show, acquire_queue = images
    * the app may take (UINT32_MAX = the thread stopped on an error) */
   struct wsi_queue present_queue, acquire_queue;
   bool queues_ready, thread_started;
   thrd_t thread;
   VkResult status;   /* < 0 once the thread failed (atomic) */
   struct layer_image images[0];
};

static IOSurfaceRef
create_iosurface(uint32_t width, uint32_t height)
{
   const int32_t w = width, h = height, bpe = 4;
   const int32_t fmt = 'BGRA';
   CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &w);
   CFNumberRef nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &h);
   CFNumberRef nb = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe);
   CFNumberRef nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
   const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight,
                          kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
   const void *vals[] = { nw, nh, nb, nf };
   CFDictionaryRef props =
      CFDictionaryCreate(NULL, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
   IOSurfaceRef surface = props ? IOSurfaceCreate(props) : NULL;
   if (props)
      CFRelease(props);
   CFRelease(nw);
   CFRelease(nh);
   CFRelease(nb);
   CFRelease(nf);
   return surface;
}

static struct wsi_image *
layer_swapchain_get_wsi_image(struct wsi_swapchain *wsi_chain, uint32_t image_index)
{
   return &((struct layer_swapchain *)wsi_chain)->images[image_index].base;
}

static VkResult
layer_swapchain_release_images(struct wsi_swapchain *wsi_chain, uint32_t count,
                               const uint32_t *indices)
{
   struct layer_swapchain *chain = (struct layer_swapchain *)wsi_chain;
   for (uint32_t i = 0; i < count; i++)
      wsi_queue_push(&chain->acquire_queue, indices[i]);
   return VK_SUCCESS;
}

static VkResult
layer_swapchain_acquire_next_image(struct wsi_swapchain *wsi_chain,
                                   const VkAcquireNextImageInfoKHR *info,
                                   uint32_t *image_index)
{
   struct layer_swapchain *chain = (struct layer_swapchain *)wsi_chain;
   VkResult result = p_atomic_read(&chain->status);
   if (result < 0)
      return result;
   /* the present thread hands images back once they are copied out */
   result = wsi_queue_pull(&chain->acquire_queue, image_index, info->timeout);
   if (result == VK_TIMEOUT && info->timeout == 0)
      result = VK_NOT_READY;
   if (result != VK_SUCCESS)
      return result;
   if (*image_index == UINT32_MAX) {   /* the thread stopped */
      wsi_queue_push(&chain->acquire_queue, UINT32_MAX);
      result = p_atomic_read(&chain->status);
      return result < 0 ? result : VK_ERROR_OUT_OF_DATE_KHR;
   }
   return VK_SUCCESS;
}

/* Wait for the image's rendering, copy it into a free IOSurface, pace FIFO
 * and put the IOSurface on the layer. */
static VkResult
layer_show_image(struct layer_swapchain *chain, uint32_t image_index)
{
   struct layer_image *image = &chain->images[image_index];
   VkResult result =
      chain->base.wsi->WaitForFences(chain->base.device, 1,
                                     &chain->base.fences[image_index],
                                     VK_TRUE, UINT64_MAX);
   if (result != VK_SUCCESS)
      return result;

   /* next IOSurface that WindowServer is not using */
   IOSurfaceRef surface = NULL;
   for (uint32_t tries = 0; tries < chain->surface_count; tries++) {
      IOSurfaceRef s = chain->surfaces[chain->next_surface];
      chain->next_surface = (chain->next_surface + 1) % chain->surface_count;
      if (!IOSurfaceIsInUse(s)) {
         surface = s;
         break;
      }
   }
   if (surface == NULL)   /* all busy: reuse the oldest (may tear once) */
      surface = chain->surfaces[chain->next_surface];

   IOSurfaceLock(surface, 0, NULL);
   uint8_t *dst = IOSurfaceGetBaseAddress(surface);
   const size_t dst_pitch = IOSurfaceGetBytesPerRow(surface);
   const uint8_t *src = (const uint8_t *)image->base.cpu_map + image->base.offsets[0];
   const size_t src_pitch = image->base.row_pitches[0];
   const size_t row = (size_t)chain->extent.width * 4;
   for (uint32_t y = 0; y < chain->extent.height; y++)
      memcpy(dst + y * dst_pitch, src + y * src_pitch, row);
   IOSurfaceUnlock(surface, 0, NULL);

   if (chain->fifo && chain->last_present_ns) {
      const uint64_t due = chain->last_present_ns + NVK_MACOS_LAYER_FRAME_NS;
      const uint64_t now = os_time_get_nano();
      if (now < due)
         os_time_sleep((due - now) / 1000);
   }
   nvk_macos_layer_set_contents(chain->layer, surface);
   chain->last_present_ns = os_time_get_nano();
   return VK_SUCCESS;
}

static int
layer_present_thread(void *data)
{
   struct layer_swapchain *chain = data;
   u_thread_setname("nvk macOS present");
   for (;;) {
      uint32_t image_index = UINT32_MAX;
      if (wsi_queue_pull(&chain->present_queue, &image_index, INT64_MAX) != VK_SUCCESS ||
          image_index == UINT32_MAX)
         break;
      const VkResult result = layer_show_image(chain, image_index);
      if (result != VK_SUCCESS) {
         p_atomic_set(&chain->status, result);
         wsi_queue_push(&chain->acquire_queue, UINT32_MAX);
         break;
      }
      wsi_queue_push(&chain->acquire_queue, image_index);
   }
   return 0;
}

static VkResult
layer_swapchain_queue_present(struct wsi_swapchain *wsi_chain, uint32_t image_index,
                              uint64_t present_id, const VkPresentRegionKHR *damage)
{
   struct layer_swapchain *chain = (struct layer_swapchain *)wsi_chain;
   const VkResult status = p_atomic_read(&chain->status);
   if (status < 0)
      return status;
   wsi_queue_push(&chain->present_queue, image_index);

   /* the window changed size: keep presenting, ask for a new swapchain */
   uint32_t w = 0, h = 0;
   nvk_macos_layer_size(chain->layer, &w, &h);
   if (w != chain->extent.width || h != chain->extent.height)
      return VK_SUBOPTIMAL_KHR;
   return VK_SUCCESS;
}

static VkResult
layer_swapchain_wait_for_present(struct wsi_swapchain *wsi_chain,
                                 uint64_t present_id, uint64_t timeout)
{
   return wsi_swapchain_wait_for_present_semaphore(wsi_chain, present_id, timeout);
}

static VkResult
layer_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                        const VkAllocationCallbacks *pAllocator)
{
   struct layer_swapchain *chain = (struct layer_swapchain *)wsi_chain;
   if (chain->thread_started) {   /* shows what is queued, then exits */
      wsi_queue_push(&chain->present_queue, UINT32_MAX);
      thrd_join(chain->thread, NULL);
   }
   if (chain->queues_ready) {
      wsi_queue_destroy(&chain->acquire_queue);
      wsi_queue_destroy(&chain->present_queue);
   }
   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].base.image != VK_NULL_HANDLE)
         wsi_destroy_image(&chain->base, &chain->images[i].base);
   }
   /* The layer keeps its own reference to the IOSurface it shows. */
   for (uint32_t i = 0; i < chain->surface_count; i++)
      CFRelease(chain->surfaces[i]);
   wsi_swapchain_finish(&chain->base);
   vk_free(pAllocator, chain);
   return VK_SUCCESS;
}

static VkResult
layer_create_swapchain(VkIcdSurfaceBase *icd_surface, VkDevice _device,
                       struct wsi_device *wsi_device,
                       const VkSwapchainCreateInfoKHR *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       struct wsi_swapchain **swapchain_out)
{
   VK_FROM_HANDLE(vk_device, dev, _device);
   VkIcdSurfaceMetal *surface = (VkIcdSurfaceMetal *)icd_surface;

   if (!layer_format_supported(pCreateInfo->imageFormat))
      return vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);

   const uint32_t num_images = CLAMP(pCreateInfo->minImageCount, 2,
                                     NVK_MACOS_LAYER_MAX_IMAGES);
   const size_t size = sizeof(struct layer_swapchain) +
                       num_images * sizeof(struct layer_image);
   struct layer_swapchain *chain =
      vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* CPU params + wants_linear: linear host-visible images with cpu_map */
   struct wsi_cpu_image_params cpu_params = {
      .base.image_type = WSI_IMAGE_TYPE_CPU,
   };
   VkResult result = wsi_swapchain_init(wsi_device, &chain->base, _device,
                                        pCreateInfo, &cpu_params.base, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, chain);
      return result;
   }

   chain->base.destroy = layer_swapchain_destroy;
   chain->base.get_wsi_image = layer_swapchain_get_wsi_image;
   chain->base.acquire_next_image = layer_swapchain_acquire_next_image;
   chain->base.release_images = layer_swapchain_release_images;
   chain->base.queue_present = layer_swapchain_queue_present;
   chain->base.wait_for_present = layer_swapchain_wait_for_present;
   chain->base.wait_for_present2 = layer_swapchain_wait_for_present;
   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, pCreateInfo);
   chain->base.image_count = num_images;
   chain->layer = surface->pLayer;
   chain->extent = pCreateInfo->imageExtent;
   chain->fifo = chain->base.present_mode != VK_PRESENT_MODE_IMMEDIATE_KHR;

   chain->surface_count = num_images + 1;
   for (uint32_t i = 0; i < chain->surface_count; i++) {
      chain->surfaces[i] = create_iosurface(chain->extent.width, chain->extent.height);
      if (chain->surfaces[i] == NULL) {
         chain->surface_count = i;
         chain->base.image_count = 0;
         layer_swapchain_destroy(&chain->base, pAllocator);
         return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }

   for (uint32_t i = 0; i < num_images; i++) {
      result = wsi_create_image(&chain->base, &chain->base.image_info,
                                &chain->images[i].base);
      if (result != VK_SUCCESS) {
         /* wsi_create_image already destroyed image i */
         chain->base.image_count = i;
         layer_swapchain_destroy(&chain->base, pAllocator);
         return result;
      }
      if (chain->images[i].base.cpu_map == NULL) {   /* needs the CPU path */
         chain->base.image_count = i + 1;
         layer_swapchain_destroy(&chain->base, pAllocator);
         return vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);
      }
   }

   /* length image_count + 1: room for the UINT32_MAX stop marker */
   if (wsi_queue_init(&chain->present_queue, num_images + 1)) {
      layer_swapchain_destroy(&chain->base, pAllocator);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   if (wsi_queue_init(&chain->acquire_queue, num_images + 1)) {
      wsi_queue_destroy(&chain->present_queue);
      layer_swapchain_destroy(&chain->base, pAllocator);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   chain->queues_ready = true;
   for (uint32_t i = 0; i < num_images; i++)
      wsi_queue_push(&chain->acquire_queue, i);
   if (thrd_create(&chain->thread, layer_present_thread, chain) != thrd_success) {
      layer_swapchain_destroy(&chain->base, pAllocator);
      return vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);
   }
   chain->thread_started = true;

   *swapchain_out = &chain->base;
   return VK_SUCCESS;
}

/* ---------------------------------------------------------- setup */

VkResult
nvk_macos_layer_wsi_init(struct wsi_device *wsi_device, const VkAllocationCallbacks *alloc)
{
   struct nvk_macos_layer_wsi *wsi =
      vk_zalloc(alloc, sizeof(*wsi), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (wsi == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   wsi->base.get_support = layer_get_support;
   wsi->base.get_capabilities2 = layer_get_capabilities2;
   wsi->base.get_formats = layer_get_formats;
   wsi->base.get_formats2 = layer_get_formats2;
   wsi->base.get_present_modes = layer_get_present_modes;
   wsi->base.get_present_rectangles = layer_get_present_rectangles;
   wsi->base.create_swapchain = layer_create_swapchain;

   /* Mesa's Metal backend needs a Metal device, which this machine lacks;
    * keep it aside so wsi_metal_finish_wsi() frees what it allocated. */
   wsi->mesa_metal = wsi_device->wsi[VK_ICD_WSI_PLATFORM_METAL];
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_METAL] = &wsi->base;
   return VK_SUCCESS;
}

void
nvk_macos_layer_wsi_finish(struct wsi_device *wsi_device, const VkAllocationCallbacks *alloc)
{
   struct nvk_macos_layer_wsi *wsi =
      (struct nvk_macos_layer_wsi *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_METAL];
   if (wsi == NULL)
      return;
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_METAL] = wsi->mesa_metal;
   vk_free(alloc, wsi);
}

#endif /* VK_USE_PLATFORM_METAL_EXT */
