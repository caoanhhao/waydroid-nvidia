# Venus over vtest: dma-buf import

Status (2026-11-05): **works on the host.** A linear dma-buf allocated outside Venus can be
imported into Venus (RTX 4070) over vtest, and the GPU can read, fill and clear it.
Nothing is deployed to Waydroid yet.

## Trees and patches

| What | Where |
|------|-------|
| Mesa (client) | `src/mesa-dmabuf`: git worktree of `src/mesa`, branch `venus-dmabuf` = `venus-vtest` (0001) + 1 commit |
| virglrenderer (server) | `src/virglrenderer`: 1.3.0, branch `vtest-dmabuf` + 1 commit |
| Mesa patch | `patches/1001-mesa-venus-vtest-support-dma-buf-import-vtest-protocol-ve.patch` (applies on top of 0001) |
| Server patch | `patches/vrend-0001-vtest-add-VCMD_RESOURCE_IMPORT_BLOB-protocol-version-5.patch` |
| Server build | `build-virglrenderer-host.sh` (clones and applies the patch if needed, builds into `src/virglrenderer/build`, installs nothing) |
| Host test | `tests/vtest-dmabuf-import.c`, `tests/run-dmabuf-test.sh` |

The Mesa host build used for testing is `src/mesa-dmabuf/build-host` (Venus only, loaded via
`VK_ICD_FILENAMES=.../virtio_devenv_icd.x86_64.json`). To use the patch on Android, apply
1001 to `src/mesa` (on top of 0001) and rebuild with `build-venus-android.sh`.

## Protocol (vtest version 5)

New command `VCMD_RESOURCE_IMPORT_BLOB = 39`:

```
req  hdr { len = 4, id = 39 }
     [0] blob type  = VCMD_BLOB_TYPE_HOST3D (2)
     [1] fd type    = VCMD_BLOB_FD_TYPE_DMABUF (1)
     [2] size lo    \ size hint; 0 = let the server use lseek(fd, 0, SEEK_END)
     [3] size hi    /
     then the dma-buf fd: sendmsg() of 1 dummy byte + SCM_RIGHTS
     (same pattern as VCMD_DRM_SYNC_FD_TO_HANDLE)
rsp  hdr { len = 1, id = 39 }
     [0] res_id     (0 = import failed; the connection stays up)
```

Server (`vtest_resource_import_blob`): takes the dma-buf size from `lseek` (the client
size must not be larger), calls `virgl_renderer_resource_import_blob(HOST3D, DMABUF)`, then
`virgl_renderer_ctx_attach_resource`. With the render-server proxy, attach already
forwards dma-buf resources to `virgl_render_server` (`RENDER_CONTEXT_OP_IMPORT_RESOURCE`),
so the render server and vkr needed no changes. Unref goes through the existing
`VCMD_RESOURCE_UNREF`.

Client (`vn_renderer_vtest.c`):
- requests protocol 5. `has_dma_buf_import` and `bo_ops.create_from_dma_buf` are set only
  if the server agrees (`protocol_version >= 5`). Otherwise behavior is unchanged.
- `vtest_bo_create_from_dma_buf` sends the fd, keeps its own `dup` for `mmap` and
  `export_dma_buf`, and sets `SHAREABLE` plus `MAPPABLE` when the memory type is host-visible
  (`mmap_size` = the dma-buf size).
- Imports are **not deduped**. virtgpu dedups by gem handle. vtest has no handle to key on,
  so each import (including the throwaway one inside `vkGetMemoryFdPropertiesKHR`) creates a
  new server resource. This works, but costs one round trip and one host import per call.
- `vn_renderer_bo_create_from_dma_buf` now returns `VK_ERROR_INVALID_EXTERNAL_HANDLE` when
  the op is NULL (old server). Before, an app that imported anyway crashed on a NULL call.

Compatibility (tested):

| Client | Server | Result |
|--------|--------|--------|
| patched | patched | import works, `IMPORTABLE` bit set |
| patched | system 1.3.0 (v4) | negotiates v4, not importable, import fails with `VK_ERROR_INVALID_EXTERNAL_HANDLE` |
| system Mesa 26.2.6 | patched | negotiates v4, `vulkaninfo` normal |

Version number caveat: upstream vtest is still at v4 / command 38 on `main` (checked
2026-11-05). If upstream ever takes 39 or v5 for something else, renumber both patches.

## Test result

Run: `sh venus/tests/run-dmabuf-test.sh <alloc>`. This starts the patched server on
`$XDG_RUNTIME_DIR/vtest-dmabuf-test.sock` with `--rendernode /dev/dri/renderD129`
(`RENDER_SERVER_EXEC_PATH` points at the build dir) and runs the test with the patched ICD.

The test allocates a 256x256 ARGB8888 linear dma-buf, writes a pattern with the CPU,
imports it, then:
1. buffer: copies it into a host-visible buffer (GPU read), fills the first half with the
   GPU (GPU write), and checks both from the CPU;
2. image: `VK_EXT_image_drm_format_modifier` LINEAR `B8G8R8A8_UNORM` with an explicit
   `rowPitch`, `vkCmdClearColorImage`, a release barrier to `VK_QUEUE_FAMILY_FOREIGN_EXT`,
   and a CPU check of every pixel.

| Allocator | Result |
|-----------|--------|
| `heap:/dev/dma_heap/system` | **PASS** (3/3) |
| `gbm:/dev/dri/renderD128` (Intel i915 linear, modifier 0) | **PASS** (3/3) |
| `gbm:/dev/dri/renderD129` (NVIDIA gbm) | `gbm_bo_create(LINEAR \| RENDERING)` fails, not tested |

`vkGetMemoryFdPropertiesKHR` reports `memoryTypeBits 0x9` for these dma-bufs (NVIDIA
types 0 and 3, both without `HOST_VISIBLE`), so the imported memory is device-only from
Vulkan's point of view.

### Cache coherency: important for gralloc

Without extra CPU cache maintenance (`CLFLUSH=0`), the results are wrong, and the amount
varies from run to run:
- the GPU reads zeros (RAM) where the CPU wrote the pattern, because the writes are still
  in the CPU cache;
- the CPU reads its own stale cache lines instead of the GPU's fill or clear.

So NVIDIA accesses imported foreign dma-bufs **without snooping the CPU cache**, and
`DMA_BUF_IOCTL_SYNC` does nothing for these exporters on x86 (system heap; i915 on an LLC
part assumes coherency). With `clflush` + `mfence` over the mapping around each CPU
access (the test's `CLFLUSH=1`, now the default in the runner), every check passes.

This does not affect the GPU→GPU path (Venus renders, and NVIDIA or the compositor reads
through the same importer). It does affect any CPU access to gralloc buffers shared with
the 4070: CPU-rendered surfaces, `lock()`/`unlock()` in gralloc, screenshots, and software
composition in the hwcomposer. The gralloc `lock`/`unlock` path will need explicit
`clflush` (or write-combined/uncached CPU mappings). Alternatively, use an allocator whose
exporter does real cache maintenance. Input for `gralloc-findings.md`.

## What's left

1. **Android build and deploy:** apply 1001 to `src/mesa` and rebuild with NDK
   (`build-venus-android.sh`). Run the patched `virgl_test_server` from
   `src/virglrenderer/build` in place of the system one for the Waydroid socket. Set
   `RENDER_SERVER_EXEC_PATH` and `LD_LIBRARY_PATH` as in `tests/run-dmabuf-test.sh`. Only
   the vtest binary changed, but use the matching build-dir lib/render server anyway. The
   socket path stays `~/.local/share/waydroid/data/vtest/vtest.sock`. Do not replace
   `/usr/lib/virgl_render_server`.
2. **AHB path on Android:** `vn_android.c` imports gralloc buffers through
   `vn_device_memory_import_dma_buf` and needs `EXT_image_drm_format_modifier` and
   `EXT_queue_family_foreign` from the renderer (both exposed by NVIDIA via Venus). Not tested
   inside Android yet. It depends on gralloc giving a layout and modifier NVIDIA accepts
   (linear worked here with `rowPitch` 1024).
3. **Allocator choice:** the Intel `renderD128` minigbm buffers import fine (linear). The
   cache-coherency issue above and the gralloc layout research decide the final allocator.
4. **Optional:** dedup imports by dma-buf inode (`fstat` `st_ino`) on the client side to save a
   round trip and a host import per `vkGetMemoryFdPropertiesKHR`. Mesa's virtgpu path
   relies on dedup for re-imports with a larger size. vtest currently makes a new resource
   each time, which is correct but slower.
5. **Optional:** send the fd in the same `sendmsg` as the command (fewer syscalls).
   The current split matches the existing fd-carrying vtest commands.

# Stage B: window buffers allocated on the host (vtest protocol 6)

Status (2026-11-05): **works on the host.** Nothing is deployed to Waydroid yet. This
implements Stage B of `gralloc-findings.md`: window buffers are NVIDIA block-linear
allocations on the 4070, handed to Android as dma-bufs.
- Venus renders into them through device-local memory: no PCIe traffic and no cache
  coherency problem.
- Hyprland can display them zero-copy through its normal `GL_TEXTURE_2D` path.

## Pieces

| What | Where |
|------|-------|
| Server patch | `patches/vrend-0002-vtest-add-VCMD_ALLOC_DMABUF_IMAGE-protocol-version-6.patch` (on top of vrend-0001; `src/virglrenderer` branch `vtest-dmabuf`) |
| gralloc shim | `patches/minigbm-vtest-0001-gralloc-shim-allocating-window-buffers-through-vtest.patch` = `src/minigbm-vtest` (own git repo), NDK build `src/minigbm-vtest/build-android.sh` |
| Deploy (user, sudo) | `deploy-gralloc-vtest.sh install \| enable \| disable \| uninstall \| status` |
| Host test | `tests/vtest-hostalloc.c`, `tests/fake-inner-wrapper.c`, `tests/run-hostalloc-test.sh [w h]` |

No Mesa change is needed. Venus keeps asking for protocol 5, which a v6 server grants,
and imports these buffers with `VCMD_RESOURCE_IMPORT_BLOB`.

## Protocol: `VCMD_ALLOC_DMABUF_IMAGE = 40`

```
req  hdr { len = 5 + 2n, id = 40 }
     [0] width  [1] height  [2] DRM fourcc (single plane)
     [3] flags: RENDER=1 (color attachment) | TEXTURE=2 (sampled) | SCANOUT=4
     [4] n = number of allowed modifiers (0 = any), max 64
     [5 + 2i], [6 + 2i] modifier i lo/hi
rsp  hdr { len = 7, id = 40 }
     [0] error (0 or positive errno; the connection stays up)
     [1..2] modifier lo/hi  [3] row pitch  [4] offset  [5..6] dma-buf size
     then, only if error == 0, the dma-buf fd (SCM_RIGHTS)
```

How the server allocates (`vtest/vtest_vk_alloc.c`):
- It has its own `VkDevice` in the `virgl_test_server` process. `libvulkan` is dlopen'ed,
  and the GPU is picked by matching `--rendernode` through `VK_EXT_physical_device_drm`.
- The command is marked `init_context = false`, so a gralloc client never spawns a render
  server.
- Candidate modifiers are the device's single-plane modifiers that:
  - have the needed tiling features (COLOR_ATTACHMENT for RENDER, which excludes NVIDIA
    LINEAR);
  - are dma-buf exportable at that size;
  - are in the client's list.
- It creates the `VkImage` with `VkImageDrmFormatModifierListCreateInfoEXT`, allocates
  dedicated, exportable device-local memory, exports a dma-buf, and returns the chosen
  modifier and the `MEMORY_PLANE_0` layout.
- **Stateless:** the image and memory are freed right after export, and the dma-buf keeps
  the allocation alive. The test checks this (see below).
- Formats: AB24/XB24 → R8G8B8A8, AR24/XR24 → B8G8R8A8, RG16 → R5G6B5, AB30/XB30 →
  A2B10G10R10, AB4H/XB4H → R16G16B16A16_SFLOAT.

## gralloc shim (`libgbm_mesa_wrapper.so`)

minigbm_gbm_mesa dlopens `libgbm_mesa_wrapper.so` and calls `get_gbm_ops()`. The shim
replaces that library.
- `alloc` for buffers **without CPU usage** (`!force_linear && !needs_map_stride`) and a
  supported format goes to vtest.
  - The allowed modifiers are `waydroid.modifiers.<fourcc>.<n>`, which hwcomposer fills
    from Hyprland. Today that is `0x…60601x` + `0x…e0801x`. Vulkan only exports `0x…60601x`,
    so the intersection is block-linear uncompressed.
- Everything else goes to the **inner wrapper**, `/vendor/lib64/libgbm_mesa_wrapper_intel.so`
  (the vendor original, or Stage A's): CPU buffers, mapper `import`/`map`/`free`, any vtest
  error, and a non-zero plane offset.
  - minigbm's mapper only gbm-imports buffers with CPU usage, so it never touches NVIDIA
    buffers.
  - The inner wrapper is loaded by path because the renamed file still has the SONAME
    `libgbm_mesa_wrapper.so`.
- Props:
  - `waydroid.gralloc.vtest=1` enables it. The default is pass-through.
  - Socket: `waydroid.gralloc.vtest.socket`, else `mesa.vtest.socket.name`, else
    `/data/vtest/vtest.sock`.
  - The connection is persistent, with a mutex, and reconnects once on EPIPE.
- ABI: the shim's `struct alloc_args` matches the shipped vendor wrapper. I checked the
  disassembly of `/lib64/libgbm_mesa_wrapper.so` from `vendor.img`: `out_fd` 0x18,
  `out_stride` 0x1c, `out_modifier` 0x20, `needs_map_stride` 0x16.
- Logcat tag: `GBM-VTEST-SHIM`. It logs the first fallback with the reason.

## Host test result

`sh venus/tests/run-hostalloc-test.sh [w h]` starts the patched server on a private
socket (`$XDG_RUNTIME_DIR/vtest-hostalloc-test.sock`), sets Hyprland's AB24 modifier list
as env props, and:
1. allocates through the shim (host build) with a fake inner wrapper:
   - a scanout AB24 buffer must come from vtest with an NVIDIA modifier;
   - a CPU-usage buffer must go to the inner wrapper;
2. allocates 8 more buffers and keeps them open;
3. imports buffer 1 into **Venus** (explicit modifier + row pitch, dma-buf memory), clears
   it green, uploads a red 64x64 square, reads all pixels back through Venus, then
   releases it to `QUEUE_FAMILY_FOREIGN`;
4. imports the 8 extra buffers into Venus and clears them blue. If the server's free after
   export let allocations alias, buffer 1 would now be wrong;
5. imports buffer 1 into **NVIDIA EGL** as `GL_TEXTURE_2D`, as Hyprland does, and checks
   pixels through an FBO.

| size | modifier | stride | dma-buf | result |
|------|----------|--------|---------|--------|
| 1920x1080 | `0x0300000000606014` | 7680 | 10 MiB | **PASS** |
| 2560x1068 (FramebufferSurface size) | `…606014` | 10240 | 12 MiB | **PASS** |
| 1080x2400 | `…606014` | 4352 | 12 MiB | **PASS** |
| 1081x1919 | `…606014` | 4352 | 8 MiB | **PASS** |
| 3840x2160 | `…606014` | 15360 | 32 MiB | **PASS** |
| 100x100 | `…606014` | 448 | 64 KiB | **PASS** |

The Venus import memory type is device-local only (`memoryTypeBits 0x3`, type 0), so there
is no CPU mapping and no coherency issue. NVIDIA EGL: bind `glError 0`, FBO complete,
pixels correct.

## Deploy (user runs; I did not touch Waydroid or the live server)

1. Rebuild the server: `sh venus/build-virglrenderer-host.sh`. The build dir now contains
   v6. **Restart** `venus/run-vtest-server.sh` so the Waydroid socket speaks v6. Venus
   clients are unaffected: they still negotiate v5.
2. `sh src/minigbm-vtest/build-android.sh`, then `sh venus/deploy-gralloc-vtest.sh enable`.
   This:
   - installs the shim;
   - keeps the current wrapper as `libgbm_mesa_wrapper_intel.so` (Stage A's if it is in
     the overlay, else the original from `vendor.img`, extracted with `debugfs`);
   - sets `waydroid.gralloc.vtest = 1` and `debug.renderengine.backend = skiavk`;
   - restarts Waydroid and prints the shim's logcat and SurfaceFlinger's buffer modifiers.
3. Expect `dumpsys SurfaceFlinger` buffers with modifier `216172782120099860`
   (= `0x0300000000606014`) instead of Intel X-tiled.

Coexistence with Stage A (`deploy-gralloc-linear.sh`): that script writes and removes
`overlay/vendor/lib64/libgbm_mesa_wrapper.so`, which is the shim's path. Running it after
the shim replaces the shim, and `off` deletes it. Re-running `deploy-gralloc-vtest.sh
enable` (or `install`) moves whatever non-shim wrapper is there to
`libgbm_mesa_wrapper_intel.so` and reinstalls the shim. With both active, CPU buffers go
LINEAR through Stage A, and GPU buffers come from the host.

## Risks and what's left

1. **GLES apps on Intel cannot use these buffers.** iris cannot import NVIDIA block-linear
   vidmem. With the shim on, every GPU window buffer is NVIDIA's, so any app that renders
   with GLES (`ro.hardware.egl = mesa` → Intel) fails at `eglCreateWindowSurface`/dequeue.
   The same goes for SurfaceFlinger client composition on GLES, which is why `enable` sets
   `debug.renderengine.backend=skiavk`. Only Vulkan (HWUI `skiavk`, Venus) clients work
   until the GLES path is on Venus (ANGLE/zink). minigbm passes no client identity to the
   wrapper, so the shim cannot choose per app. Use `disable` to go back.
2. **Not yet tried inside Android:**
   - SELinux and permissions for the allocator service (`hal_graphics_allocator`) to
     connect to `/data/vtest/vtest.sock`. If denied, the shim logs it and falls back to
     Intel, so this is harmless but ineffective.
   - Venus's AHB/ANB import with the block-linear modifier from the cros handle.
   - hwcomposer → Hyprland with that modifier (linux-dmabuf v3 path).
3. **CPU access** to a GPU-only buffer later (`lock()` without CPU usage at allocation)
   has no mapping. minigbm would fail the map. Such buffers should be allocated with CPU
   usage and then take the Intel path.
4. The vtest server allocates synchronously in its event loop. Each allocation takes about
   1–2 ms, during which other clients' commands wait. This is fine for window buffers;
   move it to a thread if it shows up.
5. Formats outside the list above (YUV, R8 blobs) always take the inner wrapper.

## Stage B in Android: black screen and SurfaceFlinger crash loop (2026-11-05 12:30)

**Cause.**
- Android 13's SurfaceFlinger RenderEngine only has a GL backend: the binary contains just
  "RenderEngine with SkiaGL Backend". `debug.renderengine.backend=skiavk` is ignored
  (SkiaVk arrived in Android 14).
- With `ro.hardware.egl = mesa`, SurfaceFlinger and bootanimation render with **iris on
  Intel** into gralloc buffers that the shim now allocates in **NVIDIA vidmem**.
- iris aborts in `dri_create_fence_fd` (from `eglCreateSyncKHR` in
  `SkiaGLRenderEngine::drawLayersInternal` during `primeShaderCache`, and from
  bootanimation's `eglSwapBuffers`). SF restarts every ~17 s: tombstones 12–23 since 12:23,
  read from `~/.local/share/waydroid/data/tombstones` (group-readable on the host).
- The earlier SIGSEGV in `HWC2::impl::Display::present` (12:20, right after a display
  hotplug, with garbage pointer r12 = "tor@4.0:") looks like a separate stale-display bug.
  Recheck it after the fix.

**Fix.** `deploy-gralloc-vtest.sh enable` now sets `ro.hardware.egl = angle`:
- All GLES (SurfaceFlinger, bootanimation, GLES apps) goes ANGLE → Vulkan → Venus →
  NVIDIA. `/vendor/lib64/egl/libEGL_angle.so` ships in the vendor image.
- It drops `debug.renderengine.backend` and sets `ro.logd.size = 16M`. The main log rotated
  within about a minute before.
- `disable` and `uninstall` restore `ro.hardware.egl = mesa`.
- `deploy-gralloc-vtest.sh logs` collects props, SF GLES/buffer lines, tombstones and
  `logcat -b all`.

**Display path verified on the host** (`tests/run-wl-show-test.sh`, `tests/wl-show-hostalloc.c`):
- The test does what the hwcomposer does:
  - a server-allocated AB24 buffer (modifier `0x0300000000606014`), drawn by Venus with
    dynamic rendering + `vkCmdClearAttachments`;
  - attached to an `xdg_toplevel` via `zwp_linux_dmabuf_v1` **v3** `params.add(fd, 0,
    offset, stride, mod_hi, mod_lo)` + `create_immed`, as `create_dmabuf_wl_buffer`
    does.
- Result: `create_immed` succeeds and Hyprland shows the correct red, green, blue and white
  quadrants (screenshot checked).
- Hyprland advertises `0x…606010–15` and `0x…e08010–15` for AB24 at v3 (`tests/wl-dmabuf-mods.c`).
  This includes the `…606013`/`…606014` that SurfaceFlinger received for NotificationShade
  and NavigationBar.
- The hwcomposer source (`gralloc_handler.cpp` `create_buffer_cros`) passes the cros
  handle's `format`, `strides[0]`, `offsets[0]` and `format_modifier` through unchanged.
- So the modifier and the hwcomposer → Hyprland handoff are **not** the cause of the black
  window.
- The `eglCreateImageKHR … requested buffer attributes are not supported` errors in the
  Hyprland log (8609, lines 3116–23581 of 40866) have no timestamps. They likely date from
  the Intel X-tiled period.

The vtest server forks one child per client even with `--multi-clients`. So an
allocation does not block other clients, which corrects the risk noted above. The many
`virgl_test_server` children are live client connections.

## ANGLE on Venus: first frames, then the region-sampling abort (12:32–12:40)

- With `ro.hardware.egl = angle`, SurfaceFlinger reports `GLES: ANGLE (NVIDIA, Vulkan 1.3.351
  (NVIDIA Virtio-GPU Venus (NVIDIA GeForce RTX 4070)))`.
  - All SF and app buffers are AB24 `0x0300000000606014`.
  - The Waydroid window showed the **animated LineageOS boot animation**, rendered by
    bootanimation through ANGLE → Venus → NVIDIA and composited by Hyprland (screenshots
    taken).
- **Remaining SurfaceFlinger abort** (`Unable to generate SkSurface`, preceded by `Failed to
  create a valid texture … [144,63] isWriteable:1` and `skia: Could not create EGL image,
  err = 0x300c`):
  - The buffer is a CPU-readable render target (usage CPU_READ + GPU render), likely region
    sampling.
  - The shim correctly sends it to the Intel/Stage A LINEAR path (`stride 640, map_stride
    640`).
  - ANGLE uses `VK_IMAGE_TILING_LINEAR` for every AHB with CPU usage
    (`HardwareBufferImageSiblingVkAndroid`). Venus's `vn_get_ahb_image_props` rejects
    anything but OPTIMAL (`VK_ERROR_FORMAT_NOT_SUPPORTED`), so `eglCreateImageKHR` fails.
  - Reproduced inside Android with `tests/android-ahb-test.c` (pushed to `/data/local/tmp`):
    - GPU-only AHB (NVIDIA): ANGLE image + FBO + clear readback OK.
    - CPU-read AHB: Venus import OK with OPTIMAL, `-11` with LINEAR; ANGLE `0x300c`.
- **Fix:** patch `1002-mesa-venus-android-accept-linear-tiling-in-AHB-image-format-queries.patch`.
  - Venus always recreates AHB images with `DRM_FORMAT_MODIFIER_EXT` and gralloc's layout,
    so LINEAR is safe to accept.
  - Verified inside Android without installing: `tests/android-ahb-linear-probe.c` dlopens
    the HAL from `/data/local/tmp`. Installed driver: `-11`. Patched build
    (`src/mesa-dmabuf/build-android`, = a5da660 + 1002): the whole import chain passes.
- **Knock-on effect (no separate fix needed):**
  - WM sent `wm_boot_animation_done` to the SF instance that had just crashed (12:32:38.228).
  - init restarted SF and bootanimation, but system_server did not restart. So the new
    bootanimation never got the exit signal and covered the UI, and SF had no app layers.
  - `setprop service.bootanim.exit 1` ends the animation, but the clean fix is a boot
    without the SF crash.
- **Still open:** `RenderEngine: failed to dup EGL native fence sync: 0x3000` on every frame.
  ANGLE gets `-1` from Venus's sync_file export (patch 0001's CPU-wait shortcut), so
  RenderEngine falls back to waiting. This costs performance only; a real vtest sync_file
  export would fix it.

## Correction: the real cause of ANGLE's `eglCreateImageKHR` failure (patch 1003, 13:00)

Patch 1002 did not fix SurfaceFlinger (tombstones 10/11 after it was installed). This
Android 13 ANGLE (`external/angle` android13-release, 2.1.20440) **always uses OPTIMAL
tiling** for AHBs (`AhbDescUsageToVkImageTiling`). LINEAR is not involved, so 1002 is
harmless but unneeded here.

The real cause is in `HardwareBufferImageSiblingVkAndroid::initImpl`:
- It imports the AHB memory requesting `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`.
- `MemoryProperties::findCompatibleMemoryIndex`'s only external-memory fallback is
  DEVICE_LOCAL again.
- Intel-allocated dma-bufs (system memory: Stage A LINEAR and any shim fallback) get
  `memoryTypeBits 0x9` from Venus/NVIDIA: type 0 (no flags) and type 3 (host-visible
  cached). Neither is device-local.
- So ANGLE fails, and `eglCreateImageKHR` returns an error.
- NVIDIA buffers report 0x3, which includes device-local type 1, so they work.

This one cause explains:
- SurfaceFlinger `Unable to generate SkSurface` (144x63 CPU-readable region-sampling
  target);
- `Failed to create a valid texture [25,32]` (the software-drawn pointer surface, CPU
  usage);
- the hwcomposer SIGSEGV: ANGLE's error path in `egl::Display::createImage` →
  `egl::Image::onDestroy` calls a null function, on its EGL snapshot/readback path;
- the SF abort that followed, a HIDL DEAD_OBJECT.

**Fix:** `1003-mesa-venus-android-advertise-device-local-types-for-foreign-AHB-memory.patch`.
- `vn_GetAndroidHardwareBufferPropertiesANDROID` wraps the common implementation. If the
  dma-buf's bits contain no device-local type, it also reports the pure device-local types
  (not host-visible ones).
- `vn_android_device_import_ahb` already swaps an incompatible memory type index for a
  compatible one, so the import still uses type 0.
- Built into `src/mesa-dmabuf/build-android` (= a5da660 + 1002 + 1003). Not yet verified
  inside Android: adb lost IPv4. ARP for 192.168.240.112 is INCOMPLETE and adbd does not
  answer on IPv6 link-local.

The shim's `Operation not supported` at 12:43:31 was a bogus 2147483647x2147483647 request,
which the Intel path also rejected. On the host the server allocates 1x1, 25x32, 64x64 and
2560x1068 in AB24/AR24/XB24/XR24, with and without Hyprland's list.

## Second ANGLE blocker for linear buffers: INPUT_ATTACHMENT (patch 1004, 13:30)

After 1003, SurfaceFlinger still aborted (tombstones 14–16, 13:22–13:24).
- Android 13 ANGLE's `AhbDescUsageToVkImageUsage` adds `INPUT_ATTACHMENT` to every sampled
  AHB.
- On the host, NVIDIA rejects `INPUT_ATTACHMENT` for any `DRM_FORMAT_MOD_LINEAR` dma-buf
  image: `vkCreateImage` and the modifier format query both return -11. `COLOR_ATTACHMENT`,
  `MUTABLE_FORMAT` + an sRGB list, `SAMPLED` and `TRANSFER` are accepted.
- Venus creates AHB images with gralloc's real modifier (LINEAR for CPU-usage buffers).
  So the deferred creation fails, and ANGLE reports it as `EGL_BAD_PARAMETER`, the same
  code as the memory-type failure.

**Fix:** `1004-mesa-venus-android-drop-input-attachment-usage-for-linear-AHB-images.patch`.
For LINEAR AHB images, `vn_android_ahb_image_init` drops `INPUT_ATTACHMENT`; it is only
used for framebuffer fetch.

Both fixes are needed for Intel/LINEAR buffers in ANGLE: 1003 (memory type) and 1004
(usage). NVIDIA block-linear buffers need neither.

`venus/try-venus-build.sh` does the whole check without adb:
1. pre-checks a driver build inside Android with `tests/android-ahb-linear-probe`, which
   now mimics ANGLE: OPTIMAL tiling, its usage, and a DEVICE_LOCAL memory type;
2. installs the driver only if the pre-check passes;
3. restarts Waydroid;
4. reports tombstones since the restart, the real EGL test and log lines
   (`venus/try-venus-build.txt`, `crash-log-stageB5.txt`).

## Patch 1004 works; the remaining crash is the hwcomposer's cursor path (13:40)

With 1004 installed (`try-venus-build.txt`, 13:34), the real ANGLE test passes for both GPU-only
and CPU-readable buffers. SurfaceFlinger no longer aborts on its own.

The remaining crash, 13:34:42, about 2 s after `sf_stop_bootanim`:
- composer@2.1-service segfaults in ANGLE `eglCreateImageKHR`; the error path in
  `egl::Image::onDestroy` calls a null function.
- SurfaceFlinger then aborts on the composer's DEAD_OBJECT.
- bootanimation stays up, because system_server did not restart.

Cause: for CROS gralloc with linux-dmabuf, the Waydroid hwcomposer only uses EGL for shm
buffers. In full-UI mode the only source of those is the cursor:
- `display->supports_cursor_hw_buffer = property_get_bool("persist.waydroid.cursor_force_shm",
  false)`. The property name is inverted: false means shm.
- So every pointer image goes through `egl_render_to_pixels`: `GraphicBuffer(WRAP_HANDLE,
  USAGE_HW_TEXTURE)` → `eglCreateImageKHR` → `glReadPixels`.
- The same buffer kinds import fine through ANGLE in a normal process: tested inside Android
  with 25x32 and 2560x1068 buffers, formats RGBA/RGBX/RGB565/BGRA, usages 0x100/0xb00/0x133.
  Why the composer's wrapped import fails is still open.

**Workaround:** `deploy-gralloc-vtest.sh enable` now sets `persist.waydroid.cursor_force_shm =
true`, so cursor images go to Hyprland as dma-bufs and the composer never uses EGL in full-UI
mode. `disable` and `uninstall` remove it.

Expected side effect: the cursor sprite is a CPU-drawn Intel LINEAR buffer, which Hyprland on
NVIDIA cannot show as `GL_TEXTURE_2D` (gralloc-findings §5). The pointer over the Waydroid
window may be invisible; this is cosmetic. Fixing it means drawing the cursor into a
host-allocated buffer or fixing the composer's EGL path.

Single-window/multi-window modes also use EGL for inactive-window snapshots, so the same crash
can happen there. Full-UI mode does not.

## Result: Android UI on the RTX 4070 (14:02)

After `deploy-gralloc-vtest.sh enable`, which adds `persist.waydroid.cursor_force_shm = true`,
and with the vtest server running again (it had stopped after 13:44; without it SF aborts with
`no suitable EGLConfig found`):
- `sys.boot_completed=1`, bootanimation exited, no tombstones after 14:01:46. The composer is
  still its first instance.
- The Waydroid window shows the **LineageOS lock screen**: clock, date, notification, status
  bar. Screenshot: `venus/screenshots/stageB-lockscreen-1402.jpg`.
- systemui: `Pipeline=Skia (Vulkan)`, 503 frames. SurfaceFlinger: `GLES: ANGLE (NVIDIA,
  Vulkan 1.3.351 (NVIDIA Virtio-GPU Venus (NVIDIA GeForce RTX 4070)))`.
- Buffers are host-allocated NVIDIA block-linear: FramebufferSurface and NotificationShade
  use `0x0300000000606014`, StatusBar uses `0x0300000000606012`.
- `nvidia-smi`: the patched `virgl_test_server` plus three `virgl_render_server` workers are
  on the GPU.

Pieces in place: patches 0001, 0002, 1001, 1002, 1003, 1004 (Mesa); vrend-0001, vrend-0002
(server); minigbm-vtest-0001 (shim). Stage A wrapper as inner; `ro.hardware.egl = angle`;
`debug.hwui.renderer = skiavk`; `persist.waydroid.cursor_force_shm = true`.

`deploy-gralloc-vtest.sh` fix: `set_prop` now deletes every copy of a key before writing it.
A run had left `waydroid.gralloc.vtest` twice in `waydroid.cfg`, and Waydroid refuses to
parse duplicates.

Open:
- the pointer may be invisible over the window (Intel LINEAR cursor sprite);
- the composer EGL import failure (only avoided);
- `failed to dup EGL native fence sync` (CPU waits, from patch 0001);
- performance not measured;
- GLES apps not tested beyond SF;
- `persist.waydroid.suspend` is still `false` from testing.
