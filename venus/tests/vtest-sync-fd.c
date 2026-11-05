/*
 * Host test for sync fd export over vtest (VCMD_SYNC_EXPORT_FD, protocol 7).
 *
 * Runs against Venus (VK_ICD_FILENAMES=virtio ICD, VTEST_SOCKET_NAME=...).
 * For a VkFence and a VkSemaphore exported as SYNC_FD it checks that:
 * - export returns quickly (it does not wait for the GPU work);
 * - the fd is a real sync_file (SYNC_IOC_FILE_INFO works) and is not yet
 *   signaled right after export while the GPU is busy;
 * - the fd signals once the work is done (time compared to the work length);
 * - after export the fence stays reset even when the old work completes
 *   (no stale signal from the exported submit), and reuse works.
 * Against an old server (protocol < 7) export falls back to a CPU wait and
 * returns -1: the test reports that instead of failing.
 *
 * usage: vtest-sync-fd [iterations]
 */
#include <errno.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult _r = (x);                                                      \
      if (_r != VK_SUCCESS) {                                                 \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, _r);    \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

#define BUF_SIZE (256ull << 20)
#define FILLS 40

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkBuffer buf;
static VkDeviceMemory mem;
static PFN_vkGetFenceFdKHR get_fence_fd;
static PFN_vkGetSemaphoreFdKHR get_sem_fd;

static int failures;

static double
now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void
expect(int ok, const char *what)
{
   printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
   if (!ok)
      failures++;
}

static void
init(void)
{
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   CHECK(vkCreateInstance(&ici, NULL, &inst));

   uint32_t n = 1;
   VkResult r = vkEnumeratePhysicalDevices(inst, &n, &phys);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) {
      fprintf(stderr, "no physical device\n");
      exit(1);
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(phys, &props);
   printf("device: %s\n", props.deviceName);

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   const char *exts[] = {
      "VK_KHR_external_fence_fd",
      "VK_KHR_external_semaphore_fd",
   };
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 2,
      .ppEnabledExtensionNames = exts,
   };
   CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &queue);
   get_fence_fd = (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(dev, "vkGetFenceFdKHR");
   get_sem_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR");

   const VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = BUF_SIZE,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, buf, &req);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(phys, &mp);
   uint32_t type = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((req.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
         type = i;
         break;
      }
   }
   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));

   const VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   const VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
   const VkCommandBufferBeginInfo cbi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   CHECK(vkBeginCommandBuffer(cmd, &cbi));
   for (int i = 0; i < FILLS; i++) {
      vkCmdFillBuffer(cmd, buf, 0, VK_WHOLE_SIZE, i);
      const VkMemoryBarrier mb = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0,
                           NULL);
   }
   CHECK(vkEndCommandBuffer(cmd));
}

static void
submit(VkSemaphore signal, VkFence fence)
{
   const VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
      .signalSemaphoreCount = signal ? 1 : 0,
      .pSignalSemaphores = &signal,
   };
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
}

/* GPU time of one submit, measured with a plain fence */
static double
measure_work_ms(void)
{
   VkFence f;
   const VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   CHECK(vkCreateFence(dev, &fci, NULL, &f));
   const double t0 = now_ms();
   submit(VK_NULL_HANDLE, f);
   CHECK(vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX));
   const double t = now_ms() - t0;
   vkDestroyFence(dev, f, NULL);
   return t;
}

static int
sync_file_info(int fd, char *name, size_t size)
{
   struct sync_fence_info fi[4];
   struct sync_file_info info = { .num_fences = 4, .sync_fence_info = (uintptr_t)fi };
   memset(fi, 0, sizeof(fi));
   if (ioctl(fd, SYNC_IOC_FILE_INFO, &info))
      return -errno;
   snprintf(name, size, "%s/%s", fi[0].driver_name, fi[0].obj_name);
   return (int)info.num_fences;
}

struct result {
   double export_ms;
   double signal_ms; /* from submit to the fd signaling */
   int fd_valid;
};

static int
check_fd(const char *kind, int fd, double t_submit, double export_ms, double work_ms,
         struct result *res)
{
   char name[80] = "";
   res->export_ms = export_ms;
   res->fd_valid = fd >= 0;

   printf(" %s: export %.3f ms, fd %d\n", kind, export_ms, fd);
   if (fd < 0) {
      res->signal_ms = now_ms() - t_submit;
      return -1;
   }

   const int nf = sync_file_info(fd, name, sizeof(name));
   printf("  sync_file: %d fence(s), %s\n", nf, name);
   expect(nf > 0, "fd is a sync_file (SYNC_IOC_FILE_INFO)");

   struct pollfd p = { .fd = fd, .events = POLLIN };
   expect(poll(&p, 1, 0) == 0, "not signaled right after export (GPU busy)");

   expect(poll(&p, 1, 5000) == 1, "signals within 5 s");
   res->signal_ms = now_ms() - t_submit;
   printf("  signaled %.2f ms after submit (work alone %.2f ms)\n", res->signal_ms, work_ms);
   expect(res->signal_ms >= work_ms * 0.5, "did not signal before the work could finish");
   return 0;
}

int
main(int argc, char **argv)
{
   const int iters = argc > 1 ? atoi(argv[1]) : 5;
   init();

   measure_work_ms(); /* warm up */
   const double work_ms = measure_work_ms();
   printf("GPU work per submit: %.2f ms\n", work_ms);

   const VkExportFenceCreateInfo efci = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   const VkFenceCreateInfo fci = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .pNext = &efci,
   };
   const VkExportSemaphoreCreateInfo esci = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   const VkSemaphoreCreateInfo sci = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &esci,
   };
   VkFence fence;
   VkSemaphore sem;
   CHECK(vkCreateFence(dev, &fci, NULL, &fence));
   CHECK(vkCreateSemaphore(dev, &sci, NULL, &sem));

   double fence_export = 0, sem_export = 0, fence_sig = 0, sem_sig = 0;
   int real_fds = 0;

   /* the same fence and semaphore are reused every iteration */
   for (int it = 0; it < iters; it++) {
      struct result r;
      printf("iteration %d\n", it);

      /* fence */
      double t0 = now_ms();
      submit(VK_NULL_HANDLE, fence);
      const VkFenceGetFdInfoKHR fgi = {
         .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
         .fence = fence,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      int fd = -1;
      double t1 = now_ms();
      CHECK(get_fence_fd(dev, &fgi, &fd));
      const double fe = now_ms() - t1;
      if (!check_fd("fence", fd, t0, fe, work_ms, &r)) {
         real_fds++;
         /* the exported submit has completed: the fence must stay reset */
         usleep(20000);
         expect(vkGetFenceStatus(dev, fence) == VK_NOT_READY,
                "fence stays reset after export (no stale signal)");
         close(fd);
      }
      fence_export += r.export_ms;
      fence_sig += r.signal_ms;

      /* semaphore */
      t0 = now_ms();
      submit(sem, VK_NULL_HANDLE);
      const VkSemaphoreGetFdInfoKHR sgi = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
         .semaphore = sem,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      fd = -1;
      t1 = now_ms();
      CHECK(get_sem_fd(dev, &sgi, &fd));
      const double se = now_ms() - t1;
      if (!check_fd("semaphore", fd, t0, se, work_ms, &r)) {
         real_fds++;
         close(fd);
      }
      sem_export += r.export_ms;
      sem_sig += r.signal_ms;
   }

   CHECK(vkDeviceWaitIdle(dev));

   printf("\nsummary over %d iterations (GPU work %.2f ms per submit)\n", iters, work_ms);
   printf("  fence:     export %.3f ms avg, signaled %.2f ms after submit\n",
          fence_export / iters, fence_sig / iters);
   printf("  semaphore: export %.3f ms avg, signaled %.2f ms after submit\n",
          sem_export / iters, sem_sig / iters);
   printf("  real sync_file fds: %d of %d\n", real_fds, 2 * iters);
   if (!real_fds)
      printf("  (no fds: old server, export waited on the CPU and returned -1)\n");

   vkDestroySemaphore(dev, sem, NULL);
   vkDestroyFence(dev, fence, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyBuffer(dev, buf, NULL);
   vkFreeMemory(dev, mem, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);

   printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
