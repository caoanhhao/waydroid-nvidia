/* GLES driver shim over ANGLE that changes GL_VENDOR / GL_RENDERER for chosen apps.
 *
 * Installed as ro.hardware.egl=gpuid: Android's EGL loader opens libEGL_gpuid.so,
 * libGLESv1_CM_gpuid.so and libGLESv2_gpuid.so. Each has the matching ANGLE library as
 * DT_NEEDED, so every symbol the loader looks up resolves to ANGLE, except glGetString,
 * which the GLESv2 shim defines (and only it is built from this file).
 *
 * /vendor/etc/gpuid.conf lists the apps, one per line, '|'-separated:
 *     <process name>|<GL_VENDOR>|<GL_RENDERER>
 * Every other process (SurfaceFlinger, HWUI, other apps) sees ANGLE's strings unchanged. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <GLES2/gl2.h>

#define CONF "/vendor/etc/gpuid.conf"

static int loaded;
static char vendor[128], renderer[128];

static void load(void)
{
    char self[256] = "", line[512];
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (f) {
        fread(self, 1, sizeof(self) - 1, f);
        fclose(f);
    }
    f = fopen(CONF, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '|'), *r = v ? strchr(v + 1, '|') : NULL;
        if (line[0] == '#' || !r)
            continue;
        *v++ = 0;
        *r++ = 0;
        r[strcspn(r, "\r\n")] = 0;
        if (strcmp(line, self) == 0) {
            snprintf(vendor, sizeof(vendor), "%s", v);
            snprintf(renderer, sizeof(renderer), "%s", r);
            break;
        }
    }
    fclose(f);
}

const GLubyte *glGetString(GLenum name)
{
    static const GLubyte *(*real)(GLenum);
    if (!real) {
        void *angle = dlopen("libGLESv2_angle.so", RTLD_NOW | RTLD_NOLOAD);
        real = angle ? dlsym(angle, "glGetString") : NULL;
        if (!real)
            return NULL;
    }
    if (!__atomic_load_n(&loaded, __ATOMIC_ACQUIRE)) {
        load();
        __atomic_store_n(&loaded, 1, __ATOMIC_RELEASE);
    }
    if (name == GL_VENDOR && vendor[0])
        return (const GLubyte *)vendor;
    if (name == GL_RENDERER && renderer[0])
        return (const GLubyte *)renderer;
    return real(name);
}
