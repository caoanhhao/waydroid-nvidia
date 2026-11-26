/* Vulkan HAL shim that overrides VkPhysicalDeviceProperties.{deviceName,vendorID,deviceID}
 * for selected processes. All other Vulkan calls go through to the real Venus driver.
 *
 * Install path: /vendor/lib{64,}/hw/vulkan.virtio.so (replacing the real Venus driver,
 * which is moved to vulkan.virtio_real.so). Config: /vendor/etc/wwm_vkhook.conf, one line
 * per process:
 *     <cmdline>|<vendorID>|<deviceID>|<deviceName>
 * vendorID/deviceID may be 0 to leave the Venus value untouched; 0x-prefixed hex works.
 *
 * Android's Vulkan loader opens this module via hw_get_module("vulkan") and expects the
 * HAL_MODULE_INFO_SYM ("HMI") struct plus a hwvulkan_device_t with CreateInstance and
 * GetInstanceProcAddr. Inline copies of those structs below keep the shim NDK-buildable.
 *
 * The device_name whitelist some engines (Messiah: Where Winds Meet) check for mobile GPUs
 * (Adreno, Mali) and silently fall back to a text-only render path when the device is not
 * recognised. Giving them "Adreno (TM) 740" over Venus keeps the render on the host GPU.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <android/log.h>
#include <vulkan/vulkan.h>

#define TAG "wwm_vkhook"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* Minimal libhardware / hwvulkan definitions, copied from AOSP headers so we do not need
 * libhardware at build time. These are ABI, not API, and have been stable since Android 7. */
#define HARDWARE_MODULE_TAG  0x4D4F4448u  /* 'HDMO' */
#define HARDWARE_DEVICE_TAG  0x44455648u  /* 'HVED' */
#define HWVULKAN_MODULE_API_VERSION_0_1 (1u << 8)
#define HWVULKAN_DEVICE_API_VERSION_0_1 (1u << 8)
#define HARDWARE_HAL_API_VERSION        (1u << 8)
#define HWVULKAN_HARDWARE_MODULE_ID     "vulkan"
#define HWVULKAN_DEVICE_0               "vk0"

struct hw_module_t;
struct hw_device_t;

typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *m, const char *id, struct hw_device_t **d);
} hw_module_methods_t;

typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
} hw_module_t;

typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *d);
} hw_device_t;

typedef struct hwvulkan_device_t {
    hw_device_t common;
    VkResult (*EnumerateInstanceExtensionProperties)(const char *, uint32_t *,
                                                     VkExtensionProperties *);
    VkResult (*CreateInstance)(const VkInstanceCreateInfo *, const VkAllocationCallbacks *,
                               VkInstance *);
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

#define CONF    "/vendor/etc/wwm_vkhook.conf"
#define REAL_SO "vulkan.virtio_real.so"

static pid_t cfg_pid;  /* pid for which `ov` was loaded; 0 = never loaded, -1 = loaded but no match */
static struct {
    bool active;
    uint32_t vendorID, deviceID;
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
} ov;

static hw_module_t *real_module;
static hwvulkan_device_t *real_dev;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
static PFN_vkCreateInstance real_ci;
static PFN_vkCreateDevice real_cd;
static void (VKAPI_PTR *real_gpdp)(VkPhysicalDevice, VkPhysicalDeviceProperties *);
static void (VKAPI_PTR *real_gpdp2)(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);

static void load_cfg_for(pid_t pid)
{
    char self[256] = { 0 }, line[512];
    cfg_pid = pid;
    memset(&ov, 0, sizeof(ov));
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (f) {
        fread(self, 1, sizeof(self) - 1, f);
        fclose(f);
    }
    f = fopen(CONF, "r");
    if (!f) {
        LOG("load_cfg: pid=%d cmdline=\"%s\" no conf at " CONF, pid, self);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        char *p1 = strchr(line, '|');
        if (!p1)
            continue;
        *p1++ = 0;
        char *p2 = strchr(p1, '|');
        if (!p2)
            continue;
        *p2++ = 0;
        char *p3 = strchr(p2, '|');
        if (!p3)
            continue;
        *p3++ = 0;
        p3[strcspn(p3, "\r\n")] = 0;
        if (strcmp(line, self) == 0) {
            ov.vendorID = strtoul(p1, NULL, 0);
            ov.deviceID = strtoul(p2, NULL, 0);
            snprintf(ov.name, sizeof(ov.name), "%s", p3);
            ov.active = true;
            break;
        }
    }
    fclose(f);
    LOG("load_cfg: pid=%d cmdline=\"%s\" active=%d name=\"%s\" vid=0x%x did=0x%x",
        pid, self, ov.active, ov.name, ov.vendorID, ov.deviceID);
}

static inline void ensure_cfg(void)
{
    pid_t pid = getpid();
    if (cfg_pid != pid)
        load_cfg_for(pid);
}

static void apply(VkPhysicalDeviceProperties *p)
{
    if (ov.vendorID)
        p->vendorID = ov.vendorID;
    if (ov.deviceID)
        p->deviceID = ov.deviceID;
    if (ov.name[0])
        snprintf(p->deviceName, sizeof(p->deviceName), "%s", ov.name);
}

/* Walk the pNext chain of VkPhysicalDeviceProperties2 and rewrite
 * VkPhysicalDeviceDriverProperties / VkPhysicalDeviceVulkan12Properties so that
 * driverID/driverName do not leak "MESA_VENUS" to engines (NetEase Messiah,
 * Unreal, …) that gate paths on it. */
static void apply_pnext(VkPhysicalDeviceProperties2 *p2)
{
    VkBaseOutStructure *s;
    for (s = (VkBaseOutStructure *)p2->pNext; s; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
            VkPhysicalDeviceDriverProperties *d = (VkPhysicalDeviceDriverProperties *)s;
            if (ov.vendorID == 0x5143 /* Qualcomm */) {
                d->driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
                snprintf(d->driverName, sizeof(d->driverName), "Qualcomm driver");
                snprintf(d->driverInfo, sizeof(d->driverInfo), "V@740.0 (Adreno)");
            } else if (ov.vendorID == 0x13B5 /* ARM */) {
                d->driverID = VK_DRIVER_ID_ARM_PROPRIETARY;
                snprintf(d->driverName, sizeof(d->driverName), "ARM driver");
                snprintf(d->driverInfo, sizeof(d->driverInfo), "0.0.0");
            }
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
            VkPhysicalDeviceVulkan12Properties *v12 = (VkPhysicalDeviceVulkan12Properties *)s;
            if (ov.vendorID == 0x5143) {
                v12->driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
                snprintf(v12->driverName, sizeof(v12->driverName), "Qualcomm driver");
                snprintf(v12->driverInfo, sizeof(v12->driverInfo), "V@740.0 (Adreno)");
            } else if (ov.vendorID == 0x13B5) {
                v12->driverID = VK_DRIVER_ID_ARM_PROPRIETARY;
                snprintf(v12->driverName, sizeof(v12->driverName), "ARM driver");
                snprintf(v12->driverInfo, sizeof(v12->driverInfo), "0.0.0");
            }
            break;
        }
        default:
            break;
        }
    }
}

static void VKAPI_PTR my_gpdp(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
    real_gpdp(pd, p);
    apply(p);
}

static void VKAPI_PTR my_gpdp2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    real_gpdp2(pd, p);
    apply(&p->properties);
    apply_pnext(p);
}

static void log_ext_list(const char *tag, uint32_t n, const char *const *names)
{
    LOG("%s: %u extension(s)", tag, n);
    for (uint32_t i = 0; i < n; i++)
        LOG("%s:   [%u] %s", tag, i, names[i] ? names[i] : "(null)");
}

static VkResult VKAPI_PTR my_create_instance(const VkInstanceCreateInfo *ci,
                                             const VkAllocationCallbacks *a, VkInstance *inst)
{
    ensure_cfg();
    if (ov.active) {
        uint32_t api = ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0;
        LOG("vkCreateInstance: pid=%d apiVersion=%u.%u.%u", getpid(),
            VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
            VK_API_VERSION_PATCH(api));
        log_ext_list("vkCreateInstance", ci->enabledExtensionCount,
                     ci->ppEnabledExtensionNames);
    }
    VkResult r = real_ci(ci, a, inst);
    if (r == VK_SUCCESS && ov.active) {
        real_gpdp = (void *)real_gipa(*inst, "vkGetPhysicalDeviceProperties");
        real_gpdp2 = (void *)real_gipa(*inst, "vkGetPhysicalDeviceProperties2");
        real_cd = (PFN_vkCreateDevice)real_gipa(*inst, "vkCreateDevice");
        real_gdpa = (PFN_vkGetDeviceProcAddr)real_gipa(*inst, "vkGetDeviceProcAddr");
    }
    return r;
}

static VkResult VKAPI_PTR my_create_device(VkPhysicalDevice pd,
                                           const VkDeviceCreateInfo *ci,
                                           const VkAllocationCallbacks *a, VkDevice *dev)
{
    if (ov.active) {
        LOG("vkCreateDevice: pid=%d phys=%p", getpid(), pd);
        log_ext_list("vkCreateDevice", ci->enabledExtensionCount,
                     ci->ppEnabledExtensionNames);
        /* Walk VkDeviceCreateInfo.pNext for feature structs and log their sType. */
        const VkBaseInStructure *p = (const VkBaseInStructure *)ci->pNext;
        for (int i = 0; p && i < 64; i++, p = p->pNext)
            LOG("vkCreateDevice: pNext[%d] sType=0x%x", i, p->sType);
    }
    return real_cd(pd, ci, a, dev);
}

static PFN_vkVoidFunction VKAPI_PTR my_gipa(VkInstance inst, const char *name)
{
    ensure_cfg();
    if (ov.active) {
        if (!strcmp(name, "vkGetPhysicalDeviceProperties"))
            return (PFN_vkVoidFunction)my_gpdp;
        if (!strcmp(name, "vkGetPhysicalDeviceProperties2") ||
            !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
            return (PFN_vkVoidFunction)my_gpdp2;
        if (!strcmp(name, "vkCreateDevice"))
            return (PFN_vkVoidFunction)my_create_device;
    }
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)my_create_instance;
    return real_gipa(inst, name);
}

static VkResult my_enum_ext(const char *layer, uint32_t *pc, VkExtensionProperties *p)
{
    return real_dev->EnumerateInstanceExtensionProperties(layer, pc, p);
}

static int wrap_close(struct hw_device_t *d)
{
    (void)d;
    return real_dev->common.close(&real_dev->common);
}

static hwvulkan_device_t wrap_dev = {
    .common = {
        .tag = HARDWARE_DEVICE_TAG,
        .version = HWVULKAN_DEVICE_API_VERSION_0_1,
        .close = wrap_close,
    },
    .EnumerateInstanceExtensionProperties = my_enum_ext,
    .CreateInstance = my_create_instance,
    .GetInstanceProcAddr = my_gipa,
};

static int wrap_open(const struct hw_module_t *m, const char *id, struct hw_device_t **d)
{
    ensure_cfg();
    LOG("wrap_open: id=%s", id ? id : "(null)");
    void *h = dlopen(REAL_SO, RTLD_NOW);
    if (!h) {
        LOG("wrap_open: dlopen(%s) failed: %s", REAL_SO, dlerror());
        return -1;
    }
    real_module = (hw_module_t *)dlsym(h, "HMI");
    if (!real_module || !real_module->methods || !real_module->methods->open) {
        LOG("wrap_open: no HMI in real driver");
        return -1;
    }
    hw_device_t *rd = NULL;
    int r = real_module->methods->open(real_module, id, &rd);
    if (r || !rd) {
        LOG("wrap_open: real open ret=%d rd=%p", r, rd);
        return r ? r : -1;
    }
    real_dev = (hwvulkan_device_t *)rd;
    real_gipa = real_dev->GetInstanceProcAddr;
    real_ci = (PFN_vkCreateInstance)real_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    wrap_dev.common.module = (hw_module_t *)m;
    *d = &wrap_dev.common;
    LOG("wrap_open: ok, pid=%d override active=%d", getpid(), ov.active);
    return 0;
}

static struct hw_module_methods_t module_methods = { .open = wrap_open };

__attribute__((visibility("default"))) hw_module_t HMI = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = HWVULKAN_HARDWARE_MODULE_ID,
    .name = "WWM Venus vkhook",
    .author = "waydroid-nvidia",
    .methods = &module_methods,
};
