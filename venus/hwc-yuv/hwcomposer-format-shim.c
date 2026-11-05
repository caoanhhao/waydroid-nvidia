/*
 * Waydroid hwcomposer shim: send SurfaceFlinger every layer the compositor
 * cannot import, instead of passing it to Wayland as a dma-buf.
 *
 * hwcomposer.waydroid forwards each cros (minigbm) layer to the compositor
 * with zwp_linux_buffer_params_v1.create_immed, using the handle's DRM format
 * and modifier as they are.  Video frames from the software codecs are YV12
 * (DRM_FORMAT_YVU420) buffers from the Intel LINEAR path; NVIDIA EGL cannot
 * import them, Hyprland fails the buffer, the next attach of that wl_buffer
 * kills the Wayland connection and hwc_wayland_thread aborts (then
 * SurfaceFlinger aborts on DEAD_OBJECT).  See venus/notes-tiktok.md.
 *
 * This module is installed as /vendor/lib64/hw/hwcomposer.waydroid.so and
 * loads the vendor.img module from hwcomposer.waydroid.real.so.  Its prepare
 * and set mark every cros layer whose (format, modifier) is not in the
 * compositor's list (waydroid.modifiers.<fourcc>.<n>, set by hwcomposer from
 * linux-dmabuf) with HWC_SKIP_LAYER + HWC_FRAMEBUFFER, the same thing
 * HWC2On1Adapter does for client-composited layers.  hwcomposer.waydroid then
 * lets SurfaceFlinger compose them (GLES on ANGLE/Venus) into the client
 * target, which is an NVIDIA block-linear buffer.
 *
 * Only the hwcomposer.h/hardware.h layouts used here are declared below
 * (LP64), so this builds with the plain NDK.
 */
#define LOG_TAG "hwc-format-shim"
#include <android/log.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define REAL_MODULE "/vendor/lib64/hw/hwcomposer.waydroid.real.so"

/* hardware/hardware.h */
#define MAKE_TAG_CONSTANT(A, B, C, D) (((A) << 24) | ((B) << 16) | ((C) << 8) | (D))
#define HARDWARE_MODULE_TAG MAKE_TAG_CONSTANT('H', 'W', 'M', 'T')

struct hw_module_t;
struct hw_device_t;

struct hw_module_methods_t {
   int (*open)(const struct hw_module_t *module, const char *id,
               struct hw_device_t **device);
};

struct hw_module_t {
   uint32_t tag;
   uint16_t module_api_version;
   uint16_t hal_api_version;
   const char *id;
   const char *name;
   const char *author;
   struct hw_module_methods_t *methods;
   void *dso;
   uint64_t reserved[32 - 7];
};

struct hw_device_t {
   uint32_t tag;
   uint32_t version;
   struct hw_module_t *module;
   uint64_t reserved[12];
   int (*close)(struct hw_device_t *device);
};

/* system/core/libcutils/include/cutils/native_handle.h */
typedef struct native_handle {
   int version;
   int numFds;
   int numInts;
   int data[0];
} native_handle_t;

/* hardware/hwcomposer_defs.h + hwcomposer.h */
enum {
   HWC_FRAMEBUFFER = 0,
   HWC_OVERLAY = 1,
   HWC_FRAMEBUFFER_TARGET = 3,
};
enum {
   HWC_SKIP_LAYER = 0x00000001,
   HWC_IS_CURSOR_LAYER = 0x00000002,
};

typedef struct hwc_layer_1 {
   int32_t compositionType;
   uint32_t hints;
   uint32_t flags;
   const native_handle_t *handle; /* union with backgroundColor/sidebandStream */
   uint32_t transform;
   int32_t blending;
   float sourceCropf[4];
   int32_t displayFrame[4];
   size_t visibleRegionNumRects;
   const void *visibleRegionRects;
   int acquireFenceFd;
   int releaseFenceFd;
   uint8_t rest[120 - 88];
} hwc_layer_1_t;
_Static_assert(offsetof(hwc_layer_1_t, handle) == 16, "hwc_layer_1 layout");
_Static_assert(offsetof(hwc_layer_1_t, acquireFenceFd) == 80, "hwc_layer_1 layout");
_Static_assert(sizeof(hwc_layer_1_t) == 120, "hwc_layer_1 size");

typedef struct hwc_display_contents_1 {
   int retireFenceFd;
   void *u0, *u1; /* dpy/sur or outbuf/outbufAcquireFenceFd */
   uint32_t flags;
   size_t numHwLayers;
   hwc_layer_1_t hwLayers[0];
} hwc_display_contents_1_t;
_Static_assert(offsetof(hwc_display_contents_1_t, hwLayers) == 40, "contents layout");

typedef int (*hwc_list_fn)(void *dev, size_t numDisplays,
                           hwc_display_contents_1_t **displays);

typedef struct hwc_composer_device_1 {
   struct hw_device_t common;
   hwc_list_fn prepare;
   hwc_list_fn set;
   /* eventControl, blank, query, ... are left to the real module */
} hwc_composer_device_1_t;

/* minigbm cros_gralloc/cros_gralloc_handle.h (packed), as read by
 * hwcomposer.waydroid's create_buffer_cros: fds[0] at 0xc, format at 0x5c,
 * format_modifier at 0x64 */
#define CROS_WIDTH_OFF 0x54
#define CROS_HEIGHT_OFF 0x58
#define CROS_FORMAT_OFF 0x5c
#define CROS_MODIFIER_OFF 0x64
#define CROS_MAGIC_OFF 0x74
#define CROS_DROID_FORMAT_OFF 0x7c
#define CROS_USAGE_OFF 0x80
#define CROS_MAGIC 0xABCDDCBAu
#define CROS_MIN_SIZE (CROS_USAGE_OFF + 4)

#define REPORT_FILE "/data/waydroid_hwc_format_shim.txt"
#define POLICY_PROP "persist.waydroid.hwc_unsupported"

struct cros_info {
   uint32_t width, height, format, droid_format, usage;
   uint64_t modifier;
};

/* what to do with a layer the compositor can't import */
enum policy {
   POLICY_CLIENT, /* SurfaceFlinger composes it (default) */
   POLICY_DROP,   /* not shown at all */
   POLICY_OFF,    /* pass it to the compositor anyway (vendor behaviour) */
};

static struct {
   pthread_once_t once;
   const struct hw_module_t *real;
   hwc_list_fn real_prepare;
   hwc_list_fn real_set;
   pthread_mutex_t lock;
   /* layers already reported */
   struct cros_info reported[32];
   unsigned num_reported;
} shim = {
   .once = PTHREAD_ONCE_INIT,
   .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void
load_real(void)
{
   void *dso = dlopen(REAL_MODULE, RTLD_NOW | RTLD_LOCAL);
   if (!dso) {
      LOGE("dlopen %s: %s", REAL_MODULE, dlerror());
      return;
   }
   struct hw_module_t *hmi = dlsym(dso, "HMI");
   if (!hmi) {
      LOGE("%s has no HMI", REAL_MODULE);
      return;
   }
   hmi->dso = dso;
   shim.real = hmi;
}

static bool
read_cros(const native_handle_t *h, struct cros_info *info)
{
   if (!h || h->numFds < 1 || h->numInts < 0)
      return false;
   if (sizeof(native_handle_t) + sizeof(int) * (size_t)(h->numFds + h->numInts) <
       CROS_MIN_SIZE)
      return false;

   const uint8_t *p = (const uint8_t *)h;
   uint32_t magic;
   memcpy(&magic, p + CROS_MAGIC_OFF, sizeof(magic));
   if (magic != CROS_MAGIC)
      return false;
   memcpy(&info->width, p + CROS_WIDTH_OFF, sizeof(info->width));
   memcpy(&info->height, p + CROS_HEIGHT_OFF, sizeof(info->height));
   memcpy(&info->format, p + CROS_FORMAT_OFF, sizeof(info->format));
   memcpy(&info->modifier, p + CROS_MODIFIER_OFF, sizeof(info->modifier));
   memcpy(&info->droid_format, p + CROS_DROID_FORMAT_OFF, sizeof(info->droid_format));
   memcpy(&info->usage, p + CROS_USAGE_OFF, sizeof(info->usage));
   return true;
}

/* Same props and parsing as the gralloc wrapper's get_supported_modifiers. */
static bool
compositor_accepts(uint32_t format, uint64_t modifier)
{
   char name[PROP_VALUE_MAX];
   char value[PROP_VALUE_MAX];

   for (unsigned i = 0;; i++) {
      snprintf(name, sizeof(name), "waydroid.modifiers.%x.%u", format, i);
      if (__system_property_get(name, value) < 1)
         return false;
      if (strtoull(value, NULL, 16) == modifier)
         return true;
   }
}

static enum policy
get_policy(void)
{
   char value[PROP_VALUE_MAX];
   if (__system_property_get(POLICY_PROP, value) < 1)
      return POLICY_CLIENT;
   if (!strcmp(value, "drop"))
      return POLICY_DROP;
   if (!strcmp(value, "off"))
      return POLICY_OFF;
   return POLICY_CLIENT;
}

static const char *const policy_names[] = { "client", "drop", "off" };

/* Log each kind of layer once, also to a file under /data, which the host
 * can read without adb (~/.local/share/waydroid/data). */
static void
report(const struct cros_info *info, enum policy policy)
{
   pthread_mutex_lock(&shim.lock);
   for (unsigned i = 0; i < shim.num_reported; i++) {
      const struct cros_info *r = &shim.reported[i];
      if (r->format == info->format && r->modifier == info->modifier &&
          r->droid_format == info->droid_format && r->usage == info->usage) {
         pthread_mutex_unlock(&shim.lock);
         return;
      }
   }
   if (shim.num_reported < sizeof(shim.reported) / sizeof(shim.reported[0]))
      shim.reported[shim.num_reported++] = *info;
   pthread_mutex_unlock(&shim.lock);

   char msg[200];
   snprintf(msg, sizeof(msg),
            "compositor can't import %ux%u format %.4s (0x%x) modifier 0x%" PRIx64
            " droid_format 0x%x usage 0x%x: %s",
            info->width, info->height, (const char *)&info->format, info->format,
            info->modifier, info->droid_format, info->usage, policy_names[policy]);
   LOGI("%s", msg);
   FILE *f = fopen(REPORT_FILE, "a");
   if (f) {
      struct timespec rt;
      clock_gettime(CLOCK_REALTIME, &rt);
      fprintf(f, "[%lld] %s\n", (long long)rt.tv_sec, msg);
      fclose(f);
   }
}

static bool
unsupported_layer(const hwc_layer_1_t *layer, struct cros_info *info)
{
   if (layer->compositionType == HWC_FRAMEBUFFER_TARGET)
      return false;
   /* the cursor goes to the compositor as wl_shm (cursor_force_shm unset) */
   if (layer->flags & HWC_IS_CURSOR_LAYER)
      return false;
   if (!read_cros(layer->handle, info))
      return false;
   return !compositor_accepts(info->format, info->modifier);
}

static int
shim_prepare(void *dev, size_t numDisplays, hwc_display_contents_1_t **displays)
{
   const enum policy policy = get_policy();

   for (size_t d = 0; displays && d < numDisplays; d++) {
      hwc_display_contents_1_t *contents = displays[d];
      for (size_t i = 0; contents && i < contents->numHwLayers; i++) {
         hwc_layer_1_t *layer = &contents->hwLayers[i];
         struct cros_info info;
         if (!unsupported_layer(layer, &info))
            continue;
         report(&info, policy);
         if (policy == POLICY_CLIENT) {
            layer->compositionType = HWC_FRAMEBUFFER;
            layer->flags |= HWC_SKIP_LAYER;
         }
      }
   }
   return shim.real_prepare(dev, numDisplays, displays);
}

#define MAX_HIDDEN 16

static int
shim_set(void *dev, size_t numDisplays, hwc_display_contents_1_t **displays)
{
   const enum policy policy = get_policy();
   /* POLICY_DROP: hwcomposer.waydroid skips overlay layers with no handle */
   hwc_layer_1_t *hidden[MAX_HIDDEN];
   const native_handle_t *hidden_handle[MAX_HIDDEN];
   unsigned num_hidden = 0;

   for (size_t d = 0; displays && d < numDisplays; d++) {
      hwc_display_contents_1_t *contents = displays[d];
      for (size_t i = 0; contents && i < contents->numHwLayers; i++) {
         hwc_layer_1_t *layer = &contents->hwLayers[i];
         struct cros_info info;
         if (!unsupported_layer(layer, &info))
            continue;
         if (layer->compositionType == HWC_FRAMEBUFFER) {
            /* prepare left it to SurfaceFlinger: keep it marked */
            layer->flags |= HWC_SKIP_LAYER;
         } else if (policy == POLICY_DROP && layer->compositionType == HWC_OVERLAY &&
                    num_hidden < MAX_HIDDEN) {
            if (layer->acquireFenceFd >= 0) {
               close(layer->acquireFenceFd);
               layer->acquireFenceFd = -1;
            }
            hidden[num_hidden] = layer;
            hidden_handle[num_hidden] = layer->handle;
            num_hidden++;
            layer->handle = NULL;
         }
      }
   }

   int ret = shim.real_set(dev, numDisplays, displays);
   for (unsigned i = 0; i < num_hidden; i++)
      hidden[i]->handle = hidden_handle[i];
   return ret;
}

static int
shim_open(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
   (void)module;
   pthread_once(&shim.once, load_real);
   if (!shim.real)
      return -1;

   int ret = shim.real->methods->open(shim.real, id, device);
   if (ret || !*device)
      return ret;

   hwc_composer_device_1_t *hwc = (hwc_composer_device_1_t *)*device;
   if (!hwc->prepare || !hwc->set) {
      LOGE("real device %s has no prepare/set, not wrapping", id);
      return 0;
   }
   shim.real_prepare = hwc->prepare;
   shim.real_set = hwc->set;
   hwc->prepare = shim_prepare;
   hwc->set = shim_set;
   LOGI("wrapped %s from %s (device version 0x%x)", id, REAL_MODULE,
        hwc->common.version);
   return 0;
}

static struct hw_module_methods_t shim_methods = {
   .open = shim_open,
};

__attribute__((visibility("default"))) struct hw_module_t HMI = {
   .tag = HARDWARE_MODULE_TAG,
   .module_api_version = 0x0001, /* HWC_MODULE_API_VERSION_0_1 */
   .hal_api_version = 0,
   .id = "hwcomposer",
   .name = "Waydroid hwcomposer format shim",
   .author = "waydroid-nvidia",
   .methods = &shim_methods,
};
