/*
 * vkpresent: VK_KHR_display present test for NVK on macOS (nvk_macos_wsi).
 * Clears each 3840x2160 swapchain image with a cycling colour and draws a
 * moving white bar with vkCmdClearAttachments inside dynamic rendering
 * (exercises NVK's linear-image tiled shadow), presents N frames FIFO and
 * prints the frame rate. Esc is not needed: it exits after N frames and
 * the swapchain destroy hands the screen back to the desktop.
 *
 *   cc -O2 vkpresent.c -I<Vulkan-Headers>/include -L<loader> -lvulkan -o vkpresent
 *   sudo env VK_ICD_FILENAMES=<nouveau_devenv_icd.x86_64.json> ./vkpresent [frames]
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static double
now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int
main(int argc, char **argv)
{
   const uint32_t frames = argc > 1 ? (uint32_t)atoi(argv[1]) : 600;
   const char *inst_ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vkpresent", .apiVersion = VK_API_VERSION_1_3,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
      .enabledExtensionCount = 2, .ppEnabledExtensionNames = inst_ext,
   };
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkPhysicalDevice pd;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) {
      fprintf(stderr, "no physical device\n");
      return 1;
   }

   /* display -> mode -> plane surface */
   VkDisplayPropertiesKHR dp;
   n = 1;
   CHECK(vkGetPhysicalDeviceDisplayPropertiesKHR(pd, &n, &dp));
   VkDisplayModePropertiesKHR mp;
   n = 1;
   CHECK(vkGetDisplayModePropertiesKHR(pd, dp.display, &n, &mp));
   printf("display: %s, %ux%u @ %.2f Hz\n", dp.displayName,
          mp.parameters.visibleRegion.width, mp.parameters.visibleRegion.height,
          mp.parameters.refreshRate / 1000.0);
   const VkDisplaySurfaceCreateInfoKHR dsci = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = mp.displayMode, .planeIndex = 0,
      .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .globalAlpha = 1.0f,
      .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .imageExtent = mp.parameters.visibleRegion,
   };
   VkSurfaceKHR surf;
   CHECK(vkCreateDisplayPlaneSurfaceKHR(inst, &dsci, NULL, &surf));
   VkBool32 supported = VK_FALSE;
   CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &supported));
   VkSurfaceCapabilitiesKHR caps;
   CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));

   /* device with swapchain + dynamic rendering */
   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio,
   };
   const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
   const VkPhysicalDeviceVulkan13Features f13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
   };
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_ext,
   };
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   const VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
   const VkSwapchainCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
      .minImageCount = caps.minImageCount, .imageFormat = fmt,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = caps.currentExtent, .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
   };
   VkSwapchainKHR sc;
   CHECK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
   uint32_t ni = 0;
   CHECK(vkGetSwapchainImagesKHR(dev, sc, &ni, NULL));
   VkImage *img = calloc(ni, sizeof(*img));
   VkImageView *view = calloc(ni, sizeof(*view));
   CHECK(vkGetSwapchainImagesKHR(dev, sc, &ni, img));
   for (uint32_t i = 0; i < ni; i++) {
      const VkImageViewCreateInfo vci = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[i],
         .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
      CHECK(vkCreateImageView(dev, &vci, NULL, &view[i]));
   }
   printf("swapchain: %u images %ux%u\n", ni, caps.currentExtent.width,
          caps.currentExtent.height);

   const VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
   };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   enum { IN_FLIGHT = 2 };
   VkCommandBuffer cb[IN_FLIGHT];
   const VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = IN_FLIGHT,
   };
   CHECK(vkAllocateCommandBuffers(dev, &cai, cb));
   VkSemaphore acq[IN_FLIGHT], done[IN_FLIGHT];
   VkFence fence[IN_FLIGHT];
   for (int i = 0; i < IN_FLIGHT; i++) {
      const VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      const VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                     .flags = VK_FENCE_CREATE_SIGNALED_BIT };
      CHECK(vkCreateSemaphore(dev, &si, NULL, &acq[i]));
      CHECK(vkCreateSemaphore(dev, &si, NULL, &done[i]));
      CHECK(vkCreateFence(dev, &fi, NULL, &fence[i]));
   }

   const uint32_t W = caps.currentExtent.width, H = caps.currentExtent.height;
   const double t0 = now_s();
   for (uint32_t f = 0; f < frames; f++) {
      const int s = f % IN_FLIGHT;
      CHECK(vkWaitForFences(dev, 1, &fence[s], VK_TRUE, UINT64_MAX));
      CHECK(vkResetFences(dev, 1, &fence[s]));
      uint32_t idx;
      CHECK(vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq[s], VK_NULL_HANDLE, &idx));

      const VkCommandBufferBeginInfo bi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK(vkResetCommandBuffer(cb[s], 0));
      CHECK(vkBeginCommandBuffer(cb[s], &bi));
      VkImageMemoryBarrier2 to_att = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .image = img[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
      VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &to_att };
      vkCmdPipelineBarrier2(cb[s], &dep);

      const float ph = (float)(f % 240) / 240.0f;
      const VkRenderingAttachmentInfo att = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view[idx],
         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .clearValue.color.float32 = { 0.1f + 0.4f * ph, 0.2f, 0.5f - 0.4f * ph, 1.0f },
      };
      const VkRenderingInfo ri = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1,
         .colorAttachmentCount = 1, .pColorAttachments = &att,
      };
      vkCmdBeginRendering(cb[s], &ri);
      const VkClearAttachment bar = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
         .clearValue.color.float32 = { 1, 1, 1, 1 },
      };
      const uint32_t bw = W / 16, x = (uint32_t)((W - bw) * ph);
      const VkClearRect rect = { { { (int32_t)x, 0 }, { bw, H } }, 0, 1 };
      vkCmdClearAttachments(cb[s], 1, &bar, 1, &rect);
      vkCmdEndRendering(cb[s]);

      VkImageMemoryBarrier2 to_present = to_att;
      to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
      to_present.dstAccessMask = 0;
      to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      dep.pImageMemoryBarriers = &to_present;
      vkCmdPipelineBarrier2(cb[s], &dep);
      CHECK(vkEndCommandBuffer(cb[s]));

      const VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      const VkSubmitInfo sub = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = 1, .pWaitSemaphores = &acq[s], .pWaitDstStageMask = &ws,
         .commandBufferCount = 1, .pCommandBuffers = &cb[s],
         .signalSemaphoreCount = 1, .pSignalSemaphores = &done[s],
      };
      CHECK(vkQueueSubmit(q, 1, &sub, fence[s]));
      const VkPresentInfoKHR pi = {
         .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
         .waitSemaphoreCount = 1, .pWaitSemaphores = &done[s],
         .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx,
      };
      CHECK(vkQueuePresentKHR(q, &pi));
   }
   CHECK(vkDeviceWaitIdle(dev));
   const double dt = now_s() - t0;
   printf("vkpresent: %u frames in %.2f s = %.1f fps: PASS\n", frames, dt, frames / dt);

   for (int i = 0; i < IN_FLIGHT; i++) {
      vkDestroySemaphore(dev, acq[i], NULL);
      vkDestroySemaphore(dev, done[i], NULL);
      vkDestroyFence(dev, fence[i], NULL);
   }
   vkDestroyCommandPool(dev, pool, NULL);
   for (uint32_t i = 0; i < ni; i++)
      vkDestroyImageView(dev, view[i], NULL);
   vkDestroySwapchainKHR(dev, sc, NULL);   /* screen back to the desktop */
   vkDestroyDevice(dev, NULL);
   vkDestroySurfaceKHR(inst, surf, NULL);
   vkDestroyInstance(inst, NULL);
   free(img);
   free(view);
   return 0;
}
