/*
 * Test a Venus driver build inside Android without installing it: dlopen the
 * vulkan HAL (vulkan.virtio.so / libvulkan_virtio.so) from a path, bypass the
 * system Vulkan loader, and run the AHB import the way Android 13's ANGLE
 * does it (HardwareBufferImageSiblingVkAndroid): OPTIMAL tiling, usage
 * TRANSFER|SAMPLED|INPUT_ATTACHMENT|COLOR_ATTACHMENT, and a memory type with
 * DEVICE_LOCAL taken from the AHB's memoryTypeBits.
 *
 * usage: android-ahb-linear-probe <driver.so> [w h [cpu [tiling]]]
 *        tiling: optimal (default) | linear
 */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <hardware/hwvulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(what, x)                                                         \
   do {                                                                        \
      VkResult r_ = (x);                                                       \
      printf("%-48s %d%s\n", what, r_, r_ ? "  <-- FAIL" : "");               \
      if (r_)                                                                  \
         return 1;                                                             \
   } while (0)

int
main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: %s <driver.so> [w h [cpu [tiling]]]\n", argv[0]);
      return 2;
   }
   const uint32_t W = argc > 3 ? atoi(argv[2]) : 144;
   const uint32_t H = argc > 3 ? atoi(argv[3]) : 63;
   const int cpu = argc > 4 ? atoi(argv[4]) : 1;
   const VkImageTiling tiling = argc > 5 && !strcmp(argv[5], "linear")
                                   ? VK_IMAGE_TILING_LINEAR
                                   : VK_IMAGE_TILING_OPTIMAL;

   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   const struct hw_module_t *hmi = lib ? dlsym(lib, HAL_MODULE_INFO_SYM_AS_STR) : NULL;
   if (!hmi) {
      printf("dlopen/HMI failed: %s\n", dlerror());
      return 1;
   }
   hwvulkan_device_t *hal;
   if (hmi->methods->open(hmi, HWVULKAN_DEVICE_0, (struct hw_device_t **)&hal)) {
      printf("HAL open failed\n");
      return 1;
   }
   PFN_vkGetInstanceProcAddr gipa = hal->GetInstanceProcAddr;

   uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
   if (cpu)
      usage |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
   AHardwareBuffer_Desc desc = {
      .width = W, .height = H, .layers = 1,
      .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      .usage = usage,
   };
   AHardwareBuffer *ahb;
   if (AHardwareBuffer_allocate(&desc, &ahb)) {
      printf("AHardwareBuffer_allocate failed\n");
      return 1;
   }
   AHardwareBuffer_describe(ahb, &desc);
   printf("driver %s\nAHB %ux%u stride %u CPU read %s, tiling %s\n", argv[1], W,
          H, desc.stride, cpu ? "yes" : "no",
          tiling == VK_IMAGE_TILING_LINEAR ? "LINEAR" : "OPTIMAL");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance inst;
   CHECK("vkCreateInstance", hal->CreateInstance(&ici, NULL, &inst));
#define IPROC(name) PFN_vk##name name = (PFN_vk##name)gipa(inst, "vk" #name)
   IPROC(EnumeratePhysicalDevices);
   IPROC(GetPhysicalDeviceProperties);
   IPROC(GetPhysicalDeviceImageFormatProperties2);
   IPROC(CreateDevice);
   IPROC(GetDeviceProcAddr);
   IPROC(GetPhysicalDeviceMemoryProperties);

   uint32_t n = 1;
   VkPhysicalDevice pdev;
   EnumeratePhysicalDevices(inst, &n, &pdev);
   VkPhysicalDeviceProperties props;
   GetPhysicalDeviceProperties(pdev, &props);
   printf("device %s\n", props.deviceName);

   const char *exts[] = {
      "VK_ANDROID_external_memory_android_hardware_buffer",
      "VK_KHR_external_memory",
      "VK_KHR_sampler_ycbcr_conversion",
      "VK_EXT_queue_family_foreign",
      "VK_KHR_dedicated_allocation",
   };
   float prio = 1;
   VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 5,
      .ppEnabledExtensionNames = exts,
   };
   VkDevice dev;
   CHECK("vkCreateDevice", CreateDevice(pdev, &dci, NULL, &dev));
#define DPROC(name) PFN_vk##name name = (PFN_vk##name)GetDeviceProcAddr(dev, "vk" #name)
   DPROC(GetAndroidHardwareBufferPropertiesANDROID);
   DPROC(CreateImage);
   DPROC(AllocateMemory);
   DPROC(BindImageMemory);

   VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
   };
   VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
      .pNext = &fmt,
   };
   CHECK("vkGetAndroidHardwareBufferPropertiesANDROID",
         GetAndroidHardwareBufferPropertiesANDROID(dev, ahb, &ahb_props));

   /* AhbDescUsageToVkImageUsage for GPU_SAMPLED_IMAGE | GPU_FRAMEBUFFER */
   const VkImageUsageFlags img_usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   VkPhysicalDeviceExternalImageFormatInfo ext_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
   };
   VkPhysicalDeviceImageFormatInfo2 fi = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &ext_info,
      .format = fmt.format,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = tiling,
      .usage = img_usage,
   };
   VkExternalImageFormatProperties ext_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
   };
   VkImageFormatProperties2 ifp = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      .pNext = &ext_props,
   };
   CHECK("vkGetPhysicalDeviceImageFormatProperties2(AHB)",
         GetPhysicalDeviceImageFormatProperties2(pdev, &fi, &ifp));

   VkExternalMemoryImageCreateInfo emi = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
   };
   VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &emi,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = fmt.format,
      .extent = { W, H, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = tiling,
      .usage = img_usage,
   };
   VkImage img;
   CHECK("vkCreateImage(AHB external)", CreateImage(dev, &ci, NULL, &img));
   VkMemoryDedicatedAllocateInfo ded = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img,
   };
   VkImportAndroidHardwareBufferInfoANDROID imp = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
      .pNext = &ded,
      .buffer = ahb,
   };
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &imp,
      .allocationSize = ahb_props.allocationSize,
      .memoryTypeIndex = UINT32_MAX,
   };
   VkPhysicalDeviceMemoryProperties mp;
   GetPhysicalDeviceMemoryProperties(pdev, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((ahb_props.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
         mai.memoryTypeIndex = i;
         break;
      }
   }
   printf("AHB memoryTypeBits 0x%x -> DEVICE_LOCAL type %d (as ANGLE)\n",
          ahb_props.memoryTypeBits, (int)mai.memoryTypeIndex);
   if (mai.memoryTypeIndex == UINT32_MAX) {
      printf("no DEVICE_LOCAL memory type  <-- FAIL (ANGLE eglCreateImageKHR fails)\n");
      return 1;
   }
   VkDeviceMemory mem;
   CHECK("vkAllocateMemory(import AHB)", AllocateMemory(dev, &mai, NULL, &mem));
   CHECK("vkBindImageMemory", BindImageMemory(dev, img, mem, 0));
   printf("RESULT: PASS\n");
   return 0;
}
