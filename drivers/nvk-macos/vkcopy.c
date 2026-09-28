/*
 * vkcopy: transfer-queue test for NVK on macOS (the copy-engine ring, kext
 * selectors 21 engine 1 + 29). Needs NVK_MACOS_CE=1, which exposes a
 * transfer-only queue family backed by the CE channel. Copies a pattern
 * host buffer -> device-local buffer -> second host buffer on that queue,
 * plus a vkCmdFillBuffer, checks every byte and prints the copy rate.
 *
 *   cc -O2 vkcopy.c -I<Vulkan-Headers>/include -L<loader> -lvulkan -o vkcopy
 *   sudo env NVK_MACOS_CE=1 VK_ICD_FILENAMES=<icd.json> ./vkcopy [MiB] [rounds] [x]
 *
 * With "x" the second copy (device-local -> host) and the fill run on a
 * graphics queue that waits on a semaphore the transfer queue signals:
 * a GR submission depending on CE work (NVK_MACOS_GPU_WAIT=1 turns that
 * into a GPU-side semaphore acquire instead of a CPU wait).
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

static VkPhysicalDeviceMemoryProperties mem_props;

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type 0x%x in 0x%x\n", want, bits);
   exit(1);
}

static void
make_buffer(VkDevice dev, VkDeviceSize size, VkMemoryPropertyFlags props,
            VkBuffer *buf, VkDeviceMemory *mem)
{
   const VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   CHECK(vkCreateBuffer(dev, &bci, NULL, buf));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, *buf, &req);
   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
      .memoryTypeIndex = mem_type(req.memoryTypeBits, props),
   };
   CHECK(vkAllocateMemory(dev, &mai, NULL, mem));
   CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
}

int
main(int argc, char **argv)
{
   const VkDeviceSize size = (VkDeviceSize)(argc > 1 ? atoi(argv[1]) : 64) << 20;
   const int rounds = argc > 2 ? atoi(argv[2]) : 20;
   const int cross = argc > 3 && argv[3][0] == 'x';
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vkcopy", .apiVersion = VK_API_VERSION_1_3,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
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
   vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);

   VkQueueFamilyProperties qf[8];
   n = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);
   uint32_t fam = UINT32_MAX;
   for (uint32_t i = 0; i < n; i++)
      if ((qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
          !(qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
         fam = i;
   if (fam == UINT32_MAX) {
      fprintf(stderr, "no transfer-only queue family (run with NVK_MACOS_CE=1)\n");
      return 2;
   }
   printf("transfer-only queue family %u (%u queues)\n", fam, qf[fam].queueCount);
   uint32_t gfam = UINT32_MAX;
   for (uint32_t i = 0; i < n; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
         gfam = i;
   if (cross && gfam == UINT32_MAX) {
      fprintf(stderr, "no graphics queue family\n");
      return 2;
   }

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qci[2] = {
      { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = fam, .queueCount = 1, .pQueuePriorities = &prio },
      { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gfam, .queueCount = 1, .pQueuePriorities = &prio },
   };
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = cross ? 2 : 1, .pQueueCreateInfos = qci,
   };
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   VkQueue q, gq = VK_NULL_HANDLE;
   vkGetDeviceQueue(dev, fam, 0, &q);
   if (cross)
      vkGetDeviceQueue(dev, gfam, 0, &gq);

   VkBuffer src, mid, dst;
   VkDeviceMemory src_m, mid_m, dst_m;
   const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   make_buffer(dev, size, host, &src, &src_m);
   make_buffer(dev, size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &mid, &mid_m);
   make_buffer(dev, size, host, &dst, &dst_m);
   uint32_t *s = NULL, *d = NULL;
   CHECK(vkMapMemory(dev, src_m, 0, size, 0, (void **)&s));
   CHECK(vkMapMemory(dev, dst_m, 0, size, 0, (void **)&d));

   const VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = fam,
   };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBuffer cb, gcb = VK_NULL_HANDLE;
   const VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
   };
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkCommandPool gpool = VK_NULL_HANDLE;
   VkSemaphore sem = VK_NULL_HANDLE;
   if (cross) {
      const VkCommandPoolCreateInfo gpci = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = gfam,
      };
      CHECK(vkCreateCommandPool(dev, &gpci, NULL, &gpool));
      const VkCommandBufferAllocateInfo gcai = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = gpool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
      };
      CHECK(vkAllocateCommandBuffers(dev, &gcai, &gcb));
      const VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
      CHECK(vkCreateSemaphore(dev, &sci, NULL, &sem));
   }
   const VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence;
   CHECK(vkCreateFence(dev, &fi, NULL, &fence));

   const size_t words = size / 4;
   int bad_rounds = 0;
   double busy = 0;
   for (int r = 0; r < rounds; r++) {
      for (size_t i = 0; i < words; i++)
         s[i] = (uint32_t)(i * 2654435761u) ^ (uint32_t)r;
      memset(d, 0, size);
      const VkCommandBufferBeginInfo bi = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK(vkResetCommandBuffer(cb, 0));
      CHECK(vkBeginCommandBuffer(cb, &bi));
      const VkBufferCopy all = { 0, 0, size };
      vkCmdCopyBuffer(cb, src, mid, 1, &all);
      const VkMemoryBarrier mb = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      };
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 1, &mb, 0, NULL, 0, NULL);
      /* cross: the second half runs on the graphics queue after `sem` */
      VkCommandBuffer tail = cb;
      if (cross) {
         CHECK(vkEndCommandBuffer(cb));
         CHECK(vkResetCommandBuffer(gcb, 0));
         CHECK(vkBeginCommandBuffer(gcb, &bi));
         tail = gcb;
      }
      vkCmdCopyBuffer(tail, mid, dst, 1, &all);
      vkCmdPipelineBarrier(tail, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 1, &mb, 0, NULL, 0, NULL);
      /* last 4 KiB: fill with a marker */
      vkCmdFillBuffer(tail, dst, size - 4096, 4096, 0xc0ffee00u | (uint32_t)r);
      CHECK(vkEndCommandBuffer(tail));
      const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      const VkSubmitInfo sub = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb,
         .signalSemaphoreCount = cross ? 1 : 0, .pSignalSemaphores = &sem,
      };
      const VkSubmitInfo gsub = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
         .pWaitSemaphores = &sem, .pWaitDstStageMask = &wait_stage,
         .commandBufferCount = 1, .pCommandBuffers = &gcb,
      };
      const double t0 = now_s();
      if (cross) {
         CHECK(vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE));
         CHECK(vkQueueSubmit(gq, 1, &gsub, fence));
      } else {
         CHECK(vkQueueSubmit(q, 1, &sub, fence));
      }
      CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
      busy += now_s() - t0;
      CHECK(vkResetFences(dev, 1, &fence));
      size_t bad = 0;
      for (size_t i = 0; i < words; i++) {
         const uint32_t want = i >= (size - 4096) / 4 ? (0xc0ffee00u | (uint32_t)r)
                                                       : ((uint32_t)(i * 2654435761u) ^ (uint32_t)r);
         bad += d[i] != want;
      }
      if (bad) {
         ++bad_rounds;
         fprintf(stderr, "round %d: %zu of %zu words wrong\n", r, bad, words);
      }
   }
   /* two copies per round */
   printf("vkcopy%s: %d rounds x 2 x %llu MiB, %.2f GB/s, %s (%d bad rounds)\n",
          cross ? " (transfer -> graphics queue)" : "", rounds,
          (unsigned long long)(size >> 20), 2.0 * rounds * size / busy / 1e9,
          bad_rounds ? "FAIL" : "PASS", bad_rounds);
   if (cross) {
      vkDestroySemaphore(dev, sem, NULL);
      vkDestroyCommandPool(dev, gpool, NULL);
   }
   vkDestroyFence(dev, fence, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkUnmapMemory(dev, src_m);
   vkUnmapMemory(dev, dst_m);
   vkDestroyBuffer(dev, src, NULL);
   vkDestroyBuffer(dev, mid, NULL);
   vkDestroyBuffer(dev, dst, NULL);
   vkFreeMemory(dev, src_m, NULL);
   vkFreeMemory(dev, mid_m, NULL);
   vkFreeMemory(dev, dst_m, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return bad_rounds != 0;
}
