/* What a process of this ABI sees as its GPU: the Vulkan physical devices and the GLES
 * context EGL gives it. Built for every ABI the image runs (x86_64, x86, and arm64-v8a /
 * armeabi-v7a through the houdini native bridge) by run-gpuinfo.sh. */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <vulkan/vulkan.h>

#if defined(__x86_64__)
#define ABI "x86_64"
#elif defined(__i386__)
#define ABI "x86"
#elif defined(__aarch64__)
#define ABI "arm64-v8a"
#else
#define ABI "armeabi-v7a"
#endif

static void vulkan(void)
{
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    VkInstance inst;
    uint32_t iv = 0, n = 8;
    VkPhysicalDevice devs[8];

    vkEnumerateInstanceVersion(&iv);
    VkResult r = vkCreateInstance(&ci, NULL, &inst);
    if (r != VK_SUCCESS) {
        printf("vulkan: vkCreateInstance failed (%d)\n", r);
        return;
    }
    vkEnumeratePhysicalDevices(inst, &n, devs);
    printf("vulkan: instance %u.%u, %u device(s)\n", VK_API_VERSION_MAJOR(iv),
           VK_API_VERSION_MINOR(iv), n);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceDriverProperties drv = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                          .pNext = &drv };
        vkGetPhysicalDeviceProperties2(devs[i], &p);
        uint32_t a = p.properties.apiVersion;
        printf("vulkan: [%u] \"%s\" vendor 0x%04x device 0x%04x type %d api %u.%u.%u driver \"%s\" (%s)\n",
               i, p.properties.deviceName, p.properties.vendorID, p.properties.deviceID,
               p.properties.deviceType, VK_API_VERSION_MAJOR(a), VK_API_VERSION_MINOR(a),
               VK_API_VERSION_PATCH(a), drv.driverName, drv.driverInfo);
    }
    vkDestroyInstance(inst, NULL);
}

static void gles(void)
{
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min, nc;
    EGLConfig cfg;
    if (!eglInitialize(d, &maj, &min)) {
        printf("egl: eglInitialize failed 0x%x\n", eglGetError());
        return;
    }
    const EGLint ca[] = { EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */,
                          EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    const EGLint sa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    if (!eglChooseConfig(d, ca, &cfg, 1, &nc) || !nc) {
        printf("egl: no ES3 pbuffer config\n");
        return;
    }
    /* ask for the highest version first: 3.2, then 3.1, 3.0 */
    EGLContext c = EGL_NO_CONTEXT;
    for (int m = 2; m >= 0 && c == EGL_NO_CONTEXT; m--) {
        const EGLint xa[] = { 0x3098 /* MAJOR */, 3, 0x30FB /* MINOR */, m, EGL_NONE };
        c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    }
    EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, s, s, c)) {
        printf("egl: no ES3 context (0x%x)\n", eglGetError());
        return;
    }
    printf("egl: %d.%d \"%s\"\n", maj, min, eglQueryString(d, EGL_VENDOR));
    printf("gles: vendor   \"%s\"\n", glGetString(GL_VENDOR));
    printf("gles: renderer \"%s\"\n", glGetString(GL_RENDERER));
    printf("gles: version  \"%s\"\n", glGetString(GL_VERSION));
    printf("gles: glsl     \"%s\"\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    /* the Android Extension Pack and what ES 3.2 adds on top of 3.1 */
    static const char *aep[] = {
        "GL_KHR_debug", "GL_KHR_texture_compression_astc_ldr", "GL_KHR_blend_equation_advanced",
        "GL_OES_sample_shading", "GL_OES_sample_variables", "GL_OES_shader_image_atomic",
        "GL_OES_shader_multisample_interpolation", "GL_OES_texture_stencil8",
        "GL_OES_texture_storage_multisample_2d_array", "GL_EXT_copy_image",
        "GL_EXT_draw_buffers_indexed", "GL_EXT_geometry_shader", "GL_EXT_gpu_shader5",
        "GL_EXT_primitive_bounding_box", "GL_EXT_shader_io_blocks", "GL_EXT_tessellation_shader",
        "GL_EXT_texture_border_clamp", "GL_EXT_texture_buffer", "GL_EXT_texture_cube_map_array",
        "GL_EXT_texture_sRGB_decode", NULL };
    printf("gles: AEP missing:");
    for (int i = 0; aep[i]; i++) {
        const char *p = ext ? strstr(ext, aep[i]) : NULL;
        size_t l = strlen(aep[i]);
        if (!p || (p[l] && p[l] != ' '))
            printf(" %s", aep[i]);
    }
    printf("\n");
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(d);
}

int main(void)
{
    Dl_info info;
    printf("== %s\n", ABI);
    vulkan();
    if (dladdr((void *)vkCreateInstance, &info))
        printf("libvulkan: %s\n", info.dli_fname);
    gles();
    return 0;
}
