/*
 * Waydroid hwcomposer wrapper: run the real composer with libnativewindow
 * preloaded, so Android 13 ANGLE can import EGL images in it.
 *
 * ANGLE resolves AHardwareBuffer_acquire/describe/release with
 * dlsym(dlopen(NULL)) (AHBFunctions.cpp), which only searches the
 * executable's own dependency tree; composer@2.1-service does not link
 * libnativewindow, so every eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) there
 * fails and ANGLE's error path calls a NULL AHardwareBuffer_release.
 * LD_PRELOAD libraries count as children of the executable.
 *
 * Installed through the Waydroid overlay over
 * /vendor/bin/hw/android.hardware.graphics.composer@2.1-service, with the
 * vendor.img binary next to it as ...-service.real.  This avoids redefining
 * the init service: an "override" in /vendor/etc/init was not applied.
 * See venus/notes-cursor.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REAL "/vendor/bin/hw/android.hardware.graphics.composer@2.1-service.real"
#define PRELOAD "libnativewindow.so"

int
main(int argc, char **argv)
{
   const char *old = getenv("LD_PRELOAD");
   if (old && *old) {
      char *both = malloc(strlen(PRELOAD) + 1 + strlen(old) + 1);
      if (both) {
         sprintf(both, "%s:%s", PRELOAD, old);
         setenv("LD_PRELOAD", both, 1);
      }
   } else {
      setenv("LD_PRELOAD", PRELOAD, 1);
   }

   /* argv[0] stays the service path, so pidof and init still match it */
   execv(REAL, argv);
   perror("composer-preload-wrapper: execv " REAL);
   return 127;
}
