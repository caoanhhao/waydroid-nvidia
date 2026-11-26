/*
 * Display-path test for Stage B, without Android: does Hyprland show a
 * window buffer that was allocated by the vtest server (NVIDIA block-linear),
 * rendered by Venus, and handed over the way Waydroid's hwcomposer does it
 * (zwp_linux_dmabuf_v1 bound at version 3, params.add(fd, 0, offset, stride,
 * modifier) + create_immed, AB24)?
 *
 * Draws a 4-quadrant pattern (red, green, blue, white) and keeps the window
 * up for SHOW_SECONDS (default 8) so it can be screenshotted.
 *
 * usage: wl-show-hostalloc [w h]
 * env:   VK_ICD_FILENAMES, VN_DEBUG=vtest, VTEST_SOCKET_NAME (Venus) and
 *        ALLOC_SOCKET (vtest server used for VCMD_ALLOC_DMABUF_IMAGE)
 */
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>

#include "linux-dmabuf-v1-client.h"
#include "vtest_alloc_client.h"
#include "vtest_protocol.h"
#include "xdg-shell-client.h"

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult r_ = (x);                                                      \
      if (r_ != VK_SUCCESS) {                                                 \
         fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_);     \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

static uint32_t W = 800, H = 600;

/* ---- Venus: import and draw quadrants ---- */

static void
venus_draw(const struct vtest_alloc_buffer *b)
{
   VkInstance inst;
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 8;
   VkPhysicalDevice pdevs[8], pdev = VK_NULL_HANDLE;
   vkEnumeratePhysicalDevices(inst, &n, pdevs);
   for (uint32_t i = 0; i < n && !pdev; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pdevs[i], &p);
      if (strstr(p.deviceName, "Venus"))
         pdev = pdevs[i];
   }
   if (!pdev) {
      fprintf(stderr, "no Venus device\n");
      exit(1);
   }
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
   VkDevice dev;
   CHECK(vkCreateDevice(pdev, &dci, NULL, &dev));
   VkQueue queue;
   vkGetDeviceQueue(dev, 0, 0, &queue);

   VkSubresourceLayout plane = { .offset = b->offset, .rowPitch = b->stride };
   VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
      .sType =
         VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = b->modifier,
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &plane,
   };
   VkExternalMemoryImageCreateInfo ext = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &mod,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   VkImageCreateInfo imgci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &ext,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM, /* AB24 */
      .extent = { W, H, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
   };
   VkImage img;
   CHECK(vkCreateImage(dev, &imgci, NULL, &img));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, img, &req);
   VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img,
   };
   VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = dup(b->fd),
   };
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import,
      .allocationSize = req.size,
      .memoryTypeIndex = __builtin_ctz(req.memoryTypeBits),
   };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindImageMemory(dev, img, mem, 0));

   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHECK(vkBeginCommandBuffer(cmd, &bi));

   VkImageMemoryBarrier bar = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                        NULL, 0, NULL, 1, &bar);

   /* render into the image (COLOR_ATTACHMENT, like HWUI), one rect per quadrant */
   VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   VkImageView view;
   CHECK(vkCreateImageView(dev, &vci, NULL, &view));
   VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue.color.float32 = { 1, 1, 1, 1 },
   };
   VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = { { 0, 0 }, { W, H } },
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att,
   };
   vkCmdBeginRendering(cmd, &ri);
   const float colors[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };
   for (int q = 0; q < 3; q++) {
      VkClearAttachment ca = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .clearValue.color.float32 = { colors[q][0], colors[q][1], colors[q][2], colors[q][3] },
      };
      VkClearRect rect = {
         .rect = { { (int32_t)((q % 2) * W / 2), (int32_t)((q / 2) * H / 2) },
                   { W / 2, H / 2 } },
         .layerCount = 1,
      };
      vkCmdClearAttachments(cmd, 1, &ca, 1, &rect);
   }
   vkCmdEndRendering(cmd);

   bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   bar.dstAccessMask = 0;
   bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
   bar.srcQueueFamilyIndex = 0;
   bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                        NULL, 1, &bar);
   CHECK(vkEndCommandBuffer(cmd));

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));
   printf("Venus drew the quadrants (top: red | green, bottom: blue | white)\n");
   /* objects intentionally kept until exit */
}

/* ---- Wayland: hwcomposer-style dma-buf v3 buffer in an xdg_toplevel ---- */

static struct zwp_linux_dmabuf_v1 *dmabuf;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static bool configured, failed;

static void
dmabuf_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f)
{
}

static void
dmabuf_modifier(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f, uint32_t hi,
                uint32_t lo)
{
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
   dmabuf_format,
   dmabuf_modifier,
};

static void
wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
   xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void
global(void *d, struct wl_registry *r, uint32_t name, const char *iface,
       uint32_t version)
{
   if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
      dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
      zwp_linux_dmabuf_v1_add_listener(dmabuf, &dmabuf_listener, NULL);
   } else if (!strcmp(iface, wl_compositor_interface.name)) {
      compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
   } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
      wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
      xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
   }
}

static void
global_remove(void *d, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

static void
surface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
   xdg_surface_ack_configure(s, serial);
   configured = true;
}

static const struct xdg_surface_listener surface_listener = { surface_configure };

static void
toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                   struct wl_array *states)
{
}

static void
toplevel_close(void *d, struct xdg_toplevel *t)
{
}

static const struct xdg_toplevel_listener toplevel_listener = {
   toplevel_configure,
   toplevel_close,
};

static void
params_created(void *d, struct zwp_linux_buffer_params_v1 *p, struct wl_buffer *b)
{
}

static void
params_failed(void *d, struct zwp_linux_buffer_params_v1 *p)
{
   /* hwcomposer abort()s here */
   fprintf(stderr, "zwp_linux_buffer_params.create_immed failed\n");
   failed = true;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
   params_created,
   params_failed,
};

int
main(int argc, char **argv)
{
   if (argc >= 3) {
      W = atoi(argv[1]);
      H = atoi(argv[2]);
   }
   const char *alloc_sock = getenv("ALLOC_SOCKET");
   int sock = alloc_sock ? vtest_alloc_connect(alloc_sock, "wl-show") : -1;
   if (sock < 0) {
      fprintf(stderr, "vtest_alloc_connect(%s) failed: %d\n", alloc_sock, sock);
      return 1;
   }

   struct wl_display *dpy = wl_display_connect(NULL);
   if (!dpy)
      return 1;
   struct wl_registry *reg = wl_display_get_registry(dpy);
   wl_registry_add_listener(reg, &registry_listener, NULL);
   wl_display_roundtrip(dpy);
   if (!dmabuf || !compositor || !wm_base) {
      fprintf(stderr, "missing globals\n");
      return 1;
   }

   /* modifiers Hyprland advertises for AB24 (what waydroid.modifiers holds) */
   uint64_t mods[12];
   for (int i = 0; i < 6; i++) {
      mods[i] = 0x0300000000606010ull + i;
      mods[6 + i] = 0x0300000000e08010ull + i;
   }
   struct vtest_alloc_buffer b;
   int ret = vtest_alloc_dmabuf_image(
      sock, W, H, DRM_FORMAT_ABGR8888,
      VCMD_ALLOC_DMABUF_IMAGE_FLAG_RENDER | VCMD_ALLOC_DMABUF_IMAGE_FLAG_TEXTURE |
         VCMD_ALLOC_DMABUF_IMAGE_FLAG_SCANOUT,
      mods, 12, &b);
   if (ret) {
      fprintf(stderr, "alloc failed: %d\n", ret);
      return 1;
   }
   printf("allocated %ux%u AB24 modifier 0x%016llx stride %u offset %u\n", W, H,
          (unsigned long long)b.modifier, b.stride, b.offset);

   venus_draw(&b);

   struct wl_surface *surface = wl_compositor_create_surface(compositor);
   struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
   xdg_surface_add_listener(xs, &surface_listener, NULL);
   struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
   xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
   xdg_toplevel_set_title(top, "stageB-dmabuf-test");
   xdg_toplevel_set_app_id(top, "stageB-dmabuf-test");
   wl_surface_commit(surface);
   while (!configured && wl_display_dispatch(dpy) >= 0)
      ;

   /* exactly hwcomposer's create_dmabuf_wl_buffer */
   struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
   zwp_linux_buffer_params_v1_add(params, b.fd, 0, b.offset, b.stride,
                                  b.modifier >> 32, b.modifier & 0xffffffff);
   zwp_linux_buffer_params_v1_add_listener(params, &params_listener, NULL);
   struct wl_buffer *buf =
      zwp_linux_buffer_params_v1_create_immed(params, W, H, DRM_FORMAT_ABGR8888, 0);
   zwp_linux_buffer_params_v1_destroy(params);

   wl_surface_attach(surface, buf, 0, 0);
   wl_surface_damage(surface, 0, 0, W, H);
   wl_surface_commit(surface);
   wl_display_roundtrip(dpy);
   printf("attached and committed; create_immed %s\n", failed ? "FAILED" : "ok");
   fflush(stdout);

   const int secs = getenv("SHOW_SECONDS") ? atoi(getenv("SHOW_SECONDS")) : 8;
   const time_t end = time(NULL) + secs;
   while (time(NULL) < end && !failed) {
      wl_display_flush(dpy);
      wl_display_dispatch_pending(dpy);
      usleep(50000);
   }
   return failed;
}
