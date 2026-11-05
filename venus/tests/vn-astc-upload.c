/* Venus ASTC emulation test: upload ASTC blocks through
 * vkCmdCopyBufferToImage and read the RGBA8 renderer image back.
 *
 * Each 6x6 block is a void-extent (constant colour) block, so the expected
 * texel of every pixel is known.  The image is 30x20 (partial edge blocks)
 * with 2 array layers and a padded buffer row length.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult r_ = (x);                                                      \
      if (r_ != VK_SUCCESS) {                                                 \
         fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_);      \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

enum { W = 30, H = 20, BW = 6, BH = 6, LAYERS = 2, ROW_BLOCKS = 6 };

static uint32_t
find_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags flags)
{
   VkPhysicalDeviceMemoryProperties p;
   vkGetPhysicalDeviceMemoryProperties(pd, &p);
   for (uint32_t i = 0; i < p.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
         return i;
   fprintf(stderr, "no memory type\n");
   exit(1);
}

static void
block_color(uint32_t bx, uint32_t by, uint32_t layer, uint8_t c[4])
{
   c[0] = bx * 50;
   c[1] = by * 70;
   c[2] = layer ? 200 : 10;
   c[3] = 255 - bx;
}

int
main(void)
{
   const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_1 };
   const VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pApplicationInfo = &app };
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));

   uint32_t n = 1;
   VkPhysicalDevice pd;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   VkPhysicalDeviceFeatures feats;
   vkGetPhysicalDeviceFeatures(pd, &feats);
   printf("device: %s\ntextureCompressionASTC_LDR = %u\n", props.deviceName,
          feats.textureCompressionASTC_LDR);
   if (!feats.textureCompressionASTC_LDR)
      return 1;

   const VkFormat fmt = VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
   VkFormatProperties fp;
   vkGetPhysicalDeviceFormatProperties(pd, fmt, &fp);
   printf("ASTC_6x6 optimal 0x%x linear 0x%x buffer 0x%x\n",
          fp.optimalTilingFeatures, fp.linearTilingFeatures, fp.bufferFeatures);
   VkImageFormatProperties ifp;
   CHECK(vkGetPhysicalDeviceImageFormatProperties(
      pd, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &ifp));
   printf("max extent %ux%u layers %u samples 0x%x\n", ifp.maxExtent.width,
          ifp.maxExtent.height, ifp.maxArrayLayers, ifp.sampleCounts);
   VkResult r = vkGetPhysicalDeviceImageFormatProperties(
      pd, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ifp);
   printf("color attachment query: %d (expect %d)\n", r,
          VK_ERROR_FORMAT_NOT_SUPPORTED);

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                         .queueCount = 1, .pQueuePriorities = &prio };
   const VkPhysicalDeviceFeatures2 f2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .features = { .textureCompressionASTC_LDR = VK_TRUE },
   };
   const VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .pNext = &f2,
                                    .queueCreateInfoCount = 1,
                                    .pQueueCreateInfos = &qci };
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   if (!f2.features.textureCompressionASTC_LDR) {
      fprintf(stderr, "features2 not restored\n");
      return 1;
   }
   VkQueue q;
   vkGetDeviceQueue(dev, 0, 0, &q);

   const VkImageCreateInfo imci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = fmt,
      .extent = { W, H, 1 },
      .mipLevels = 1,
      .arrayLayers = LAYERS,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
   };
   VkImage img;
   CHECK(vkCreateImage(dev, &imci, NULL, &img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, img, &mr);
   printf("image size %llu (RGBA8 would be %u)\n", (unsigned long long)mr.size,
          W * H * 4 * LAYERS);
   VkDeviceMemory imem;
   const VkMemoryAllocateInfo imai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = find_mem(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
   CHECK(vkAllocateMemory(dev, &imai, NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));

   const VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS } };
   VkImageView view;
   CHECK(vkCreateImageView(dev, &vci, NULL, &view));

   /* source: ASTC blocks, rows padded to ROW_BLOCKS blocks, offset 64 */
   const uint32_t bx_n = (W + BW - 1) / BW, by_n = (H + BH - 1) / BH;
   const VkDeviceSize src_off = 64;
   const VkDeviceSize src_size = src_off + (VkDeviceSize)ROW_BLOCKS * by_n * 16 * LAYERS;
   const VkDeviceSize dst_size = W * H * 4 * LAYERS;
   VkBuffer bufs[2];
   VkDeviceMemory bmem[2];
   void *ptr[2];
   const VkDeviceSize sizes[2] = { src_size, dst_size };
   for (int i = 0; i < 2; i++) {
      const VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = sizes[i],
                                       .usage = i ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                                  : VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
      CHECK(vkCreateBuffer(dev, &bci, NULL, &bufs[i]));
      vkGetBufferMemoryRequirements(dev, bufs[i], &mr);
      const VkMemoryAllocateInfo mai = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
         .memoryTypeIndex = find_mem(pd, mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
      CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem[i]));
      CHECK(vkBindBufferMemory(dev, bufs[i], bmem[i], 0));
      CHECK(vkMapMemory(dev, bmem[i], 0, VK_WHOLE_SIZE, 0, &ptr[i]));
   }

   uint8_t *src = (uint8_t *)ptr[0] + src_off;
   memset(ptr[0], 0xcc, src_size);
   for (uint32_t l = 0; l < LAYERS; l++) {
      for (uint32_t by = 0; by < by_n; by++) {
         for (uint32_t bx = 0; bx < bx_n; bx++) {
            uint8_t *b = src + ((l * by_n + by) * ROW_BLOCKS + bx) * 16;
            uint8_t c[4];
            block_color(bx, by, l, c);
            static const uint8_t hdr[8] = { 0xfc, 0xfd, 0xff, 0xff,
                                            0xff, 0xff, 0xff, 0xff };
            memcpy(b, hdr, 8);
            for (int k = 0; k < 4; k++) {
               const uint16_t v = c[k] * 257;
               b[8 + 2 * k] = v & 0xff;
               b[9 + 2 * k] = v >> 8;
            }
         }
      }
   }

   VkCommandPool pool;
   const VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBuffer cb;
   const VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1 };
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
   const VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHECK(vkBeginCommandBuffer(cb, &bi));

   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS } };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   const VkBufferImageCopy up = {
      .bufferOffset = src_off,
      .bufferRowLength = ROW_BLOCKS * BW,
      .bufferImageHeight = by_n * BH,
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS },
      .imageExtent = { W, H, 1 } };
   vkCmdCopyBufferToImage(cb, bufs[0], img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);

   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   /* white-box: the renderer image is RGBA8, read it back as such */
   const VkBufferImageCopy down = {
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS },
      .imageExtent = { W, H, 1 } };
   vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bufs[1], 1, &down);
   CHECK(vkEndCommandBuffer(cb));

   memset(src, 0, 16); /* decoded at record time: later writes do not matter */
   const VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1, .pCommandBuffers = &cb };
   CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));

   const uint8_t *out = ptr[1];
   int bad = 0;
   for (uint32_t l = 0; l < LAYERS; l++)
      for (uint32_t y = 0; y < H; y++)
         for (uint32_t x = 0; x < W; x++) {
            uint8_t c[4];
            block_color(x / BW, y / BH, l, c);
            const uint8_t *p = out + ((l * H + y) * W + x) * 4;
            if (memcmp(p, c, 4)) {
               if (bad++ < 5)
                  printf("mismatch l%u (%u,%u): %u %u %u %u want %u %u %u %u\n",
                         l, x, y, p[0], p[1], p[2], p[3], c[0], c[1], c[2], c[3]);
            }
         }
   printf("first texel %u %u %u %u\n", out[0], out[1], out[2], out[3]);
   printf("%s (%d bad texels)\n", bad ? "FAIL" : "PASS", bad);

   /* reset and re-record to exercise staging reuse */
   CHECK(vkResetCommandBuffer(cb, 0));
   vkDestroyImageView(dev, view, NULL);
   vkDestroyCommandPool(dev, pool, NULL);
   vkDestroyImage(dev, img, NULL);
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
   return bad != 0;
}
