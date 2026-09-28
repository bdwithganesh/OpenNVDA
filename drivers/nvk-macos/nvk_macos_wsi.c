/*
 * nvk_macos_wsi: Vulkan presentation for NVK on macOS through the
 * NVGspControl kext (RTX 4080 / AD103), no DRM, no Metal.
 *
 * VK_KHR_display exposes the one head the kext drives (DP, 3840x2160 @ 60 Hz,
 * window 0). A display-plane surface gets a swapchain whose images are
 * pitch-linear, device-local VRAM objects; vkQueuePresentKHR waits for the
 * image's rendering fence and asks the kext (selector 27) to point window 0
 * at that image: zero-copy scan-out. NVK renders linear colour attachments
 * through its tiled shadow, so depth/MSAA work as usual.
 *
 * Pacing: the window channel accepts at most one flip per vblank
 * (MIN_PRESENT_INTERVAL 1), so when a flip returns the previous one has been
 * latched. The last two presented images (on screen + pending) are never
 * handed out by vkAcquireNextImageKHR; everything else is free (FIFO).
 * Destroying the swapchain, or the process exiting, gives the screen back to
 * the desktop surface (selector 28 / kext clientClose).
 */
#include "nvk_device_memory.h"
#include "nvk_entrypoints.h"
#include "nvkmd/nvkmd.h"

#include "vk_device.h"
#include "vk_instance.h"
#include "vk_util.h"
#include "wsi_common_private.h"
#include "util/os_time.h"

VkResult nvkmd_macos_mem_present(struct nvkmd_mem *mem, uint64_t offset_B,
                                 uint32_t pitch_B, uint32_t width,
                                 uint32_t height, uint32_t wnd_format);
void nvkmd_macos_present_stop(struct nvkmd_dev *dev);

VkResult nvk_macos_wsi_init(struct wsi_device *wsi_device,
                            const VkAllocationCallbacks *alloc);
void nvk_macos_wsi_finish(struct wsi_device *wsi_device,
                          const VkAllocationCallbacks *alloc);

/* The head the kext lights at boot (NVGspControl kDesktopSurface). */
#define NVK_MACOS_WIDTH        3840u
#define NVK_MACOS_HEIGHT       2160u
#define NVK_MACOS_REFRESH_MHZ  60000u
/* BenQ PD2705U, 27" 16:9 */
#define NVK_MACOS_WIDTH_MM     597u
#define NVK_MACOS_HEIGHT_MM    336u

/* Non-dispatchable handles for the single display and its single mode. */
static struct nvk_macos_display { int unused; } nvk_macos_display;
static struct nvk_macos_mode { int unused; } nvk_macos_mode;

static inline VkDisplayKHR
display_handle(void)
{
   return (VkDisplayKHR)(uintptr_t)&nvk_macos_display;
}

static inline VkDisplayModeKHR
mode_handle(void)
{
   return (VkDisplayModeKHR)(uintptr_t)&nvk_macos_mode;
}

/* ----------------------------------------------------- formats */

static const struct {
   VkFormat vk;
   uint32_t wnd;   /* NVC67E_SET_PARAMS_FORMAT */
} nvk_macos_formats[] = {
   { VK_FORMAT_B8G8R8A8_UNORM,           0xCF },  /* A8R8G8B8 */
   { VK_FORMAT_B8G8R8A8_SRGB,            0xCF },
   { VK_FORMAT_R8G8B8A8_UNORM,           0xD5 },  /* A8B8G8R8 */
   { VK_FORMAT_R8G8B8A8_SRGB,            0xD5 },
   { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0xD1 },  /* A2B10G10R10 */
};

static uint32_t
wnd_format(VkFormat format)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(nvk_macos_formats); i++) {
      if (nvk_macos_formats[i].vk == format)
         return nvk_macos_formats[i].wnd;
   }
   return 0;
}

/* ------------------------------------------------ VK_KHR_display */

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice physicalDevice,
                                          uint32_t *pPropertyCount,
                                          VkDisplayPropertiesKHR *pProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkDisplayPropertiesKHR, out, pProperties, pPropertyCount);
   vk_outarray_append_typed(VkDisplayPropertiesKHR, &out, p) {
      *p = (VkDisplayPropertiesKHR) {
         .display = display_handle(),
         .displayName = "NVIDIA RTX 4080 DP-0 (NVGspControl)",
         .physicalDimensions = { NVK_MACOS_WIDTH_MM, NVK_MACOS_HEIGHT_MM },
         .physicalResolution = { NVK_MACOS_WIDTH, NVK_MACOS_HEIGHT },
         .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
         .planeReorderPossible = VK_FALSE,
         .persistentContent = VK_FALSE,
      };
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice physicalDevice,
                                               uint32_t *pPropertyCount,
                                               VkDisplayPlanePropertiesKHR *pProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkDisplayPlanePropertiesKHR, out, pProperties, pPropertyCount);
   vk_outarray_append_typed(VkDisplayPlanePropertiesKHR, &out, p) {
      p->currentDisplay = display_handle();
      p->currentStackIndex = 0;
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice physicalDevice,
                                        uint32_t planeIndex,
                                        uint32_t *pDisplayCount,
                                        VkDisplayKHR *pDisplays)
{
   VK_OUTARRAY_MAKE_TYPED(VkDisplayKHR, out, pDisplays, pDisplayCount);
   if (planeIndex == 0) {
      vk_outarray_append_typed(VkDisplayKHR, &out, d)
         *d = display_handle();
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetDisplayModePropertiesKHR(VkPhysicalDevice physicalDevice,
                                VkDisplayKHR display,
                                uint32_t *pPropertyCount,
                                VkDisplayModePropertiesKHR *pProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkDisplayModePropertiesKHR, out, pProperties, pPropertyCount);
   if (display == display_handle()) {
      vk_outarray_append_typed(VkDisplayModePropertiesKHR, &out, p) {
         p->displayMode = mode_handle();
         p->parameters.visibleRegion = (VkExtent2D) { NVK_MACOS_WIDTH, NVK_MACOS_HEIGHT };
         p->parameters.refreshRate = NVK_MACOS_REFRESH_MHZ;
      }
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateDisplayModeKHR(VkPhysicalDevice physicalDevice,
                         VkDisplayKHR display,
                         const VkDisplayModeCreateInfoKHR *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkDisplayModeKHR *pMode)
{
   /* Mode setting is the kext's job (A3); only the current mode exists. */
   const VkDisplayModeParametersKHR *p = &pCreateInfo->parameters;
   if (display != display_handle() ||
       p->visibleRegion.width != NVK_MACOS_WIDTH ||
       p->visibleRegion.height != NVK_MACOS_HEIGHT ||
       (p->refreshRate && p->refreshRate != NVK_MACOS_REFRESH_MHZ))
      return VK_ERROR_INITIALIZATION_FAILED;
   *pMode = mode_handle();
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                   VkDisplayModeKHR mode,
                                   uint32_t planeIndex,
                                   VkDisplayPlaneCapabilitiesKHR *pCapabilities)
{
   const VkExtent2D full = { NVK_MACOS_WIDTH, NVK_MACOS_HEIGHT };
   *pCapabilities = (VkDisplayPlaneCapabilitiesKHR) {
      .supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .minSrcPosition = { 0, 0 },
      .maxSrcPosition = { 0, 0 },
      .minSrcExtent = full,
      .maxSrcExtent = full,
      .minDstPosition = { 0, 0 },
      .maxDstPosition = { 0, 0 },
      .minDstExtent = full,
      .maxDstExtent = full,
   };
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateDisplayPlaneSurfaceKHR(VkInstance _instance,
                                 const VkDisplaySurfaceCreateInfoKHR *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   if (pCreateInfo->displayMode != mode_handle() || pCreateInfo->planeIndex != 0)
      return vk_error(instance, VK_ERROR_INITIALIZATION_FAILED);

   VkIcdSurfaceDisplay *surface =
      vk_zalloc2(&instance->alloc, pAllocator, sizeof(*surface), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   surface->base.platform = VK_ICD_WSI_PLATFORM_DISPLAY;
   surface->displayMode = pCreateInfo->displayMode;
   surface->planeIndex = pCreateInfo->planeIndex;
   surface->planeStackIndex = pCreateInfo->planeStackIndex;
   surface->transform = pCreateInfo->transform;
   surface->globalAlpha = pCreateInfo->globalAlpha;
   surface->alphaMode = pCreateInfo->alphaMode;
   surface->imageExtent = (VkExtent2D) { NVK_MACOS_WIDTH, NVK_MACOS_HEIGHT };

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}

/* ------------------------------------------------ surface queries */

static VkResult
macos_surface_get_support(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                          uint32_t queueFamilyIndex, VkBool32 *pSupported)
{
   *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

static VkResult
macos_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                struct wsi_device *wsi_device,
                                const void *info_next,
                                VkSurfaceCapabilities2KHR *caps2)
{
   const VkExtent2D full = { NVK_MACOS_WIDTH, NVK_MACOS_HEIGHT };
   VkSurfaceCapabilitiesKHR *caps = &caps2->surfaceCapabilities;

   /* on screen + pending flip + one to render: triple buffering */
   caps->minImageCount = 3;
   caps->maxImageCount = 8;
   caps->currentExtent = full;
   caps->minImageExtent = full;
   caps->maxImageExtent = full;
   caps->maxImageArrayLayers = 1;
   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   caps->supportedUsageFlags = wsi_caps_get_image_usage() |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   const VkSurfacePresentModeKHR *present_mode =
      vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_EXT);

   vk_foreach_struct(ext, caps2->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *prot = (void *)ext;
         prot->supportsProtected = VK_FALSE;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         VkSurfacePresentScalingCapabilitiesKHR *scaling = (void *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent = full;
         scaling->maxScaledImageExtent = full;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext;
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
macos_surface_get_formats(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                          uint32_t *pSurfaceFormatCount,
                          VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(nvk_macos_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = nvk_macos_formats[i].vk;
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
macos_surface_get_formats2(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                           const void *info_next, uint32_t *pSurfaceFormatCount,
                           VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(nvk_macos_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         f->surfaceFormat.format = nvk_macos_formats[i].vk;
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

static VkResult
macos_surface_get_present_modes(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                                uint32_t *pPresentModeCount,
                                VkPresentModeKHR *pPresentModes)
{
   VK_OUTARRAY_MAKE_TYPED(VkPresentModeKHR, out, pPresentModes, pPresentModeCount);
   vk_outarray_append_typed(VkPresentModeKHR, &out, m)
      *m = VK_PRESENT_MODE_FIFO_KHR;
   return vk_outarray_status(&out);
}

static VkResult
macos_surface_get_present_rectangles(VkIcdSurfaceBase *surface,
                                     struct wsi_device *wsi_device,
                                     uint32_t *pRectCount, VkRect2D *pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);
   vk_outarray_append_typed(VkRect2D, &out, r) {
      *r = (VkRect2D) { .offset = { 0, 0 },
                        .extent = { NVK_MACOS_WIDTH, NVK_MACOS_HEIGHT } };
   }
   return vk_outarray_status(&out);
}

/* ------------------------------------------------------ swapchain */

struct nvk_macos_image {
   struct wsi_image base;
   bool busy_on_host;   /* acquired by the app, not yet presented */
};

struct nvk_macos_swapchain {
   struct wsi_swapchain base;
   struct nvkmd_dev *nvkmd;
   VkExtent2D extent;
   uint32_t wnd_format;
   /* shown[0]: latched on screen, shown[1]: flip pending; -1 = none */
   int32_t shown[2];
   bool presented;
   struct nvk_macos_image images[0];
};

static struct wsi_image *
macos_swapchain_get_wsi_image(struct wsi_swapchain *wsi_chain, uint32_t image_index)
{
   struct nvk_macos_swapchain *chain = (struct nvk_macos_swapchain *)wsi_chain;
   return &chain->images[image_index].base;
}

static VkResult
macos_swapchain_release_images(struct wsi_swapchain *wsi_chain,
                               uint32_t count, const uint32_t *indices)
{
   struct nvk_macos_swapchain *chain = (struct nvk_macos_swapchain *)wsi_chain;
   for (uint32_t i = 0; i < count; i++) {
      assert(indices[i] < chain->base.image_count);
      chain->images[indices[i]].busy_on_host = false;
   }
   return VK_SUCCESS;
}

static bool
image_on_screen(const struct nvk_macos_swapchain *chain, uint32_t i)
{
   return chain->shown[0] == (int32_t)i || chain->shown[1] == (int32_t)i;
}

static VkResult
macos_swapchain_acquire_next_image(struct wsi_swapchain *wsi_chain,
                                   const VkAcquireNextImageInfoKHR *info,
                                   uint32_t *image_index)
{
   struct nvk_macos_swapchain *chain = (struct nvk_macos_swapchain *)wsi_chain;
   const uint64_t start = os_time_get_nano();
   for (;;) {
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         if (!chain->images[i].busy_on_host && !image_on_screen(chain, i)) {
            chain->images[i].busy_on_host = true;
            *image_index = i;
            return VK_SUCCESS;
         }
      }
      /*
       * Images come back only through vkQueuePresentKHR on this thread's queue,
       * so an app holding every free image cannot make progress.
       */
      if (info->timeout == 0)
         return VK_NOT_READY;
      if (info->timeout != UINT64_MAX && os_time_get_nano() - start >= info->timeout)
         return VK_TIMEOUT;
      os_time_sleep(100);
   }
}

static VkResult
macos_swapchain_queue_present(struct wsi_swapchain *wsi_chain, uint32_t image_index,
                              uint64_t present_id, const VkPresentRegionKHR *damage)
{
   struct nvk_macos_swapchain *chain = (struct nvk_macos_swapchain *)wsi_chain;
   struct nvk_macos_image *image = &chain->images[image_index];
   assert(image_index < chain->base.image_count);
   image->busy_on_host = false;

   /*
    * The common code submitted this image's rendering with fences[i]; the
    * display must not scan it out before the GPU is done with it.
    */
   VkResult result =
      chain->base.wsi->WaitForFences(chain->base.device, 1,
                                     &chain->base.fences[image_index],
                                     VK_TRUE, UINT64_MAX);
   if (result != VK_SUCCESS)
      return result;

   VK_FROM_HANDLE(nvk_device_memory, mem, image->base.memory);
   result = nvkmd_macos_mem_present(mem->mem, image->base.offsets[0],
                                    image->base.row_pitches[0],
                                    chain->extent.width, chain->extent.height,
                                    chain->wnd_format);
   if (result != VK_SUCCESS)
      return result;

   /* The flip call returns once the previous flip latched. */
   chain->shown[0] = chain->shown[1];
   chain->shown[1] = (int32_t)image_index;
   chain->presented = true;
   return VK_SUCCESS;
}

static VkResult
macos_swapchain_wait_for_present(struct wsi_swapchain *wsi_chain,
                                 uint64_t present_id, uint64_t timeout)
{
   return wsi_swapchain_wait_for_present_semaphore(wsi_chain, present_id, timeout);
}

static VkResult
macos_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                        const VkAllocationCallbacks *pAllocator)
{
   struct nvk_macos_swapchain *chain = (struct nvk_macos_swapchain *)wsi_chain;

   /* Hand the screen back before the scanned-out images are freed. */
   if (chain->presented)
      nvkmd_macos_present_stop(chain->nvkmd);

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].base.image != VK_NULL_HANDLE)
         wsi_destroy_image(&chain->base, &chain->images[i].base);
   }
   wsi_swapchain_finish(&chain->base);
   vk_free(pAllocator, chain);
   return VK_SUCCESS;
}

/*
 * Swapchain images: linear, dedicated, device-local (VRAM) so window 0 can scan
 * them out directly.
 */
static VkResult
macos_create_vram_image_mem(const struct wsi_swapchain *chain,
                            const struct wsi_image_info *info,
                            struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;

   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->device, image->image, &reqs);

   VkSubresourceLayout layout;
   wsi->GetImageSubresourceLayout(chain->device, image->image,
                                  &(VkImageSubresource) {
                                     .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                     .mipLevel = 0,
                                     .arrayLayer = 0,
                                  }, &layout);

   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->image,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated,
      .allocationSize = reqs.size,
      .memoryTypeIndex = wsi_select_device_memory_type(wsi, reqs.memoryTypeBits),
   };
   VkResult result = wsi->AllocateMemory(chain->device, &memory_info,
                                         &chain->alloc, &image->memory);
   if (result != VK_SUCCESS)
      return result;

   image->num_planes = 1;
   image->sizes[0] = reqs.size;
   image->row_pitches[0] = layout.rowPitch;
   image->offsets[0] = layout.offset;
   return VK_SUCCESS;
}

static VkResult
macos_surface_create_swapchain(VkIcdSurfaceBase *icd_surface, VkDevice _device,
                               struct wsi_device *wsi_device,
                               const VkSwapchainCreateInfoKHR *pCreateInfo,
                               const VkAllocationCallbacks *pAllocator,
                               struct wsi_swapchain **swapchain_out)
{
   VK_FROM_HANDLE(vk_device, dev, _device);
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   const uint32_t fmt = wnd_format(pCreateInfo->imageFormat);
   if (!fmt || pCreateInfo->imageExtent.width != NVK_MACOS_WIDTH ||
       pCreateInfo->imageExtent.height != NVK_MACOS_HEIGHT)
      return vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);

   const uint32_t num_images = MAX2(pCreateInfo->minImageCount, 3);
   const size_t size = sizeof(struct nvk_macos_swapchain) +
                       num_images * sizeof(struct nvk_macos_image);
   struct nvk_macos_swapchain *chain =
      vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   /*
    * CPU image params + wsi_device->wants_linear = linear images, no blit; the
    * memory callback is replaced with a VRAM one below.
    */
   struct wsi_cpu_image_params cpu_params = {
      .base.image_type = WSI_IMAGE_TYPE_CPU,
   };
   VkResult result = wsi_swapchain_init(wsi_device, &chain->base, _device,
                                        pCreateInfo, &cpu_params.base, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, chain);
      return result;
   }
   if (chain->base.blit.type != WSI_SWAPCHAIN_NO_BLIT) {
      /* e.g. MESA_VK_WSI_DEBUG=buffer: that path scans out host memory */
      wsi_swapchain_finish(&chain->base);
      vk_free(pAllocator, chain);
      return vk_error(dev, VK_ERROR_INITIALIZATION_FAILED);
   }
   assert(chain->base.image_info.create.tiling == VK_IMAGE_TILING_LINEAR);
   chain->base.image_info.create_mem = macos_create_vram_image_mem;

   chain->base.destroy = macos_swapchain_destroy;
   chain->base.get_wsi_image = macos_swapchain_get_wsi_image;
   chain->base.acquire_next_image = macos_swapchain_acquire_next_image;
   chain->base.release_images = macos_swapchain_release_images;
   chain->base.queue_present = macos_swapchain_queue_present;
   chain->base.wait_for_present = macos_swapchain_wait_for_present;
   chain->base.wait_for_present2 = macos_swapchain_wait_for_present;
   chain->base.present_mode = VK_PRESENT_MODE_FIFO_KHR;
   chain->base.image_count = num_images;
   chain->extent = pCreateInfo->imageExtent;
   chain->wnd_format = fmt;
   chain->shown[0] = chain->shown[1] = -1;

   for (uint32_t i = 0; i < num_images; i++) {
      result = wsi_create_image(&chain->base, &chain->base.image_info,
                                &chain->images[i].base);
      if (result != VK_SUCCESS) {
         chain->base.image_count = i;
         macos_swapchain_destroy(&chain->base, pAllocator);
         return result;
      }
   }
   VK_FROM_HANDLE(nvk_device_memory, mem0, chain->images[0].base.memory);
   chain->nvkmd = mem0->mem->dev;

   *swapchain_out = &chain->base;
   return VK_SUCCESS;
}

/* ---------------------------------------------------------- setup */

VkResult
nvk_macos_wsi_init(struct wsi_device *wsi_device, const VkAllocationCallbacks *alloc)
{
   struct wsi_interface *iface =
      vk_zalloc(alloc, sizeof(*iface), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (iface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   iface->get_support = macos_surface_get_support;
   iface->get_capabilities2 = macos_surface_get_capabilities2;
   iface->get_formats = macos_surface_get_formats;
   iface->get_formats2 = macos_surface_get_formats2;
   iface->get_present_modes = macos_surface_get_present_modes;
   iface->get_present_rectangles = macos_surface_get_present_rectangles;
   iface->create_swapchain = macos_surface_create_swapchain;

   /* No DRM display backend on macOS, so the DISPLAY slot is free. */
   assert(wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] == NULL);
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] = iface;
   wsi_device->wants_linear = true;
   return VK_SUCCESS;
}

void
nvk_macos_wsi_finish(struct wsi_device *wsi_device, const VkAllocationCallbacks *alloc)
{
   vk_free(alloc, wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY]);
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_DISPLAY] = NULL;
}
