/*
 * Host test for Stage B: window buffers allocated on the host GPU through
 * vtest (VCMD_ALLOC_DMABUF_IMAGE), as the Android gralloc shim does.
 *
 *  1. Loads the gralloc shim (host build of libgbm_mesa_wrapper.so) with a
 *     fake inner wrapper and allocates through get_gbm_ops()->alloc, like
 *     minigbm_gbm_mesa: a scanout buffer must come from the server with an
 *     NVIDIA modifier, a CPU-usage buffer must fall back to the inner wrapper.
 *  2. Allocates more buffers after the first one (the server frees its
 *     Vulkan objects right after export, the dma-buf must keep the memory).
 *  3. Imports the first buffer into Venus (VK_EXT_image_drm_format_modifier
 *     explicit layout + dma-buf memory), clears it green, uploads a red
 *     square, and reads it back through Venus.
 *  4. Imports the same dma-buf into NVIDIA EGL as GL_TEXTURE_2D (what
 *     Hyprland does) and reads pixels through an FBO.
 *
 * Usage: vtest-hostalloc <shim.so> <fake-inner.so> [width height]
 * Env:   VK_ICD_FILENAMES, VN_DEBUG=vtest, VTEST_SOCKET_NAME,
 *        waydroid_gralloc_vtest_socket, waydroid_modifiers_<fourcc>_<n>
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "gbm_mesa_wrapper.h"

#define RED 0xff0000ffu   /* ABGR8888 little-endian: R=ff G=00 B=00 A=ff */
#define GREEN 0xff00ff00u
#define SQ_X 16
#define SQ_Y 24
#define SQ 64

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult r_ = (x);                                                      \
      if (r_ != VK_SUCCESS) {                                                 \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_);     \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

static uint32_t W = 1920, H = 1080;

#define EXTRA 8
static struct alloc_args extras[EXTRA];

static uint32_t
expected(uint32_t x, uint32_t y)
{
   return (x >= SQ_X && x < SQ_X + SQ && y >= SQ_Y && y < SQ_Y + SQ) ? RED
                                                                      : GREEN;
}

/* ---- 1/2: allocate through the shim ---- */

static int
alloc_through_shim(const char *shim_path, const char *inner_path,
                   struct alloc_args *out)
{
   setenv("waydroid_gralloc_vtest", "1", 1);
   setenv("waydroid_gralloc_vtest_inner", inner_path, 1);

   void *inner = dlopen(inner_path, RTLD_NOW | RTLD_GLOBAL);
   void *shim = dlopen(shim_path, RTLD_NOW);
   if (!inner || !shim) {
      fprintf(stderr, "dlopen: %s\n", dlerror());
      exit(1);
   }
   int *inner_calls = dlsym(inner, "fake_inner_alloc_calls");
   struct gbm_ops *(*get_ops)(void) = (struct gbm_ops * (*)(void))
      dlsym(shim, "get_gbm_ops");
   struct gbm_ops *ops = get_ops ? get_ops() : NULL;
   if (!ops || !inner_calls) {
      fprintf(stderr, "shim did not return ops\n");
      exit(1);
   }

   int fails = 0;

   struct alloc_args a = {
      .gbm = ops->dev_create(-1),
      .width = W,
      .height = H,
      .drm_format = DRM_FORMAT_ABGR8888,
      .use_scanout = true,
   };
   if (ops->alloc(&a) || a.out_fd < 0 || *inner_calls) {
      printf("shim scanout alloc: FAIL (fd %d, inner calls %d)\n", a.out_fd,
             *inner_calls);
      exit(1);
   }
   const bool nv = (a.out_modifier >> 56) == DRM_FORMAT_MOD_VENDOR_NVIDIA;
   printf("shim scanout alloc %ux%u AB24: fd %d stride %u modifier 0x%016llx "
          "dma-buf size %lld: %s\n",
          W, H, a.out_fd, a.out_stride, (unsigned long long)a.out_modifier,
          (long long)lseek(a.out_fd, 0, SEEK_END),
          nv ? "ok (NVIDIA block-linear)" : "FAIL (not an NVIDIA modifier)");
   fails += !nv;

   struct alloc_args sw = a;
   sw.force_linear = true;
   sw.needs_map_stride = true;
   sw.out_fd = -2;
   ops->alloc(&sw);
   printf("shim CPU-usage alloc: %s (inner wrapper calls %d)\n",
          *inner_calls == 1 ? "ok, fell back to inner" : "FAIL", *inner_calls);
   fails += *inner_calls != 1;

   /* more buffers after the first one, kept alive and later overwritten:
    * the server frees its Vulkan objects after export, so this catches
    * allocations aliasing each other
    */
   for (int i = 0; i < EXTRA; i++) {
      extras[i] = a;
      extras[i].force_linear = false;
      extras[i].needs_map_stride = false;
      if (ops->alloc(&extras[i]) || extras[i].out_fd < 0) {
         printf("extra alloc %d: FAIL\n", i);
         exit(1);
      }
   }
   printf("%d more allocations kept alive\n", EXTRA);

   *out = a;
   return fails;
}

/* ---- 3: Venus import ---- */

struct vk {
   VkInstance inst;
   VkPhysicalDevice pdev;
   VkDevice dev;
   VkQueue queue;
   VkCommandPool pool;
   VkPhysicalDeviceMemoryProperties mem_props;
};

static uint32_t
find_type(const struct vk *v, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < v->mem_props.memoryTypeCount; i++) {
      if ((bits & (1u << i)) &&
          (v->mem_props.memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   return UINT32_MAX;
}

static void
vk_init(struct vk *v)
{
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   CHECK(vkCreateInstance(&ici, NULL, &v->inst));
   uint32_t n = 8;
   VkPhysicalDevice pdevs[8];
   vkEnumeratePhysicalDevices(v->inst, &n, pdevs);
   for (uint32_t i = 0; i < n && !v->pdev; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pdevs[i], &p);
      if (strstr(p.deviceName, "Venus")) {
         v->pdev = pdevs[i];
         printf("Venus device: %s\n", p.deviceName);
      }
   }
   if (!v->pdev) {
      fprintf(stderr, "no Venus device\n");
      exit(1);
   }
   vkGetPhysicalDeviceMemoryProperties(v->pdev, &v->mem_props);

   const char *exts[] = {
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
   };
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 4,
      .ppEnabledExtensionNames = exts,
   };
   CHECK(vkCreateDevice(v->pdev, &dci, NULL, &v->dev));
   vkGetDeviceQueue(v->dev, 0, 0, &v->queue);
   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
   };
   CHECK(vkCreateCommandPool(v->dev, &pci, NULL, &v->pool));
}

static VkBuffer
host_buffer(struct vk *v, VkDeviceSize size, VkBufferUsageFlags usage,
            VkDeviceMemory *mem, uint32_t **map)
{
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
   };
   VkBuffer buf;
   CHECK(vkCreateBuffer(v->dev, &bci, NULL, &buf));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(v->dev, buf, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = find_type(v, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   CHECK(vkAllocateMemory(v->dev, &ai, NULL, mem));
   CHECK(vkBindBufferMemory(v->dev, buf, *mem, 0));
   CHECK(vkMapMemory(v->dev, *mem, 0, VK_WHOLE_SIZE, 0, (void **)map));
   return buf;
}

static void
barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from,
        VkImageLayout to, uint32_t src_qf, uint32_t dst_qf)
{
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = src_qf,
      .dstQueueFamilyIndex = dst_qf,
      .image = img,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                        NULL, 1, &b);
}

static void
import_image(struct vk *v, const struct alloc_args *a, VkImage *out_img,
             VkDeviceMemory *out_mem, bool verbose)
{
   VkSubresourceLayout plane = { .offset = 0, .rowPitch = a->out_stride };
   VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
      .sType =
         VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = a->out_modifier,
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
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { W, H, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT,
   };
   VkImage img;
   CHECK(vkCreateImage(v->dev, &ici, NULL, &img));

   PFN_vkGetMemoryFdPropertiesKHR get_fd_props =
      (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
         v->dev, "vkGetMemoryFdPropertiesKHR");
   VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   CHECK(get_fd_props(v->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                      a->out_fd, &fd_props));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(v->dev, img, &req);
   const uint32_t type = find_type(v, fd_props.memoryTypeBits & req.memoryTypeBits, 0);
   if (verbose || type == UINT32_MAX)
      printf("Venus import: memoryTypeBits fd 0x%x image 0x%x, size %llu, "
             "type %u (flags 0x%x)\n",
             fd_props.memoryTypeBits, req.memoryTypeBits,
             (unsigned long long)req.size, type,
             type == UINT32_MAX ? 0
                                : v->mem_props.memoryTypes[type].propertyFlags);
   if (type == UINT32_MAX)
      exit(1);

   VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img,
   };
   VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = dup(a->out_fd),
   };
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(v->dev, &mai, NULL, &mem));
   CHECK(vkBindImageMemory(v->dev, img, mem, 0));
   *out_img = img;
   *out_mem = mem;
}

static void
submit_wait(struct vk *v, VkCommandBuffer cmd)
{
   CHECK(vkEndCommandBuffer(cmd));
   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence;
   CHECK(vkCreateFence(v->dev, &fci, NULL, &fence));
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(v->queue, 1, &si, fence));
   CHECK(vkWaitForFences(v->dev, 1, &fence, VK_TRUE, 5000000000ull));
   vkDestroyFence(v->dev, fence, NULL);
   vkFreeCommandBuffers(v->dev, v->pool, 1, &cmd);
}

static VkCommandBuffer
begin_cmd(struct vk *v)
{
   VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = v->pool,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(v->dev, &cai, &cmd));
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   CHECK(vkBeginCommandBuffer(cmd, &bi));
   return cmd;
}

/* clear the extra buffers to blue after the first one was rendered */
static void
venus_clear_extras(struct vk *v)
{
   for (int i = 0; i < EXTRA; i++) {
      VkImage img;
      VkDeviceMemory mem;
      import_image(v, &extras[i], &img, &mem, false);
      VkCommandBuffer cmd = begin_cmd(v);
      barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
              VK_QUEUE_FAMILY_IGNORED);
      VkClearColorValue blue = { .float32 = { 0, 0, 1, 1 } };
      VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &blue, 1, &range);
      barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_GENERAL, 0, VK_QUEUE_FAMILY_FOREIGN_EXT);
      submit_wait(v, cmd);
      vkDestroyImage(v->dev, img, NULL);
      vkFreeMemory(v->dev, mem, NULL);
   }
   printf("cleared the %d extra buffers to blue through Venus\n", EXTRA);
}

static int
venus_render(struct vk *v, const struct alloc_args *a)
{
   VkImage img;
   VkDeviceMemory mem;
   import_image(v, a, &img, &mem, true);

   const VkDeviceSize size = (VkDeviceSize)W * H * 4;
   VkDeviceMemory up_mem, rb_mem;
   uint32_t *up, *rb;
   VkBuffer up_buf = host_buffer(v, SQ * SQ * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 &up_mem, &up);
   VkBuffer rb_buf = host_buffer(v, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 &rb_mem, &rb);
   for (int i = 0; i < SQ * SQ; i++)
      up[i] = RED;
   memset(rb, 0, size);

   VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = v->pool,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(v->dev, &cai, &cmd));
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   CHECK(vkBeginCommandBuffer(cmd, &bi));

   barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
           VK_QUEUE_FAMILY_IGNORED);
   VkClearColorValue green = { .float32 = { 0, 1, 0, 1 } };
   VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &green,
                        1, &range);
   barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
           VK_QUEUE_FAMILY_IGNORED);
   VkBufferImageCopy up_region = {
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageOffset = { SQ_X, SQ_Y, 0 },
      .imageExtent = { SQ, SQ, 1 },
   };
   vkCmdCopyBufferToImage(cmd, up_buf, img,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up_region);
   barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
           VK_QUEUE_FAMILY_IGNORED);
   VkBufferImageCopy rb_region = {
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { W, H, 1 },
   };
   vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          rb_buf, 1, &rb_region);
   /* hand the image to the compositor (foreign queue), like a present */
   barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_IMAGE_LAYOUT_GENERAL, 0, VK_QUEUE_FAMILY_FOREIGN_EXT);
   CHECK(vkEndCommandBuffer(cmd));

   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence;
   CHECK(vkCreateFence(v->dev, &fci, NULL, &fence));
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(v->queue, 1, &si, fence));
   CHECK(vkWaitForFences(v->dev, 1, &fence, VK_TRUE, 5000000000ull));

   uint64_t bad = 0;
   for (uint32_t y = 0; y < H; y++) {
      for (uint32_t x = 0; x < W; x++) {
         const uint32_t got = rb[y * W + x];
         if (got != expected(x, y) && bad++ < 4)
            printf("  Venus readback (%u,%u) = 0x%08x, expected 0x%08x\n", x,
                   y, got, expected(x, y));
      }
   }
   printf("Venus render + readback: %s (%llu/%u pixels wrong)\n",
          bad ? "FAIL" : "ok", (unsigned long long)bad, W * H);

   vkDestroyFence(v->dev, fence, NULL);
   vkDestroyBuffer(v->dev, up_buf, NULL);
   vkDestroyBuffer(v->dev, rb_buf, NULL);
   vkFreeMemory(v->dev, up_mem, NULL);
   vkFreeMemory(v->dev, rb_mem, NULL);
   vkDestroyImage(v->dev, img, NULL);
   vkFreeMemory(v->dev, mem, NULL);
   return !!bad;
}

/* ---- 4: NVIDIA EGL import as GL_TEXTURE_2D (Hyprland's path) ---- */

static int
egl_check(const struct alloc_args *a, const char *node)
{
   PFNEGLQUERYDEVICESEXTPROC query_devices =
      (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
   PFNEGLQUERYDEVICESTRINGEXTPROC query_string =
      (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   PFNEGLCREATEIMAGEKHRPROC create_image =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
         "glEGLImageTargetTexture2DOES");
   if (!query_devices || !query_string || !get_display || !create_image ||
       !image_target) {
      printf("EGL: missing extensions, skipped\n");
      return 1;
   }

   EGLDeviceEXT devs[8];
   EGLint n = 0;
   query_devices(8, devs, &n);
   EGLDisplay d = EGL_NO_DISPLAY;
   for (EGLint i = 0; i < n && d == EGL_NO_DISPLAY; i++) {
      const char *nd = query_string(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT);
      if (nd && !strcmp(nd, node))
         d = get_display(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
   }
   if (d == EGL_NO_DISPLAY || !eglInitialize(d, NULL, NULL)) {
      printf("EGL: no display for %s\n", node);
      return 1;
   }
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext c =
      eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
   eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);

   const EGLint attribs[] = {
      EGL_WIDTH, (EGLint)W,
      EGL_HEIGHT, (EGLint)H,
      EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
      EGL_DMA_BUF_PLANE0_FD_EXT, a->out_fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)a->out_stride,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(uint32_t)a->out_modifier,
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(uint32_t)(a->out_modifier >> 32),
      EGL_NONE,
   };
   EGLImageKHR image =
      create_image(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
   if (!image) {
      printf("EGL (%s): eglCreateImage FAIL (0x%x)\n",
             eglQueryString(d, EGL_VENDOR), eglGetError());
      return 1;
   }

   GLuint tex, fbo;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   while (glGetError())
      ;
   image_target(GL_TEXTURE_2D, image);
   const GLenum bind_err = glGetError();
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          tex, 0);
   const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

   const uint32_t points[][2] = {
      { 0, 0 }, { W - 1, H - 1 }, { SQ_X, SQ_Y },
      { SQ_X + SQ - 1, SQ_Y + SQ - 1 }, { SQ_X + SQ, SQ_Y }, { W / 2, H / 2 },
   };
   int bad = bind_err || status != GL_FRAMEBUFFER_COMPLETE;
   for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
      uint32_t px = 0;
      glReadPixels(points[i][0], points[i][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                   &px);
      if (px != expected(points[i][0], points[i][1])) {
         printf("  EGL pixel (%u,%u) = 0x%08x, expected 0x%08x\n",
                points[i][0], points[i][1], px,
                expected(points[i][0], points[i][1]));
         bad++;
      }
   }
   printf("EGL (%s) GL_TEXTURE_2D import: bind glError 0x%x, fbo 0x%x, "
          "pixels %s\n",
          eglQueryString(d, EGL_VENDOR), bind_err, status,
          bad ? "FAIL" : "ok");

   eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglTerminate(d);
   return !!bad;
}

int
main(int argc, char **argv)
{
   if (argc < 3) {
      fprintf(stderr, "usage: %s <shim.so> <fake-inner.so> [w h]\n", argv[0]);
      return 2;
   }
   if (argc >= 5) {
      W = atoi(argv[3]);
      H = atoi(argv[4]);
   }
   if (W < SQ_X + SQ || H < SQ_Y + SQ) {
      fprintf(stderr, "size must be at least %ux%u\n", SQ_X + SQ, SQ_Y + SQ);
      return 2;
   }

   struct alloc_args a;
   int fails = alloc_through_shim(argv[1], argv[2], &a);

   struct vk v = { 0 };
   vk_init(&v);
   fails += venus_render(&v, &a);
   venus_clear_extras(&v);
   vkDestroyCommandPool(v.dev, v.pool, NULL);
   vkDestroyDevice(v.dev, NULL);
   vkDestroyInstance(v.inst, NULL);

   fails += egl_check(&a, "/dev/dri/renderD129");

   close(a.out_fd);
   for (int i = 0; i < EXTRA; i++)
      close(extras[i].out_fd);
   printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
   return !!fails;
}
