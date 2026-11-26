/*
 * Host test for Venus dma-buf import over vtest (VCMD_RESOURCE_IMPORT_BLOB).
 *
 * Allocates a linear dma-buf (gbm on a DRM render node, or the system
 * dma-heap), writes a pattern with the CPU, imports it into Venus with
 * VK_EXT_external_memory_dma_buf, then:
 *   1. buffer test: copies the dma-buf into a host-visible buffer (GPU read)
 *      and fills the first half of it (GPU write), checks both from the CPU;
 *   2. image test (if VK_EXT_image_drm_format_modifier is exposed): binds a
 *      LINEAR B8G8R8A8 image to the dma-buf and clears it, checks a pixel.
 *
 * Usage: vtest-dmabuf-import [gbm:/dev/dri/renderD128 | heap:/dev/dma_heap/system]
 * Env:   VK_ICD_FILENAMES=<patched virtio_devenv_icd.json>
 *        VN_DEBUG=vtest VTEST_SOCKET_NAME=<socket>
 */
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <x86intrin.h>

#define W 256
#define H 256
#define PATTERN 0x11223344u
#define FILL 0xdeadbeefu
#define CLEAR 0xff00ff00u /* B8G8R8A8: b=0x00 g=0xff r=0x00 a=0xff */

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult r_ = (x);                                                      \
      if (r_ != VK_SUCCESS) {                                                 \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_);     \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

struct dmabuf {
   int fd;
   uint32_t stride;
   uint64_t size;
   struct gbm_device *gbm;
   struct gbm_bo *bo;
   void *map;
};

static void
alloc_dmabuf(const char *spec, struct dmabuf *d)
{
   memset(d, 0, sizeof(*d));
   if (!strncmp(spec, "heap:", 5)) {
      int heap = open(spec + 5, O_RDWR | O_CLOEXEC);
      if (heap < 0) {
         perror(spec + 5);
         exit(1);
      }
      struct dma_heap_allocation_data data = {
         .len = W * H * 4,
         .fd_flags = O_RDWR | O_CLOEXEC,
      };
      if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data)) {
         perror("DMA_HEAP_IOCTL_ALLOC");
         exit(1);
      }
      close(heap);
      d->fd = data.fd;
      d->stride = W * 4;
   } else {
      const char *node = !strncmp(spec, "gbm:", 4) ? spec + 4 : spec;
      int drm = open(node, O_RDWR | O_CLOEXEC);
      if (drm < 0) {
         perror(node);
         exit(1);
      }
      d->gbm = gbm_create_device(drm);
      if (!d->gbm) {
         fprintf(stderr, "gbm_create_device failed\n");
         exit(1);
      }
      d->bo = gbm_bo_create(d->gbm, W, H, GBM_FORMAT_ARGB8888,
                            GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
      if (!d->bo) {
         fprintf(stderr, "gbm_bo_create(LINEAR) failed\n");
         exit(1);
      }
      d->fd = gbm_bo_get_fd(d->bo);
      d->stride = gbm_bo_get_stride(d->bo);
      printf("gbm %s: backend %s, modifier 0x%llx\n", node,
             gbm_device_get_backend_name(d->gbm),
             (unsigned long long)gbm_bo_get_modifier(d->bo));
   }
   d->size = lseek(d->fd, 0, SEEK_END);
   printf("dma-buf fd %d: %ux%u stride %u size %llu\n", d->fd, W, H,
          d->stride, (unsigned long long)d->size);
}

static uint32_t *
map_dmabuf(struct dmabuf *d)
{
   void *p = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
   if (p == MAP_FAILED) {
      perror("mmap dma-buf");
      exit(1);
   }
   d->map = p;
   return p;
}

static bool use_clflush;

/* DMA_BUF_IOCTL_SYNC plus, with CLFLUSH=1, an explicit flush/invalidate of
 * the CPU cache lines.  The NVIDIA importer does not snoop the CPU cache, and
 * the exporters here (system heap, i915 on an LLC part) treat the sync ioctl
 * as a no-op on x86.
 */
static void
dmabuf_sync(struct dmabuf *d, uint64_t flags)
{
   struct dma_buf_sync s = { .flags = flags };
   ioctl(d->fd, DMA_BUF_IOCTL_SYNC, &s);

   if (use_clflush) {
      volatile char *p = d->map;
      _mm_mfence();
      for (uint64_t off = 0; off < d->size; off += 64)
         _mm_clflush((const void *)(p + off));
      _mm_mfence();
   }
}

static uint32_t
find_type(const VkPhysicalDeviceMemoryProperties *props, uint32_t bits,
          VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
      if ((bits & (1u << i)) &&
          (props->memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   return UINT32_MAX;
}

static bool
has_ext(VkPhysicalDevice pdev, const char *name)
{
   uint32_t n = 0;
   vkEnumerateDeviceExtensionProperties(pdev, NULL, &n, NULL);
   VkExtensionProperties *e = calloc(n, sizeof(*e));
   vkEnumerateDeviceExtensionProperties(pdev, NULL, &n, e);
   bool found = false;
   for (uint32_t i = 0; i < n; i++)
      found |= !strcmp(e[i].extensionName, name);
   free(e);
   return found;
}

struct ctx {
   VkPhysicalDevice pdev;
   VkDevice dev;
   VkQueue queue;
   VkCommandPool pool;
   VkPhysicalDeviceMemoryProperties mem_props;
   PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
};

static VkDeviceMemory
import_memory(struct ctx *c, struct dmabuf *d, VkMemoryRequirements req,
              VkImage image, VkBuffer buffer)
{
   VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   CHECK(c->GetMemoryFdPropertiesKHR(
      c->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, d->fd,
      &fd_props));
   printf("  vkGetMemoryFdPropertiesKHR: memoryTypeBits 0x%x, required 0x%x\n",
          fd_props.memoryTypeBits, req.memoryTypeBits);

   uint32_t type = find_type(&c->mem_props,
                             fd_props.memoryTypeBits & req.memoryTypeBits, 0);
   if (type == UINT32_MAX) {
      fprintf(stderr, "  no compatible memory type\n");
      exit(1);
   }

   VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image,
      .buffer = buffer,
   };
   VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = dup(d->fd), /* consumed on success */
   };
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(c->dev, &alloc, NULL, &mem));
   printf("  imported dma-buf as memory type %u (flags 0x%x)\n", type,
          c->mem_props.memoryTypes[type].propertyFlags);
   return mem;
}

static VkCommandBuffer
begin_cmd(struct ctx *c)
{
   VkCommandBufferAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = c->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(c->dev, &ai, &cmd));
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(vkBeginCommandBuffer(cmd, &bi));
   return cmd;
}

static void
submit_wait(struct ctx *c, VkCommandBuffer cmd)
{
   CHECK(vkEndCommandBuffer(cmd));
   VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence;
   CHECK(vkCreateFence(c->dev, &fi, NULL, &fence));
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(c->queue, 1, &si, fence));
   CHECK(vkWaitForFences(c->dev, 1, &fence, VK_TRUE, 5000000000ull));
   /* debug knob: SETTLE_MS=<ms> sleeps after the fence wait */
   if (getenv("SETTLE_MS"))
      usleep(atoi(getenv("SETTLE_MS")) * 1000);
   vkDestroyFence(c->dev, fence, NULL);
   vkFreeCommandBuffers(c->dev, c->pool, 1, &cmd);
}

static int
test_buffer(struct ctx *c, struct dmabuf *d, uint32_t *cpu)
{
   const VkDeviceSize size = (VkDeviceSize)d->stride * H;
   printf("buffer test (%llu bytes)\n", (unsigned long long)size);

   dmabuf_sync(d, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
   for (uint64_t i = 0; i < size / 4; i++)
      cpu[i] = PATTERN;
   dmabuf_sync(d, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

   VkExternalMemoryBufferCreateInfo ext = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .pNext = &ext,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer buf;
   CHECK(vkCreateBuffer(c->dev, &bci, NULL, &buf));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(c->dev, buf, &req);
   VkDeviceMemory mem = import_memory(c, d, req, VK_NULL_HANDLE, buf);
   CHECK(vkBindBufferMemory(c->dev, buf, mem, 0));

   /* host-visible readback buffer */
   bci.pNext = NULL;
   bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer rb;
   CHECK(vkCreateBuffer(c->dev, &bci, NULL, &rb));
   vkGetBufferMemoryRequirements(c->dev, rb, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex =
         find_type(&c->mem_props, req.memoryTypeBits,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   VkDeviceMemory rb_mem;
   CHECK(vkAllocateMemory(c->dev, &ai, NULL, &rb_mem));
   CHECK(vkBindBufferMemory(c->dev, rb, rb_mem, 0));

   VkCommandBuffer cmd = begin_cmd(c);
   vkCmdCopyBuffer(cmd, buf, rb, 1,
                   &(VkBufferCopy){ .srcOffset = 0, .dstOffset = 0, .size = size });
   VkMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                        NULL, 0, NULL);
   vkCmdFillBuffer(cmd, buf, 0, size / 2, FILL);
   submit_wait(c, cmd);

   int fails = 0;
   uint32_t *rbp;
   CHECK(vkMapMemory(c->dev, rb_mem, 0, VK_WHOLE_SIZE, 0, (void **)&rbp));
   uint64_t bad = 0;
   for (uint64_t i = 0; i < size / 4; i++)
      bad += rbp[i] != PATTERN;
   printf("  GPU read of CPU pattern: %s (%llu/%llu words wrong, [0]=0x%08x)\n",
          bad ? "FAIL" : "ok", (unsigned long long)bad,
          (unsigned long long)(size / 4), rbp[0]);
   fails += !!bad;
   vkUnmapMemory(c->dev, rb_mem);

   dmabuf_sync(d, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
   bad = 0;
   for (uint64_t i = 0; i < size / 4; i++)
      bad += cpu[i] != (i < size / 8 ? FILL : PATTERN);
   printf("  CPU read of GPU fill:    %s (%llu words wrong, [0]=0x%08x "
          "[last]=0x%08x)\n",
          bad ? "FAIL" : "ok", (unsigned long long)bad, cpu[0],
          cpu[size / 4 - 1]);
   dmabuf_sync(d, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
   fails += !!bad;

   vkDestroyBuffer(c->dev, rb, NULL);
   vkFreeMemory(c->dev, rb_mem, NULL);
   vkDestroyBuffer(c->dev, buf, NULL);
   vkFreeMemory(c->dev, mem, NULL);
   return fails;
}

static int
test_image(struct ctx *c, struct dmabuf *d, uint32_t *cpu)
{
   printf("image test (LINEAR B8G8R8A8_UNORM %ux%u, rowPitch %u)\n", W, H,
          d->stride);

   VkSubresourceLayout plane = { .offset = 0, .rowPitch = d->stride };
   VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
      .sType =
         VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = 0, /* DRM_FORMAT_MOD_LINEAR */
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &plane,
   };
   VkExternalMemoryImageCreateInfo ext = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &mod,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &ext,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { W, H, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage img;
   VkResult r = vkCreateImage(c->dev, &ici, NULL, &img);
   if (r != VK_SUCCESS) {
      printf("  vkCreateImage(LINEAR modifier) = %d: FAIL\n", r);
      return 1;
   }
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(c->dev, img, &req);
   VkDeviceMemory mem = import_memory(c, d, req, img, VK_NULL_HANDLE);
   CHECK(vkBindImageMemory(c->dev, img, mem, 0));

   VkCommandBuffer cmd = begin_cmd(c);
   VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkImageMemoryBarrier to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img,
      .subresourceRange = range,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                        1, &to_dst);
   VkClearColorValue color = { .float32 = { 0.0f, 1.0f, 0.0f, 1.0f } };
   vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        &color, 1, &range);
   VkImageMemoryBarrier to_general = to_dst;
   to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   to_general.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
   to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
   /* release to the foreign (CPU / other device) owner */
   to_general.srcQueueFamilyIndex = 0;
   to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                        NULL, 1, &to_general);
   submit_wait(c, cmd);

   dmabuf_sync(d, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
   uint64_t bad = 0;
   for (uint32_t y = 0; y < H; y++) {
      for (uint32_t x = 0; x < W; x++) {
         const uint32_t v = cpu[y * (d->stride / 4) + x];
         if (v != CLEAR && bad++ < 4)
            printf("  wrong pixel (%u,%u) = 0x%08x\n", x, y, v);
      }
   }
   printf("  CPU read of GPU clear:   %s (%llu/%u pixels wrong, (0,0)=0x%08x "
          "(%u,%u)=0x%08x)\n",
          bad ? "FAIL" : "ok", (unsigned long long)bad, W * H, cpu[0], W - 1,
          H - 1, cpu[(H - 1) * (d->stride / 4) + W - 1]);
   dmabuf_sync(d, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);

   vkDestroyImage(c->dev, img, NULL);
   vkFreeMemory(c->dev, mem, NULL);
   return !!bad;
}

int
main(int argc, char **argv)
{
   const char *spec = argc > 1 ? argv[1] : "gbm:/dev/dri/renderD128";
   use_clflush = getenv("CLFLUSH") && atoi(getenv("CLFLUSH"));
   printf("CPU cache flush around CPU access: %s\n", use_clflush ? "on" : "off");
   struct dmabuf d;
   alloc_dmabuf(spec, &d);
   uint32_t *cpu = map_dmabuf(&d);

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vtest-dmabuf-import",
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));

   uint32_t n = 0;
   CHECK(vkEnumeratePhysicalDevices(inst, &n, NULL));
   VkPhysicalDevice pdevs[8];
   n = n > 8 ? 8 : n;
   CHECK(vkEnumeratePhysicalDevices(inst, &n, pdevs));

   struct ctx c = { 0 };
   for (uint32_t i = 0; i < n && !c.pdev; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pdevs[i], &p);
      printf("device %u: %s\n", i, p.deviceName);
      if (strstr(p.deviceName, "Venus"))
         c.pdev = pdevs[i];
   }
   if (!c.pdev) {
      fprintf(stderr, "no Venus device (check VK_ICD_FILENAMES / "
                      "VTEST_SOCKET_NAME / server)\n");
      return 1;
   }
   if (!has_ext(c.pdev, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
      fprintf(stderr, "VK_EXT_external_memory_dma_buf not exposed\n");
      return 1;
   }
   const bool has_mod =
      has_ext(c.pdev, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) &&
      has_ext(c.pdev, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

   VkPhysicalDeviceExternalBufferInfo ebi = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   VkExternalBufferProperties ebp = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
   };
   vkGetPhysicalDeviceExternalBufferProperties(c.pdev, &ebi, &ebp);
   const VkExternalMemoryFeatureFlags feats =
      ebp.externalMemoryProperties.externalMemoryFeatures;
   printf("dma-buf buffer features: 0x%x (importable: %s)\n", feats,
          (feats & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "yes" : "NO");
   if (!(feats & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
      /* old server or client: still check the import fails cleanly */
      VkDevice dev;
      float prio = 1.0f;
      const char *e[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                          VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME };
      VkDeviceCreateInfo dci = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount = 1,
            .pQueuePriorities = &prio,
         },
         .enabledExtensionCount = 2,
         .ppEnabledExtensionNames = e,
      };
      CHECK(vkCreateDevice(c.pdev, &dci, NULL, &dev));
      PFN_vkGetMemoryFdPropertiesKHR get_props =
         (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            dev, "vkGetMemoryFdPropertiesKHR");
      VkMemoryFdPropertiesKHR props = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      };
      VkResult r = get_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                             d.fd, &props);
      printf("vkGetMemoryFdPropertiesKHR without import support = %d\n", r);
      vkDestroyDevice(dev, NULL);
      vkDestroyInstance(inst, NULL);
      printf("RESULT: SKIP (dma-buf import not supported)\n");
      return 2;
   }

   vkGetPhysicalDeviceMemoryProperties(c.pdev, &c.mem_props);

   const char *exts[] = {
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
   };
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = has_mod ? 4 : 2,
      .ppEnabledExtensionNames = exts,
   };
   CHECK(vkCreateDevice(c.pdev, &dci, NULL, &c.dev));
   vkGetDeviceQueue(c.dev, 0, 0, &c.queue);
   c.GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)
      vkGetDeviceProcAddr(c.dev, "vkGetMemoryFdPropertiesKHR");
   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   CHECK(vkCreateCommandPool(c.dev, &pci, NULL, &c.pool));

   int fails = test_buffer(&c, &d, cpu);
   if (has_mod)
      fails += test_image(&c, &d, cpu);
   else
      printf("image test skipped: VK_EXT_image_drm_format_modifier not "
             "exposed\n");

   vkDestroyCommandPool(c.dev, c.pool, NULL);
   vkDestroyDevice(c.dev, NULL);
   vkDestroyInstance(inst, NULL);

   printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
   return !!fails;
}
