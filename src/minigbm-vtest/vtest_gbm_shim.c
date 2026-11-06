/*
 * Copyright 2026 waydroid-nvidia contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * libgbm_mesa_wrapper.so shim for minigbm_gbm_mesa: GPU-only window buffers
 * are allocated on the host GPU through vtest (VCMD_ALLOC_DMABUF_IMAGE) and
 * carry the host's real DRM modifier (NVIDIA block-linear); everything else
 * (CPU-usage buffers, mapper import/map, unsupported formats, any vtest
 * failure) goes to the original wrapper, installed next to it as
 * libgbm_mesa_wrapper_intel.so.
 *
 * Properties:
 *   waydroid.gralloc.vtest          1 = enable (default 0: pass-through only)
 *   waydroid.gralloc.vtest.socket   socket path (default: mesa.vtest.socket.name,
 *                                   then /data/vtest/vtest.sock)
 *   waydroid.gralloc.vtest.inner    inner wrapper (default
 *                                   /vendor/lib64/libgbm_mesa_wrapper_intel.so)
 *   waydroid.modifiers.<fourcc>.<n> allowed modifiers (set by hwcomposer from
 *                                   the compositor); empty = any host modifier
 */

#define LOG_TAG "GBM-VTEST-SHIM"

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <stdint.h>

#include "gbm_mesa_wrapper.h"
#include "vtest_alloc_client.h"
#include "vtest_protocol.h"

/* marker for the deploy script: never install this file as the inner wrapper */
__attribute__((used, visibility("default"))) const char vtest_gbm_shim_marker[] =
   "vtest_gbm_shim";

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
static int
prop_get(const char *name, char *value)
{
   return __system_property_get(name, value);
}
#else
/* host test build: properties come from the environment, with '.' -> '_' */
#define PROP_VALUE_MAX 92
#define LOGI(...) (fprintf(stderr, LOG_TAG ": " __VA_ARGS__), fputc('\n', stderr))
#define LOGE(...) LOGI(__VA_ARGS__)
static int
prop_get(const char *name, char *value)
{
   char env[128];
   snprintf(env, sizeof(env), "%s", name);
   for (char *p = env; *p; p++) {
      if (*p == '.')
         *p = '_';
   }
   const char *v = getenv(env);
   if (!v) {
      value[0] = '\0';
      return 0;
   }
   snprintf(value, PROP_VALUE_MAX, "%s", v);
   return strlen(value);
}
#endif

#define DRM_FOURCC(a, b, c, d)                                                \
   ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |           \
    ((uint32_t)(d) << 24))

/* single-plane formats the server maps to a VkFormat */
static const uint32_t vtest_formats[] = {
   DRM_FOURCC('A', 'B', '2', '4'), /* ABGR8888 */
   DRM_FOURCC('X', 'B', '2', '4'), /* XBGR8888 */
   DRM_FOURCC('A', 'R', '2', '4'), /* ARGB8888 */
   DRM_FOURCC('X', 'R', '2', '4'), /* XRGB8888 */
   DRM_FOURCC('R', 'G', '1', '6'), /* RGB565 */
   DRM_FOURCC('A', 'B', '3', '0'), /* ABGR2101010 */
   DRM_FOURCC('X', 'B', '3', '0'), /* XBGR2101010 */
   DRM_FOURCC('A', 'B', '4', 'H'), /* ABGR16161616F */
   DRM_FOURCC('X', 'B', '4', 'H'), /* XBGR16161616F */
};

#define MAX_MODIFIERS VCMD_ALLOC_DMABUF_IMAGE_MAX_MODIFIERS

static struct {
   pthread_once_t once;
   pthread_mutex_t lock;

   struct gbm_ops *inner;
   struct gbm_ops ops;

   bool enabled;
   char socket_path[PROP_VALUE_MAX];
   int sock;
   bool warned;

   struct {
      uint32_t format;
      uint32_t count;
      uint64_t modifiers[MAX_MODIFIERS];
   } mod_cache[sizeof(vtest_formats) / sizeof(vtest_formats[0])];
} shim = {
   .once = PTHREAD_ONCE_INIT,
   .lock = PTHREAD_MUTEX_INITIALIZER,
   .sock = -1,
};

static int
format_index(uint32_t drm_format)
{
   for (unsigned i = 0; i < sizeof(vtest_formats) / sizeof(vtest_formats[0]); i++) {
      if (vtest_formats[i] == drm_format)
         return i;
   }
   return -1;
}

/* Same props and parsing as the original wrapper's get_supported_modifiers. */
static void
load_modifiers(int idx, uint32_t drm_format)
{
   char name[PROP_VALUE_MAX];
   char value[PROP_VALUE_MAX];

   shim.mod_cache[idx].format = drm_format;
   shim.mod_cache[idx].count = 0;
   for (uint32_t i = 0; i < MAX_MODIFIERS; i++) {
      snprintf(name, sizeof(name), "waydroid.modifiers.%x.%u", drm_format, i);
      if (prop_get(name, value) < 1)
         break;
      shim.mod_cache[idx].modifiers[i] = strtoull(value, NULL, 16);
      shim.mod_cache[idx].count = i + 1;
   }
}

static int
vtest_alloc(struct alloc_args *args)
{
   const int idx = format_index(args->drm_format);
   if (idx < 0)
      return -ENOTSUP;

   pthread_mutex_lock(&shim.lock);

   /* hwcomposer sets the modifier props when it connects to the compositor,
    * which can be after our first allocation: reload until we have a list
    */
   if (!shim.mod_cache[idx].count)
      load_modifiers(idx, args->drm_format);

   uint32_t flags = VCMD_ALLOC_DMABUF_IMAGE_FLAG_RENDER |
                    VCMD_ALLOC_DMABUF_IMAGE_FLAG_TEXTURE;
   if (args->use_scanout)
      flags |= VCMD_ALLOC_DMABUF_IMAGE_FLAG_SCANOUT;

   struct vtest_alloc_buffer buf;
   int ret = -EPIPE;
   for (int attempt = 0; attempt < 2 && ret == -EPIPE; attempt++) {
      if (shim.sock < 0) {
         shim.sock = vtest_alloc_connect(shim.socket_path, "gralloc");
         if (shim.sock < 0) {
            ret = shim.sock;
            shim.sock = -1;
            break;
         }
      }
      ret = vtest_alloc_dmabuf_image(shim.sock, args->width, args->height,
                                     args->drm_format, flags,
                                     shim.mod_cache[idx].modifiers,
                                     shim.mod_cache[idx].count, &buf);
      if (ret == -EPIPE) {
         close(shim.sock);
         shim.sock = -1;
      }
   }

   const bool warn = ret && !shim.warned;
   if (ret)
      shim.warned = true;
   pthread_mutex_unlock(&shim.lock);

   if (ret) {
      if (warn) {
         LOGE("vtest allocation via %s failed (%s), falling back to the "
              "inner wrapper", shim.socket_path, strerror(-ret));
      }
      return ret;
   }

   /* minigbm derives plane offsets itself and assumes plane 0 at 0 */
   if (buf.offset != 0) {
      LOGE("unexpected plane offset %u, falling back", buf.offset);
      close(buf.fd);
      return -EINVAL;
   }

   args->out_fd = buf.fd;
   args->out_stride = buf.stride;
   args->out_modifier = buf.modifier;
   args->out_map_stride = 0;
   return 0;
}

static int
shim_alloc(struct alloc_args *args)
{
   /* CPU-usage buffers must be mappable through gbm on the guest device */
   if (shim.enabled && !args->force_linear && !args->needs_map_stride &&
       !vtest_alloc(args))
      return 0;

   return shim.inner->alloc(args);
}

static void
shim_init(void)
{
   char value[PROP_VALUE_MAX];
   /* by path: the renamed inner library still has the SONAME
    * libgbm_mesa_wrapper.so, the same as ours
    */
   char inner_name[PROP_VALUE_MAX] = "/vendor/lib64/libgbm_mesa_wrapper_intel.so";

   prop_get("waydroid.gralloc.vtest.inner", value);
   if (value[0])
      snprintf(inner_name, sizeof(inner_name), "%s", value);

   void *lib = dlopen(inner_name, RTLD_NOW | RTLD_LOCAL);
   struct gbm_ops *(*get_ops)(void) =
      lib ? (struct gbm_ops * (*)(void)) dlsym(lib, "get_gbm_ops") : NULL;
   shim.inner = get_ops ? get_ops() : NULL;
   if (!shim.inner) {
      LOGE("cannot load inner wrapper %s: %s", inner_name, dlerror());
      return;
   }

   shim.ops = *shim.inner;
   shim.ops.alloc = shim_alloc;

   prop_get("waydroid.gralloc.vtest", value);
   shim.enabled = value[0] == '1' || !strcmp(value, "true");

   prop_get("waydroid.gralloc.vtest.socket", shim.socket_path);
   if (!shim.socket_path[0])
      prop_get("mesa.vtest.socket.name", shim.socket_path);
   if (!shim.socket_path[0])
      snprintf(shim.socket_path, sizeof(shim.socket_path), "/data/vtest/vtest.sock");

   LOGI("inner wrapper %s, host allocation via vtest %s (%s)", inner_name,
        shim.enabled ? "enabled" : "disabled", shim.socket_path);
}

__attribute__((visibility("default"))) struct gbm_ops *
get_gbm_ops(void)
{
   pthread_once(&shim.once, shim_init);
   return shim.inner ? &shim.ops : NULL;
}
