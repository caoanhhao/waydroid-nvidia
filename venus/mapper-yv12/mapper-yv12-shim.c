/*
 * Waydroid gralloc4 mapper shim: give YV12 buffers real chroma plane layouts.
 *
 * minigbm_gbm_mesa allocates HAL YV12 (DRM_FORMAT_YVU420_ANDROID) as one GR88
 * block and records it as a single R8 plane (gbm_mesa_internals.cpp), so the
 * cros handle only has offsets/strides for plane 0.  The gralloc4 mapper still
 * reports three PlaneLayouts (Y, Cr, Cb) for the format, with the offset,
 * stride and size of planes 1 and 2 read from the empty handle slots: 0.
 * Every CPU writer that locks through gralloc4 (the external camera HAL,
 * Codec2 software decoders) then writes all chroma rows over the first luma
 * row, while the consumer (Mesa vk_android, patch 1007) reads chroma at the
 * standard YV12 offsets, where it stays 0: the picture is green.
 * See venus/notes-camera.md.
 *
 * This module is installed as
 * /vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa.so
 * and loads the vendor.img module from ...minigbm_gbm_mesa.real.so.  The real
 * mapper calls cros_gralloc_buffer::get_plane_{offset,stride,size} from
 * libminigbm_gralloc_gbm_mesa.so through its PLT; the shim points those GOT
 * entries at versions that return the Android YV12 layout (system/graphics.h)
 * for single-plane YVU420_ANDROID buffers, the same layout Mesa expects:
 *   chroma stride = align(y stride / 2, 16)
 *   Cr (plane 1) at y stride * height
 *   Cb (plane 2) at Cr + chroma stride * height / 2
 */
#define LOG_TAG "mapper-yv12-shim"
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Both ABIs are needed: the external camera provider is a 32-bit process. */
#ifdef __LP64__
#define LIB_DIR "/vendor/lib64/hw/"
typedef Elf64_Rela plt_reloc;
#define PLT_R_SYM ELF64_R_SYM
#define PLT_R_TYPE ELF64_R_TYPE
#define PLT_JUMP_SLOT R_X86_64_JUMP_SLOT
#else
#define LIB_DIR "/vendor/lib/hw/"
typedef Elf32_Rel plt_reloc;
#define PLT_R_SYM ELF32_R_SYM
#define PLT_R_TYPE ELF32_R_TYPE
#define PLT_JUMP_SLOT R_386_JMP_SLOT
#endif
#define REAL_MODULE LIB_DIR "android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa.real.so"

#define DRM_FOURCC(a, b, c, d)                                                \
   ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |           \
    ((uint32_t)(d) << 24))
/* minigbm's private fourcc for HAL_PIXEL_FORMAT_YV12 */
#define DRM_FORMAT_YVU420_ANDROID DRM_FOURCC('9', '9', '9', '7')

#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

/* const member functions of cros_gralloc_buffer: (this) or (this, plane) */
typedef uint32_t (*getter_fn)(const void *buffer);
typedef uint32_t (*plane_getter_fn)(const void *buffer, uint32_t plane);

static getter_fn get_format, get_height, get_num_planes;
static plane_getter_fn real_offset, real_stride, real_size;

static int
is_single_plane_yv12(const void *buffer, uint32_t plane)
{
   return plane > 0 && get_format(buffer) == DRM_FORMAT_YVU420_ANDROID &&
          get_num_planes(buffer) == 1;
}

static uint32_t
chroma_stride(const void *buffer)
{
   return ALIGN(real_stride(buffer, 0) / 2, 16);
}

static uint32_t
shim_offset(const void *buffer, uint32_t plane)
{
   if (!is_single_plane_yv12(buffer, plane))
      return real_offset(buffer, plane);
   uint32_t h = get_height(buffer);
   uint32_t cr = real_stride(buffer, 0) * h;
   return plane == 1 ? cr : cr + chroma_stride(buffer) * (h / 2);
}

static uint32_t
shim_stride(const void *buffer, uint32_t plane)
{
   if (!is_single_plane_yv12(buffer, plane))
      return real_stride(buffer, plane);
   return chroma_stride(buffer);
}

static uint32_t
shim_size(const void *buffer, uint32_t plane)
{
   if (!is_single_plane_yv12(buffer, plane))
      return real_size(buffer, plane);
   return chroma_stride(buffer) * (get_height(buffer) / 2);
}

static const struct {
   const char *name;
   void *shim;
   plane_getter_fn *real;
} hooks[] = {
   { "_ZNK19cros_gralloc_buffer16get_plane_offsetEj", (void *)shim_offset, &real_offset },
   { "_ZNK19cros_gralloc_buffer16get_plane_strideEj", (void *)shim_stride, &real_stride },
   { "_ZNK19cros_gralloc_buffer14get_plane_sizeEj", (void *)shim_size, &real_size },
};
#define NUM_HOOKS (sizeof(hooks) / sizeof(hooks[0]))

struct find_module {
   uintptr_t addr; /* an address inside the real module */
   struct dl_phdr_info info;
   int found;
};

static int
find_module_cb(struct dl_phdr_info *info, size_t size, void *data)
{
   struct find_module *f = data;
   (void)size;
   for (int i = 0; i < info->dlpi_phnum; i++) {
      const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
      uintptr_t start = info->dlpi_addr + ph->p_vaddr;
      if (ph->p_type == PT_LOAD && f->addr >= start && f->addr < start + ph->p_memsz) {
         f->info = *info;
         f->found = 1;
         return 1;
      }
   }
   return 0;
}

/* Point the real module's JUMP_SLOT entries for the hooked getters at the
 * shims.  The module is BIND_NOW, so the slots already hold the real
 * addresses; .got.plt is in RELRO, hence the mprotect. */
static int
patch_got(void *in_module)
{
   struct find_module f = { .addr = (uintptr_t)in_module };
   dl_iterate_phdr(find_module_cb, &f);
   if (!f.found) {
      LOGE("cannot find the real mapper's program headers");
      return -1;
   }

   uintptr_t base = f.info.dlpi_addr;
   const ElfW(Dyn) *dyn = NULL;
   for (int i = 0; i < f.info.dlpi_phnum; i++) {
      if (f.info.dlpi_phdr[i].p_type == PT_DYNAMIC)
         dyn = (const ElfW(Dyn) *)(base + f.info.dlpi_phdr[i].p_vaddr);
   }
   if (!dyn) {
      LOGE("real mapper has no PT_DYNAMIC");
      return -1;
   }

   /* bionic leaves d_ptr unrelocated */
   const ElfW(Sym) *symtab = NULL;
   const char *strtab = NULL;
   const plt_reloc *jmprel = NULL;
   size_t jmprel_size = 0;
   for (; dyn->d_tag != DT_NULL; dyn++) {
      switch (dyn->d_tag) {
      case DT_SYMTAB: symtab = (const ElfW(Sym) *)(base + dyn->d_un.d_ptr); break;
      case DT_STRTAB: strtab = (const char *)(base + dyn->d_un.d_ptr); break;
      case DT_JMPREL: jmprel = (const plt_reloc *)(base + dyn->d_un.d_ptr); break;
      case DT_PLTRELSZ: jmprel_size = dyn->d_un.d_val; break;
      }
   }
   if (!symtab || !strtab || !jmprel) {
      LOGE("real mapper: missing DT_SYMTAB/DT_STRTAB/DT_JMPREL");
      return -1;
   }

   long page = sysconf(_SC_PAGESIZE);
   size_t patched = 0;
   for (size_t r = 0; r < jmprel_size / sizeof(plt_reloc); r++) {
      if (PLT_R_TYPE(jmprel[r].r_info) != PLT_JUMP_SLOT)
         continue;
      const char *name = strtab + symtab[PLT_R_SYM(jmprel[r].r_info)].st_name;
      for (size_t h = 0; h < NUM_HOOKS; h++) {
         if (strcmp(name, hooks[h].name) != 0)
            continue;
         void **slot = (void **)(base + jmprel[r].r_offset);
         void *page_start = (void *)((uintptr_t)slot & ~(uintptr_t)(page - 1));
         if (mprotect(page_start, page, PROT_READ | PROT_WRITE) != 0) {
            LOGE("mprotect GOT: %s", strerror(errno));
            return -1;
         }
         *hooks[h].real = (plane_getter_fn)*slot;
         *slot = hooks[h].shim;
         mprotect(page_start, page, PROT_READ);
         patched++;
      }
   }
   if (patched != NUM_HOOKS) {
      LOGE("patched %zu of %zu GOT slots", patched, NUM_HOOKS);
      return -1;
   }
   return 0;
}

__attribute__((visibility("default"))) void *
HIDL_FETCH_IMapper(const char *name)
{
   static void *(*real_fetch)(const char *);
   static int hooked;

   if (!real_fetch) {
      void *real = dlopen(REAL_MODULE, RTLD_NOW | RTLD_LOCAL);
      if (!real) {
         LOGE("dlopen %s: %s", REAL_MODULE, dlerror());
         return NULL;
      }
      real_fetch = (void *(*)(const char *))dlsym(real, "HIDL_FETCH_IMapper");
      get_format = (getter_fn)dlsym(real, "_ZNK19cros_gralloc_buffer10get_formatEv");
      get_height = (getter_fn)dlsym(real, "_ZNK19cros_gralloc_buffer10get_heightEv");
      get_num_planes = (getter_fn)dlsym(real, "_ZNK19cros_gralloc_buffer14get_num_planesEv");
      if (!real_fetch) {
         LOGE("%s has no HIDL_FETCH_IMapper", REAL_MODULE);
         return NULL;
      }
      /* without the hooks the mapper behaves as before */
      if (get_format && get_height && get_num_planes)
         hooked = patch_got((void *)real_fetch) == 0;
      else
         LOGE("cros_gralloc_buffer getters not found");
      if (!hooked)
         LOGE("YV12 plane layouts not fixed");
   }
   return real_fetch(name);
}
