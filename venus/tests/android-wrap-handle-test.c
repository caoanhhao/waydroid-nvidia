/*
 * Android-side reproducer for the Waydroid hwcomposer cursor crash: mimic
 * egl_render_to_pixels() (hardware/waydroid/hwcomposer/egl-tools.cpp):
 *   - EGL set up like egl_init(): RGBA8888 pbuffer config, ES2 context and a
 *     pbuffer surface, current on a separate thread;
 *   - the buffer handle wrapped with GraphicBuffer(WRAP_HANDLE) using the
 *     width/height/pixel_stride/droid_format of the cros_gralloc_handle and
 *     usage USAGE_HW_TEXTURE only (libui is called through dlsym);
 *   - eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) + glReadPixels.
 *
 * usage: android-wrap-handle-test [w h]
 *   USAGE=0x...  allocation usage (default 0xb33, a software-drawn cursor
 *                surface: CPU read/write often + GPU sampled + composer)
 *   FORMAT=n     AHardwareBuffer format (default 1, RGBA_8888)
 *   SRC=ahb      use the AHB's own ANativeWindowBuffer instead of a
 *                WRAP_HANDLE GraphicBuffer (the normal-process path)
 *   WRAP_USAGE=  usage given to the wrapping GraphicBuffer (default 0x100)
 *
 * The binary links none of libEGL/libGLESv2/libnativewindow/libandroid; they
 * are dlopen'ed RTLD_LOCAL, so like composer@2.1-service, the executable's
 * global group has no AHardwareBuffer_* symbols.  Android 13 ANGLE resolves
 * those with dlsym(dlopen(NULL)) (AHBFunctions.cpp), so without
 * LD_PRELOAD=libnativewindow.so the import fails and ANGLE's error path calls
 * a NULL AHardwareBuffer_release.
 * build: NDK clang -ldl -lpthread
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cros_gralloc/cros_gralloc_handle.h, after native_handle_t's 3 ints */
struct cros_handle {
   int version, numFds, numInts;
   int32_t fds[5];
   uint32_t strides[4], offsets[4], sizes[4];
   uint32_t id, width, height, format, tiling;
   uint64_t format_modifier, use_flags;
   uint32_t magic, pixel_stride;
   int32_t droid_format, usage;
   uint32_t num_planes;
   uint64_t reserved_region_size, total_size;
} __attribute__((packed));

typedef void (*gb_ctor_t)(void *self, const void *handle, uint8_t method,
                          uint32_t w, uint32_t h, int32_t format,
                          uint32_t layers, uint64_t usage, uint32_t stride);
typedef void *(*gb_native_t)(const void *self);

static AHardwareBuffer *ahb;

/* everything through dlsym, see the header */
static void *lib_egl, *lib_gles, *lib_nw;
#define FN(lib, name) ((__typeof__(&name))dlsym_or_die(lib, #name))
static void *
dlsym_or_die(void *lib, const char *name)
{
   void *f = dlsym(lib, name);
   if (!f) {
      printf("dlsym %s: %s\n", name, dlerror());
      exit(2);
   }
   return f;
}
#define eglGetProcAddress FN(lib_egl, eglGetProcAddress)
#define eglGetDisplay FN(lib_egl, eglGetDisplay)
#define eglInitialize FN(lib_egl, eglInitialize)
#define eglQueryString FN(lib_egl, eglQueryString)
#define eglChooseConfig FN(lib_egl, eglChooseConfig)
#define eglCreateContext FN(lib_egl, eglCreateContext)
#define eglCreatePbufferSurface FN(lib_egl, eglCreatePbufferSurface)
#define eglMakeCurrent FN(lib_egl, eglMakeCurrent)
#define eglGetError FN(lib_egl, eglGetError)
#define eglCreateImageKHR ((PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR"))
#define eglDestroyImageKHR ((PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR"))
#define eglGetNativeClientBufferANDROID \
   ((PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID"))
#define glGetString FN(lib_gles, glGetString)
#define glGenFramebuffers FN(lib_gles, glGenFramebuffers)
#define glBindFramebuffer FN(lib_gles, glBindFramebuffer)
#define glGenTextures FN(lib_gles, glGenTextures)
#define glBindTexture FN(lib_gles, glBindTexture)
#define glTexParameteri FN(lib_gles, glTexParameteri)
#define glEGLImageTargetTexture2DOES \
   ((PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES"))
#define glFramebufferTexture2D FN(lib_gles, glFramebufferTexture2D)
#define glReadPixels FN(lib_gles, glReadPixels)
#define glGetError FN(lib_gles, glGetError)
#define glCheckFramebufferStatus FN(lib_gles, glCheckFramebufferStatus)
#define glDeleteTextures FN(lib_gles, glDeleteTextures)
#define AHardwareBuffer_allocate FN(lib_nw, AHardwareBuffer_allocate)
#define AHardwareBuffer_describe FN(lib_nw, AHardwareBuffer_describe)
#define AHardwareBuffer_lock FN(lib_nw, AHardwareBuffer_lock)
#define AHardwareBuffer_unlock FN(lib_nw, AHardwareBuffer_unlock)
static uint32_t W = 25, H = 32;

static void EGLAPIENTRY
egl_debug(EGLenum error, const char *command, EGLint type, EGLLabelKHR thread,
          EGLLabelKHR object, const char *message)
{
   printf("EGL debug: %s error 0x%x: %s\n", command, error, message ? message : "");
}

static void *
client_buffer(void)
{
   if (getenv("SRC") && !strcmp(getenv("SRC"), "ahb")) {
      printf("source: AHB's own ANativeWindowBuffer\n");
      return eglGetNativeClientBufferANDROID(ahb);
   }

   /* libnativewindow exports it, but the NDK headers hide it */
   const void *(*get_native_handle)(const AHardwareBuffer *) =
      dlsym(lib_nw, "AHardwareBuffer_getNativeHandle");
   const struct cros_handle *h = get_native_handle(ahb);
   printf("handle: fds %d ints %d magic 0x%x %ux%u drm 0x%x mod 0x%llx "
          "pixel_stride %u droid_format %d usage 0x%x planes %u stride0 %u\n",
          h->numFds, h->numInts, h->magic, h->width, h->height, h->format,
          (unsigned long long)h->format_modifier, h->pixel_stride,
          h->droid_format, h->usage, h->num_planes, h->strides[0]);

   void *ui = dlopen("libui.so", RTLD_NOW);
   gb_ctor_t ctor = ui ? (gb_ctor_t)dlsym(ui,
      "_ZN7android13GraphicBufferC1EPK13native_handleNS0_16HandleWrapMethodEjjijmj") : NULL;
   gb_native_t native = ui ? (gb_native_t)dlsym(ui,
      "_ZNK7android13GraphicBuffer15getNativeBufferEv") : NULL;
   if (!ctor || !native) {
      printf("libui: %s\n", dlerror());
      exit(1);
   }
   const uint64_t wrap_usage =
      getenv("WRAP_USAGE") ? strtoull(getenv("WRAP_USAGE"), NULL, 0) : 0x100;
   /* leaked on purpose: sizeof(GraphicBuffer) is well below this */
   void *gb = calloc(1, 1024);
   ctor(gb, h, 0 /* WRAP_HANDLE */, h->width, h->height, h->droid_format, 1,
        wrap_usage, h->pixel_stride);
   printf("source: GraphicBuffer(WRAP_HANDLE, usage 0x%llx)\n",
          (unsigned long long)wrap_usage);
   return native(gb);
}

static void *
egl_thread(void *arg)
{
   /* egl_init() */
   EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   eglInitialize(d, NULL, NULL);
   /* the loader's forwarder for this crashes before eglInitialize */
   PFNEGLDEBUGMESSAGECONTROLKHRPROC debug_control =
      (PFNEGLDEBUGMESSAGECONTROLKHRPROC)eglGetProcAddress("eglDebugMessageControlKHR");
   if (debug_control) {
      const EGLAttrib attribs[] = { EGL_DEBUG_MSG_CRITICAL_KHR, EGL_TRUE,
                                    EGL_DEBUG_MSG_ERROR_KHR, EGL_TRUE,
                                    EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE,
                                    EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE };
      debug_control(egl_debug, attribs);
   }

   printf("EGL: %s / %s\n", eglQueryString(d, EGL_VENDOR), eglQueryString(d, EGL_VERSION));
   const EGLint cfg_attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
   EGLConfig config;
   EGLint n = 0;
   eglChooseConfig(d, cfg_attrs, &config, 1, &n);
   const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, ctx_attrs);
   const EGLint pbuf_attrs[] = { EGL_WIDTH, 2560, EGL_HEIGHT, 1068, EGL_NONE };
   EGLSurface pbuf = eglCreatePbufferSurface(d, config, pbuf_attrs);
   const EGLBoolean cur = eglMakeCurrent(d, pbuf, pbuf, ctx);
   printf("EGL: configs %d ctx %p pbuf %p current %d err 0x%x\n", n, ctx, pbuf,
          cur, eglGetError());
   printf("GL: %s\n", glGetString(GL_RENDERER));
   GLuint fbo;
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);

   /* egl_render_to_pixels() */
   void *cb = client_buffer();
   const EGLint image_attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
   printf("eglCreateImageKHR...\n");
   fflush(stdout);
   EGLImageKHR image = eglCreateImageKHR(d, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                         (EGLClientBuffer)cb, image_attrs);
   if (image == EGL_NO_IMAGE_KHR) {
      printf("eglCreateImageKHR FAILED 0x%x\n", eglGetError());
      return (void *)1;
   }
   GLuint tex;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   uint32_t *px = calloc(W * H, 4);
   glReadPixels(0, 0, W, H, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
   const GLenum err = glGetError();
   printf("image ok: fbo 0x%x glError 0x%x pixel[0] 0x%08x\n",
          glCheckFramebufferStatus(GL_FRAMEBUFFER), err, px[0]);
   glDeleteTextures(1, &tex);
   eglDestroyImageKHR(d, image);
   return (void *)(uintptr_t)(err != GL_NO_ERROR);
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   lib_nw = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
   lib_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
   lib_gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
   if (!lib_nw || !lib_egl || !lib_gles) {
      printf("dlopen: %s\n", dlerror());
      return 2;
   }
   void *global = dlopen(NULL, RTLD_NOW);
   printf("AHardwareBuffer_acquire in the global group (what ANGLE sees): %p\n",
          dlsym(global, "AHardwareBuffer_acquire"));
   if (argc >= 3) {
      W = atoi(argv[1]);
      H = atoi(argv[2]);
   }
   AHardwareBuffer_Desc desc = {
      .width = W,
      .height = H,
      .layers = 1,
      .format = getenv("FORMAT") ? (uint32_t)strtoul(getenv("FORMAT"), NULL, 0)
                                 : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      .usage = getenv("USAGE") ? strtoull(getenv("USAGE"), NULL, 0) : 0xb33,
   };
   if (AHardwareBuffer_allocate(&desc, &ahb)) {
      printf("AHardwareBuffer_allocate failed\n");
      return 1;
   }
   /* fill with a known pattern through the CPU, like the pointer icon */
   void *map;
   if ((desc.usage & AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK) &&
       !AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &map)) {
      AHardwareBuffer_describe(ahb, &desc);
      for (uint32_t y = 0; y < H; y++)
         for (uint32_t x = 0; x < W; x++)
            ((uint32_t *)map)[y * desc.stride + x] = 0xff0000ff; /* opaque red */
      AHardwareBuffer_unlock(ahb, NULL);
   }
   AHardwareBuffer_describe(ahb, &desc);
   printf("AHB %ux%u stride %u format %u usage 0x%llx\n", desc.width, desc.height,
          desc.stride, desc.format, (unsigned long long)desc.usage);

   pthread_t t;
   void *ret;
   pthread_create(&t, NULL, egl_thread, NULL);
   pthread_join(t, &ret);
   printf("RESULT: %s\n", ret ? "FAIL" : "PASS");
   return !!ret;
}
