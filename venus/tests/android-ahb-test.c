/*
 * Android-side reproducer for SurfaceFlinger's "Failed to create a valid
 * texture ... isWriteable:1" with ANGLE on Venus: allocate an AHardwareBuffer
 * like RenderEngine's writeable buffers (optionally with CPU read usage, which
 * makes the gralloc shim take the Intel LINEAR path) and try
 *   1. ANGLE: eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) + render target
 *   2. Venus directly: AHB properties, image format query, image + memory import
 *
 * usage: android-ahb-test [w h [cpu]]   (cpu=1 adds CPU_READ_OFTEN)
 * build: NDK clang, -lEGL -lGLESv3 -lvulkan -landroid -llog
 */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static uint32_t W = 144, H = 63;

static void EGLAPIENTRY
egl_debug(EGLenum error, const char *command, EGLint type, EGLLabelKHR thread,
          EGLLabelKHR object, const char *message)
{
   printf("EGL debug: %s error 0x%x: %s\n", command, error, message ? message : "");
}

static int
egl_test(AHardwareBuffer *ahb)
{
   PFNEGLDEBUGMESSAGECONTROLKHRPROC debug_control =
      (PFNEGLDEBUGMESSAGECONTROLKHRPROC)eglGetProcAddress("eglDebugMessageControlKHR");
   if (debug_control) {
      const EGLAttrib attribs[] = { EGL_DEBUG_MSG_CRITICAL_KHR, EGL_TRUE,
                                    EGL_DEBUG_MSG_ERROR_KHR, EGL_TRUE,
                                    EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE,
                                    EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE };
      debug_control(egl_debug, attribs);
   }
   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (!eglInitialize(d, NULL, NULL)) {
      printf("EGL: eglInitialize failed 0x%x\n", eglGetError());
      return 1;
   }
   printf("EGL: %s / %s\n", eglQueryString(d, EGL_VENDOR),
          eglQueryString(d, EGL_VERSION));
   eglBindAPI(EGL_OPENGL_ES_API);
   const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
   if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
      printf("EGL: context failed 0x%x\n", eglGetError());
      return 1;
   }
   printf("GL: %s\n", glGetString(GL_RENDERER));

   PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_client_buffer =
      (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress(
         "eglGetNativeClientBufferANDROID");
   PFNEGLCREATEIMAGEKHRPROC create_image =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
         "glEGLImageTargetTexture2DOES");

   const EGLint img_attribs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
   EGLImageKHR img = create_image(d, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                  get_client_buffer(ahb), img_attribs);
   if (!img) {
      printf("EGL: eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) FAILED 0x%x\n",
             eglGetError());
      return 1;
   }
   GLuint tex, fbo;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   image_target(GL_TEXTURE_2D, img);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   glClearColor(0, 1, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   uint32_t px = 0;
   glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px);
   printf("EGL: image ok, glError 0x%x, fbo 0x%x, clear readback 0x%08x %s\n",
          glGetError(), status, px, px == 0xff00ff00 ? "ok" : "WRONG");
   return status != GL_FRAMEBUFFER_COMPLETE || px != 0xff00ff00;
}

#define VKCHECK(what, x)                                                       \
   do {                                                                        \
      VkResult r_ = (x);                                                       \
      printf("VK: %-52s %d%s\n", what, r_, r_ ? "  <-- FAIL" : "");           \
      if (r_)                                                                  \
         return 1;                                                             \
   } while (0)

static int
vk_test(AHardwareBuffer *ahb, uint64_t ahb_usage)
{
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance inst;
   VKCHECK("vkCreateInstance", vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   VkPhysicalDevice pdev;
   vkEnumeratePhysicalDevices(inst, &n, &pdev);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("VK: device %s\n", props.deviceName);

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
   VKCHECK("vkCreateDevice", vkCreateDevice(pdev, &dci, NULL, &dev));

   PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_props =
      (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
         dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
   VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
   };
   VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
      .pNext = &fmt,
   };
   VKCHECK("vkGetAndroidHardwareBufferPropertiesANDROID",
           get_ahb_props(dev, ahb, &ahb_props));
   printf("VK:   allocationSize %llu memoryTypeBits 0x%x format %d "
          "formatFeatures 0x%x (COLOR_ATTACHMENT %s)\n",
          (unsigned long long)ahb_props.allocationSize,
          ahb_props.memoryTypeBits, fmt.format, fmt.formatFeatures,
          (fmt.formatFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? "yes" : "NO");

   VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if (ahb_usage & AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER)
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   /* ANGLE picks LINEAR tiling for AHBs with CPU usage (TILING=linear) */
   const VkImageTiling tiling =
      getenv("TILING") && !strcmp(getenv("TILING"), "linear")
         ? VK_IMAGE_TILING_LINEAR
         : VK_IMAGE_TILING_OPTIMAL;
   printf("VK: tiling %s\n", tiling == VK_IMAGE_TILING_LINEAR ? "LINEAR" : "OPTIMAL");

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
      .usage = usage,
   };
   VkAndroidHardwareBufferUsageANDROID ahb_usage_out = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID,
   };
   VkExternalImageFormatProperties ext_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
      .pNext = &ahb_usage_out,
   };
   VkImageFormatProperties2 ifp = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      .pNext = &ext_props,
   };
   VKCHECK("vkGetPhysicalDeviceImageFormatProperties2(AHB)",
           vkGetPhysicalDeviceImageFormatProperties2(pdev, &fi, &ifp));

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
      .usage = usage,
   };
   VkImage img;
   VKCHECK("vkCreateImage(AHB external)", vkCreateImage(dev, &ci, NULL, &img));

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
      .memoryTypeIndex = __builtin_ctz(ahb_props.memoryTypeBits),
   };
   VkDeviceMemory mem;
   VKCHECK("vkAllocateMemory(import AHB)", vkAllocateMemory(dev, &mai, NULL, &mem));
   VKCHECK("vkBindImageMemory", vkBindImageMemory(dev, img, mem, 0));
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc >= 3) {
      W = atoi(argv[1]);
      H = atoi(argv[2]);
   }
   const int cpu = argc >= 4 ? atoi(argv[3]) : 1;
   uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
   if (cpu)
      usage |= AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
   /* USAGE=0x... overrides, e.g. a software-drawn cursor: 0x133 */
   if (getenv("USAGE"))
      usage = strtoull(getenv("USAGE"), NULL, 0);

   AHardwareBuffer_Desc desc = {
      .width = W,
      .height = H,
      .layers = 1,
      .format = getenv("FORMAT") ? (uint32_t)strtoul(getenv("FORMAT"), NULL, 0)
                                 : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      .usage = usage,
   };
   AHardwareBuffer *ahb;
   if (AHardwareBuffer_allocate(&desc, &ahb)) {
      printf("AHardwareBuffer_allocate failed\n");
      return 1;
   }
   AHardwareBuffer_describe(ahb, &desc);
   printf("AHB %ux%u stride %u usage 0x%llx (CPU read %s)\n", desc.width,
          desc.height, desc.stride, (unsigned long long)usage, cpu ? "yes" : "no");

   const char *which = getenv("TEST");
   int fails = 0;
   if (!which || !strcmp(which, "vk"))
      fails += vk_test(ahb, usage);
   if (!which || !strcmp(which, "egl"))
      fails += egl_test(ahb);
   printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
   return !!fails;
}
