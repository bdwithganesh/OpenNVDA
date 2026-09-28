/*
 * vkwindow: windowed Vulkan test for NVK on macOS (VK_EXT_metal_surface via
 * nvk_macos_layer_wsi: IOSurface layer contents, no Metal device).
 * Opens a 1280x720 window, clears each frame to a cycling colour plus a
 * moving white bar (vkCmdClearColorImage), presents FIFO, prints fps every
 * 120 frames. Resize the window to exercise swapchain recreation; close it
 * or wait for N frames to exit.
 *
 *   clang -O2 -fobjc-arc vkwindow.m -I<Vulkan-Headers>/include -L<loader> -lvulkan \
 *         -framework Cocoa -framework QuartzCore -o vkwindow
 *   sudo env VK_ICD_FILENAMES=<nouveau_devenv_icd.x86_64.json> ./vkwindow [frames]
 */
#define VK_USE_PLATFORM_METAL_EXT
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkSurfaceKHR surf;
static VkSwapchainKHR sc;
static VkImage img[8];
static uint32_t ni;
static VkExtent2D ext;
static VkCommandPool pool;
static VkCommandBuffer cb[2];
static VkSemaphore acq[2], done[2];
static VkFence fence[2];
static uint32_t frame, frames = 1200;
static double t_last;

static double
now_s(void)
{
   return [NSDate timeIntervalSinceReferenceDate];
}

static void
create_swapchain(void)
{
   VkSurfaceCapabilitiesKHR caps;
   CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
   ext = caps.currentExtent;
   VkSwapchainKHR old = sc;
   const VkSwapchainCreateInfoKHR sci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
      .minImageCount = MAX(caps.minImageCount, 3), .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = ext,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = old,
   };
   CHECK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
   if (old)
      vkDestroySwapchainKHR(dev, old, NULL);
   ni = 8;
   CHECK(vkGetSwapchainImagesKHR(dev, sc, &ni, img));
   printf("swapchain %ux%u, %u images\n", ext.width, ext.height, ni);
}

static void
draw_frame(void)
{
   const int s = frame % 2;
   CHECK(vkWaitForFences(dev, 1, &fence[s], VK_TRUE, UINT64_MAX));
   uint32_t idx;
   VkResult r = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq[s], VK_NULL_HANDLE, &idx);
   if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      CHECK(vkDeviceWaitIdle(dev));
      create_swapchain();
      return;
   }
   CHECK(r == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : r);
   CHECK(vkResetFences(dev, 1, &fence[s]));

   const VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(vkResetCommandBuffer(cb[s], 0));
   CHECK(vkBeginCommandBuffer(cb[s], &bi));
   const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img[idx], .subresourceRange = range,
   };
   vkCmdPipelineBarrier(cb[s], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   const float ph = (float)(frame % 180) / 180.0f;
   const VkClearColorValue bg = { .float32 = { 0.1f + 0.5f * ph, 0.3f, 0.6f - 0.5f * ph, 1 } };
   vkCmdClearColorImage(cb[s], img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &bg, 1, &range);
   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b.dstAccessMask = 0;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   vkCmdPipelineBarrier(cb[s], VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   CHECK(vkEndCommandBuffer(cb[s]));

   const VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
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
   r = vkQueuePresentKHR(q, &pi);
   if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
      CHECK(vkDeviceWaitIdle(dev));
      create_swapchain();
   } else {
      CHECK(r);
   }
   if (++frame % 120 == 0) {
      const double t = now_s();
      printf("frame %u: %.1f fps\n", frame, 120 / (t - t_last));
      t_last = t;
   }
   if (frame >= frames) {
      CHECK(vkDeviceWaitIdle(dev));
      printf("vkwindow: %u frames: PASS\n", frame);
      exit(0);
   }
}

@interface VkView : NSView
@end
@implementation VkView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(strong) NSWindow *window;
@end
@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n
{
   self.window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(100, 100, 1280, 720)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered defer:NO];
   self.window.title = @"NVK on RTX 4080 (macOS)";
   VkView *view = [[VkView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
   view.wantsLayer = YES;
   self.window.contentView = view;
   [self.window makeKeyAndOrderFront:nil];
   [NSApp activateIgnoringOtherApps:YES];

   const VkMetalSurfaceCreateInfoEXT msci = {
      .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
      .pLayer = (CAMetalLayer *)view.layer,
   };
   CHECK(vkCreateMetalSurfaceEXT(inst, &msci, NULL, &surf));
   VkBool32 ok = VK_FALSE;
   CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &ok));
   create_swapchain();
   t_last = now_s();
   [NSTimer scheduledTimerWithTimeInterval:0 repeats:YES block:^(NSTimer *t) { draw_frame(); }];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }
@end

int
main(int argc, char **argv)
{
   if (argc > 1)
      frames = (uint32_t)atoi(argv[1]);
   const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME };
   const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .pApplicationName = "vkwindow",
                                   .apiVersion = VK_API_VERSION_1_3 };
   const VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pApplicationInfo = &app,
                                      .enabledExtensionCount = 2,
                                      .ppEnabledExtensionNames = iext };
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) {
      fprintf(stderr, "no physical device\n");
      return 1;
   }
   const float prio = 1;
   const VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueCount = 1, .pQueuePriorities = &prio };
   const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
   const VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                                    .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);
   const VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
   };
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   const VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2,
   };
   CHECK(vkAllocateCommandBuffers(dev, &cai, cb));
   for (int i = 0; i < 2; i++) {
      const VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      const VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                     .flags = VK_FENCE_CREATE_SIGNALED_BIT };
      CHECK(vkCreateSemaphore(dev, &si, NULL, &acq[i]));
      CHECK(vkCreateSemaphore(dev, &si, NULL, &done[i]));
      CHECK(vkCreateFence(dev, &fi, NULL, &fence[i]));
   }

   @autoreleasepool {
      NSApplication *a = [NSApplication sharedApplication];
      [a setActivationPolicy:NSApplicationActivationPolicyRegular];
      AppDelegate *d = [AppDelegate new];
      a.delegate = d;
      [a run];
   }
   return 0;
}
