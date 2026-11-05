/*
 * Reproduce SurfaceFlinger's import of a software-decoder video frame:
 *   - AHardwareBuffer YV12 (default 720x1280, usage 0x933, as the TikTok
 *     splash video layer reported by the hwcomposer format shim);
 *   - fill Y/V/U through AHardwareBuffer_lock (Y=81, V=240, U=90: a red frame
 *     in BT.601 limited range);
 *   - eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) on ANGLE, bind it as
 *     GL_TEXTURE_EXTERNAL_OES, draw a full-screen quad, glReadPixels.
 *
 * usage: android-yv12-import-test [w h]   (FORMAT=, USAGE= override)
 * Prints PASS with the sampled pixel, or where it failed.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...)                                                       \
   do {                                                                        \
      if (!(cond)) {                                                           \
         printf("FAIL: " __VA_ARGS__);                                         \
         printf(" (egl 0x%x gl 0x%x)\n", eglGetError(), glGetError());        \
         return 1;                                                             \
      }                                                                        \
   } while (0)

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[512];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      printf("shader: %s\n", log);
   }
   return s;
}

static void EGLAPIENTRY
egl_debug(EGLenum error, const char *command, EGLint type, EGLLabelKHR tl,
          EGLLabelKHR ol, const char *message)
{
   (void)type, (void)tl, (void)ol;
   printf("EGL debug: %s error 0x%x: %s\n", command, error, message ? message : "");
}

/* What ANGLE sees from vkGetAndroidHardwareBufferPropertiesANDROID. */
static void
probe_vulkan(AHardwareBuffer *ahb)
{
   const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_1 };
   const VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pApplicationInfo = &app };
   VkInstance inst;
   if (vkCreateInstance(&ici, NULL, &inst)) {
      printf("vk: no instance\n");
      return;
   }
   uint32_t n = 1;
   VkPhysicalDevice pd;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   const float prio = 1;
   const VkDeviceQueueCreateInfo q = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueCount = 1, .pQueuePriorities = &prio };
   const char *exts[] = { "VK_ANDROID_external_memory_android_hardware_buffer",
                          "VK_EXT_queue_family_foreign", "VK_KHR_external_memory",
                          "VK_KHR_sampler_ycbcr_conversion" };
   VkPhysicalDeviceSamplerYcbcrConversionFeatures enable_ycbcr = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
      .samplerYcbcrConversion = VK_TRUE
   };
   const VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .pNext = &enable_ycbcr,
                                    .queueCreateInfoCount = 1, .pQueueCreateInfos = &q,
                                    .enabledExtensionCount = 4,
                                    .ppEnabledExtensionNames = exts };
   VkDevice dev;
   VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
   if (r) {
      printf("vk: vkCreateDevice %d\n", r);
      return;
   }
   PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_props =
      (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
         dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
   VkAndroidHardwareBufferFormatPropertiesANDROID fp = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID
   };
   VkAndroidHardwareBufferPropertiesANDROID props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fp
   };
   r = get_props(dev, ahb, &props);
   printf("vk: AHB properties %d: size %llu memoryTypeBits 0x%x format %d externalFormat "
          "%llu formatFeatures 0x%x model %d range %d\n",
          r, (unsigned long long)props.allocationSize, props.memoryTypeBits, fp.format,
          (unsigned long long)fp.externalFormat, fp.formatFeatures,
          fp.suggestedYcbcrModel, fp.suggestedYcbcrRange);

   VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_feat = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES
   };
   VkPhysicalDeviceFeatures2 feat2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &ycbcr_feat };
   vkGetPhysicalDeviceFeatures2(pd, &feat2);
   uint32_t next = 0;
   vkEnumerateDeviceExtensionProperties(pd, NULL, &next, NULL);
   VkExtensionProperties ext_props[512];
   next = next > 512 ? 512 : next;
   vkEnumerateDeviceExtensionProperties(pd, NULL, &next, ext_props);
   int has_ycbcr_ext = 0;
   for (uint32_t i = 0; i < next; i++)
      has_ycbcr_ext |= !strcmp(ext_props[i].extensionName, "VK_KHR_sampler_ycbcr_conversion");
   printf("vk: samplerYcbcrConversion feature %u, VK_KHR_sampler_ycbcr_conversion %s\n",
          ycbcr_feat.samplerYcbcrConversion, has_ycbcr_ext ? "listed" : "NOT listed");

   /* the image ANGLE creates for an external-format AHB */
   VkExternalFormatANDROID ext_fmt = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
                                       .externalFormat = fp.format ? 0 : fp.externalFormat };
   VkExternalMemoryImageCreateInfo emi = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &ext_fmt,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
   };
   AHardwareBuffer_Desc desc;
   AHardwareBuffer_describe(ahb, &desc);
   VkImageCreateInfo ici2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi,
                              .imageType = VK_IMAGE_TYPE_2D, .format = fp.format,
                              .extent = { desc.width, desc.height, 1 }, .mipLevels = 1,
                              .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage img;
   r = vkCreateImage(dev, &ici2, NULL, &img);
   printf("vk: vkCreateImage(external format) %d\n", r);
   if (r == VK_SUCCESS) {
      VkImportAndroidHardwareBufferInfoANDROID imp = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = ahb
      };
      VkMemoryDedicatedAllocateInfo ded = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &imp, .image = img
      };
      VkPhysicalDeviceMemoryProperties mp;
      vkGetPhysicalDeviceMemoryProperties(pd, &mp);
      uint32_t type = 0;
      while (type < mp.memoryTypeCount &&
             (!(props.memoryTypeBits & (1u << type)) ||
              !(mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
         type++;
      VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .pNext = &ded, .allocationSize = props.allocationSize,
                                   .memoryTypeIndex = type };
      VkDeviceMemory mem;
      r = vkAllocateMemory(dev, &mai, NULL, &mem);
      printf("vk: vkAllocateMemory(import AHB, type %u) %d\n", type, r);
      if (r == VK_SUCCESS) {
         r = vkBindImageMemory(dev, img, mem, 0);
         printf("vk: vkBindImageMemory %d\n", r);
         VkSamplerYcbcrConversionCreateInfo cci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, .pNext = &ext_fmt,
            .format = fp.format, .ycbcrModel = fp.suggestedYcbcrModel,
            .ycbcrRange = fp.suggestedYcbcrRange, .components = fp.samplerYcbcrConversionComponents,
            .xChromaOffset = fp.suggestedXChromaOffset, .yChromaOffset = fp.suggestedYChromaOffset,
            .chromaFilter = VK_FILTER_NEAREST
         };
         VkSamplerYcbcrConversion conv;
         r = vkCreateSamplerYcbcrConversion(dev, &cci, NULL, &conv);
         printf("vk: vkCreateSamplerYcbcrConversion %d\n", r);
         if (r == VK_SUCCESS)
            vkDestroySamplerYcbcrConversion(dev, conv, NULL);
         vkFreeMemory(dev, mem, NULL);
      }
      vkDestroyImage(dev, img, NULL);
   }
   vkDestroyDevice(dev, NULL);
   vkDestroyInstance(inst, NULL);
}

int
main(int argc, char **argv)
{
   AHardwareBuffer_Desc desc = {
      .width = argc > 2 ? atoi(argv[1]) : 720,
      .height = argc > 2 ? atoi(argv[2]) : 1280,
      .layers = 1,
      .format = getenv("FORMAT") ? strtoul(getenv("FORMAT"), NULL, 0)
                                 : 0x32315659 /* YV12 */,
      .usage = getenv("USAGE") ? strtoull(getenv("USAGE"), NULL, 0) : 0x933,
   };
   AHardwareBuffer *ahb = NULL;
   CHECK(!AHardwareBuffer_allocate(&desc, &ahb), "AHardwareBuffer_allocate");
   AHardwareBuffer_describe(ahb, &desc);
   printf("AHB %ux%u stride %u format 0x%x usage 0x%llx\n", desc.width, desc.height,
          desc.stride, desc.format, (unsigned long long)desc.usage);

   if (desc.format == 0x32315659) {
      /* Android YV12 layout: Y, then V, then U; chroma stride aligned to 16 */
      uint8_t *p = NULL;
      CHECK(!AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL,
                                  (void **)&p),
            "lock");
      const uint32_t ys = desc.stride, cs = ((ys / 2) + 15) & ~15u;
      const uint32_t ch = desc.height / 2;
      memset(p, 81, (size_t)ys * desc.height);
      memset(p + (size_t)ys * desc.height, 240, (size_t)cs * ch);
      memset(p + (size_t)ys * desc.height + (size_t)cs * ch, 90, (size_t)cs * ch);
      AHardwareBuffer_unlock(ahb, NULL);
   }

   /* the cros_gralloc_handle Mesa reads the plane layout from */
   typedef struct {
      int version, numFds, numInts, data[0];
   } native_handle_t;
   const native_handle_t *(*get_handle)(const AHardwareBuffer *) =
      dlsym(dlopen("libnativewindow.so", RTLD_NOW), "AHardwareBuffer_getNativeHandle");
   const native_handle_t *nh = get_handle ? get_handle(ahb) : NULL;
   if (nh && nh->numFds >= 1) {
      const uint32_t *u = (const uint32_t *)&nh->data[0];
      /* fds[5] then strides[4], offsets[4], sizes[4], id, w, h, format,
       * tiling, modifier(2), use_flags(2), magic, pixel_stride, droid_format,
       * usage, num_planes */
      const uint32_t *strides = u + 5, *offsets = u + 9, *sizes = u + 13;
      printf("cros: numFds %d numInts %d format 0x%x mod 0x%x%08x magic 0x%x planes %u\n",
             nh->numFds, nh->numInts, u[20], u[23], u[22], u[26], u[30]);
      for (int i = 0; i < 4; i++)
         printf("cros: plane %d stride %u offset %u size %u\n", i, strides[i], offsets[i],
                sizes[i]);
   }

   probe_vulkan(ahb);

   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   CHECK(eglInitialize(d, NULL, NULL), "eglInitialize");
   /* after eglInitialize: earlier crashes in libEGL's forwarder */
   PFNEGLDEBUGMESSAGECONTROLKHRPROC debug_control =
      (PFNEGLDEBUGMESSAGECONTROLKHRPROC)eglGetProcAddress("eglDebugMessageControlKHR");
   if (debug_control) {
      const EGLAttrib dbg_attr[] = { EGL_DEBUG_MSG_ERROR_KHR, EGL_TRUE,
                                     EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_NONE };
      debug_control(egl_debug, dbg_attr);
   }
   printf("EGL vendor %s\n", eglQueryString(d, EGL_VENDOR));
   const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                               EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                               EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
   EGLConfig cfg;
   EGLint n = 0;
   CHECK(eglChooseConfig(d, cfg_attr, &cfg, 1, &n) && n == 1, "eglChooseConfig");
   const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
   CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext");
   const EGLint pb_attr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
   EGLSurface surf = eglCreatePbufferSurface(d, cfg, pb_attr);
   CHECK(surf != EGL_NO_SURFACE, "eglCreatePbufferSurface");
   CHECK(eglMakeCurrent(d, surf, surf, ctx), "eglMakeCurrent");
   printf("GL renderer %s\n", glGetString(GL_RENDERER));

   PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getbuf =
      (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress(
         "eglGetNativeClientBufferANDROID");
   PFNEGLCREATEIMAGEKHRPROC create_image =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_tex =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
         "glEGLImageTargetTexture2DOES");
   CHECK(getbuf && create_image && target_tex, "EGL/GL extension entry points");

   const EGLint img_attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
   printf("eglCreateImageKHR...\n");
   EGLImageKHR image =
      create_image(d, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, getbuf(ahb), img_attr);
   CHECK(image != EGL_NO_IMAGE_KHR, "eglCreateImageKHR");

   GLuint tex;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
   glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   printf("glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)...\n");
   target_tex(GL_TEXTURE_EXTERNAL_OES, image);
   GLenum err = glGetError();
   CHECK(err == GL_NO_ERROR, "glEGLImageTargetTexture2DOES gl 0x%x", err);

   GLuint prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER,
                               "attribute vec2 p; varying vec2 uv;\n"
                               "void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0, 1); }\n"));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
                               "#extension GL_OES_EGL_image_external : require\n"
                               "precision mediump float; varying vec2 uv;\n"
                               "uniform samplerExternalOES t;\n"
                               "void main() { gl_FragColor = texture2D(t, uv); }\n"));
   glBindAttribLocation(prog, 0, "p");
   glLinkProgram(prog);
   GLint linked = 0;
   glGetProgramiv(prog, GL_LINK_STATUS, &linked);
   CHECK(linked, "link");
   glUseProgram(prog);
   const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
   glEnableVertexAttribArray(0);
   glViewport(0, 0, 64, 64);
   glClearColor(0, 0, 1, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   uint8_t px[4] = { 0 };
   glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   err = glGetError();
   CHECK(err == GL_NO_ERROR, "draw/readback gl 0x%x", err);
   printf("pixel rgba %u %u %u %u (expect ~255 0 0 for the red YV12 frame)\n", px[0],
          px[1], px[2], px[3]);
   printf(px[0] > 200 && px[1] < 60 && px[2] < 60 ? "PASS\n" : "WRONG COLOR\n");
   return 0;
}
