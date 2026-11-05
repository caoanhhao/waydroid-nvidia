# Gralloc buffer layout for Venus → NVIDIA (findings, 2026-11-05)

Question: how should Waydroid's gralloc allocate window buffers so that (a) host NVIDIA can
import them through Venus/vtest and (b) Hyprland on the 4070 can still display them?

Test programs (C, host-side, read-only) and raw output are in the shared notes folder:
`~/.config/Claude/side-session-notes/local_4688a598-0079-49ec-9f07-b55b3907d5d5/gralloc-tests/`
(`xdev.c` is the cross-device render test, `egllin.c` is the Hyprland-path check).

## TL;DR

1. **Today minigbm_gbm_mesa allocates Intel-tiled buffers.** Window and framebuffer buffers
   are X-tiled (`0x0100000000000001`) and non-scanout buffers are Y-tiled (`0x...02`). NVIDIA
   cannot use either: its `vkCreateImage` **silently accepts** the Intel modifier and renders
   it as linear, so the image comes out scrambled with no error.
2. **NVIDIA Vulkan can import LINEAR and render into it**, but only under three conditions,
   and Intel-allocated buffers break all three today:
   - **Pitch:** the row pitch must be a multiple of **128 bytes**. Otherwise NVIDIA silently
     uses its own pitch: the image shears and NVIDIA writes past the end of the dma-buf.
     Intel GBM aligns LINEAR to only 64 B (e.g. 1080 px → 4320 B).
   - **Cache coherency:** NVIDIA accesses imported i915/system-memory dma-bufs **without
     snooping**. Dirty CPU cache lines (at least the kernel's zeroing of freshly allocated
     pages) get written back over NVIDIA's output, so **even GPU-only paths are corrupted on
     a fresh buffer** (NVIDIA re-reading its own write saw holes). Flushing the buffer's CPU
     cache lines (clflush) once before NVIDIA writes makes it 100% correct.
   - **Spec:** LINEAR lacks `COLOR_ATTACHMENT` in NVIDIA's modifier tiling features. It
     works in practice, but it is out of spec.
3. **LINEAR does not work for display.** Hyprland 0.57 does not advertise LINEAR, because
   NVIDIA EGL marks it external-only. It binds every client dma-buf to `GL_TEXTURE_2D`, and
   NVIDIA rejects that for LINEAR (`GL_INVALID_OPERATION`). The texture still counts as
   "ok", so the window would be black. Hyprland can display only NVIDIA block-linear
   modifiers, and only NVIDIA can allocate those.
4. **No single modifier fits Venus, Hyprland and Intel without a patch somewhere.**
   - LINEAR fits Venus and Intel but not Hyprland.
   - NVIDIA block-linear fits Venus and Hyprland but not Intel.
5. **Recommendation:** stage the work.
   - **Now (to unblock Venus):** use LINEAR with a 128-byte-aligned pitch and a CPU cache
     flush at allocation, via a small patch to `libgbm_mesa_wrapper.so`. Display stays broken
     until a Hyprland fix lands or hwcomposer copies frames.
   - **Final:** host-allocated NVIDIA block-linear buffers through vtest (plan.md phase 3), so
     Venus and Hyprland both get zero-copy, and Intel never touches window buffers.

## 1. What minigbm_gbm_mesa allocates today (Intel renderD128)

Stack: `allocator@4.0-service.minigbm_gbm_mesa` → `libminigbm_gralloc_gbm_mesa.so` →
`libgbm_mesa_wrapper.so` → Mesa `libgbm_mesa.so`/iris on `gralloc.gbm.device`. Mesa Venus reads
buffer info with the **gralloc0 CrOS API** (`I MESA: Using gralloc0 CrOS API` in logcat), so
`vk_android_get_anb_layout()` passes minigbm's real modifier to the host.

Source code (waydroid/android_external_minigbm `lineage-18.1`, `gbm_mesa_driver/`):

- `gbm_mesa_internals.cpp`: `force_linear = (use_flags & BO_USE_SW_MASK) != 0`, so only
  buffers with CPU read/write usage are LINEAR. `use_scanout = BO_USE_SCANOUT`, which comes
  from the COMPOSER/overlay usage that SurfaceFlinger adds to every layer.
- `gbm_mesa_wrapper.cpp` `gbm_mesa_alloc()`: reads a modifier list from
  **`waydroid.modifiers.<fourcc-hex>.<n>`** props and calls `gbm_bo_create_with_modifiers2()`
  with it. If that fails it falls back to `gbm_bo_create(usage)`. If `force_linear` is set,
  it uses `{LINEAR}` instead.
- hwcomposer (`wayland-hwc.cpp` `dmabuf_modifiers()`) **sets those props** from Hyprland's
  `zwp_linux_dmabuf_v1` v3 `modifier` events. Live values: 12 NVIDIA block-linear modifiers
  per format (`0x300000000606010..15`, `0x300000000e08010..15`). LINEAR is not among them.
- iris cannot allocate NVIDIA modifiers (tested: `create_with_modifiers2` fails), so every
  allocation falls back to plain `gbm_bo_create`:

| GBM usage (iris, tested on host) | modifier |
|---|---|
| SCANOUT / SCANOUT\|RENDERING | `0x0100000000000001` I915 X_TILED |
| RENDERING, or texture-only | `0x0100000000000002` I915 Y_TILED |
| LINEAR (any SW usage) | `0x0` LINEAR, pitch aligned to 64 B |

Live, from `dumpsys SurfaceFlinger`: the FramebufferSurface buffers are 2560x1068, usage
`0x1b00` (HW_TEXTURE|HW_RENDER|HW_COMPOSER|HW_FB), fourcc AB24, modifier
`72057594037927937` = `0x0100000000000001` (X-tiled). I could not dump app buffers because
HWUI-on-Vulkan apps are crash-looping. Their usage would be GPU_RENDER_TARGET (from
`vk_common_GetSwapchainGrallocUsage2ANDROID`) plus SurfaceFlinger's COMPOSER bit, so they
take the SCANOUT path and get X-tiled too.

## 2. NVIDIA modifiers (Vulkan, nvidia-open 615.78.08)

From `vkGetPhysicalDeviceFormatProperties2` + `VkDrmFormatModifierPropertiesListEXT`; full dump
in `gralloc-tests/vkmods.txt`. R8G8B8A8_UNORM, B8G8R8A8_UNORM, R8G8B8A8_SRGB, R5G6B5,
A2B10G10R10 and RGBA16F all have the same list:

| modifier | meaning | tiling features | dma-buf import/export (COLOR_ATTACHMENT\|SAMPLED\|TRANSFER) |
|---|---|---|---|
| `0x0300000000606010`–`…15` | NVIDIA block-linear 2D, kind 0x06, GOB h 1–32, uncompressed | `0x1dd83` (incl. COLOR_ATTACHMENT) | import + export |
| `0x0000000000000000` | LINEAR | `0x1dd03`: **no COLOR_ATTACHMENT** (has blend, storage, blit/transfer dst) | `vkGetPhysicalDeviceImageFormatProperties2` still says supported |

- NV12 offers the same block-linear set plus LINEAR, as 2-plane and sample-only.
- Vulkan does **not** list the compressed `0x…e0801x` modifiers that NVIDIA EGL and Hyprland
  advertise. If a host allocator is built later, it must pick from `0x…60601x`, which both
  Vulkan and Hyprland accept.
- For comparison, Intel Vulkan offers LINEAR, X (`…01`), Y (`…02`), Y_CCS (`…06`) and
  Y_RC_CCS_CC (`…08`). The only modifier the two GPUs share is **LINEAR**.

### Cross-device test results (`xdev.c`)

Intel GBM allocates; NVIDIA Vulkan imports with an explicit modifier and draws a two-colour
pattern (dynamic-rendering clear + `vkCmdClearAttachments`); I check every pixel.

| case | result |
|---|---|
| Intel LINEAR, pitch % 128 == 0, CPU cache flushed first | **correct**, both with direct rendering into the import and with render-to-vidmem + `vkCmdCopyImage` |
| Intel LINEAR, no flush | **40–95% of pixels lost** (zeros), random per run. CPU reads after clflush and NVIDIA's own read-back are also wrong |
| Intel LINEAR, 1080 px (pitch 4320) | NVIDIA reports pitch 4352 and memory size 2088960 > dma-buf 2073600: **sheared, and writes past the end of the buffer** |
| Intel X-tiled `…01` | `vkCreateImage` **succeeds**, image is scrambled (treated as linear) |
| Intel GPU writes LINEAR → NVIDIA reads | correct (3/3 runs) |
| control: Intel Vulkan renders into its own buffer | correct |

Consequences for the vtest dma-buf importer (sibling session):

- **Validate on the server.** Accept only modifiers in the host device's list (LINEAR or
  `0x03…`). After `vkCreateImage`, check that `vkGetImageSubresourceLayout().rowPitch`
  equals the requested pitch and that memory requirements ≤ the dma-buf size (`lseek`).
  Otherwise return an error: NVIDIA will not fail on its own.
- **Coherency.** One clflush pass over a fresh Intel buffer, or any allocator that leaves no
  dirty CPU lines (for example writing it through a WC mapping), is needed before NVIDIA
  writes. Later CPU access (SW usage) needs a flush again. "GPU-only is fine" holds only once
  the zeroed pages' dirty lines are gone.

## 3. Is LINEAR enough? Can minigbm be forced to LINEAR?

**For Venus import: yes**, if the pitch is 128-aligned and the cache is flushed. Direct
rendering works. Rendering over PCIe into system memory is slower than rendering to vidmem and
blitting (see §4). The missing COLOR_ATTACHMENT bit means Venus's AHB format query
(`vkGetAndroidHardwareBufferPropertiesANDROID`) reports LINEAR AHBs as not renderable.
Swapchain/ANB images go through `vn_android_image_from_anb` and do not check that bit.

**For display: no** (see §5).

How to force it:

- **By property only, without code: not reliably.**
  - `waydroid.modifiers.<fmt>.0 = 0` would make iris allocate LINEAR. But hwcomposer rewrites
    `.0…11` with Hyprland's list when it connects, and the wrapper caches the list per
    process on first allocation.
  - There is no `force_linear` property. The only built-in trigger is CPU usage bits, which
    apps don't set.
  - Even LINEAR from props would still have the 64-byte pitch, which NVIDIA corrupts.
- **Small patch (recommended):** rebuild only `libgbm_mesa_wrapper.so` (≈257-line C++,
  depends on liblog/libcutils/libgbm_mesa/libc++) with the NDK and ship it through the
  overlay at `/vendor/lib64/libgbm_mesa_wrapper.so`. In `gbm_mesa_alloc()`:
  1. If `property_get_bool("waydroid.gralloc.force_linear")`, set `modifiers = {LINEAR}`.
  2. Allocate with the width rounded up so `stride % 128 == 0` (e.g. `ALIGN(width, 32)` for
     4 bpp). Verify the stride that comes back, and keep the real width in the handle
     metadata. This is the same trick `gbm_mesa_internals.cpp` already uses for YV12.
  3. After `gbm_bo_create`, mmap the fd, clflush it once and munmap (x86 only).

  Rebuilding the whole minigbm or allocator service is not needed.

## 4. Alternatives: renderD129 and copy costs

- **renderD129 as the gralloc device: not possible inside Android.**
  - Waydroid's `tools/helpers/gpu.py` has `unsupported = ["nvidia"]` and never bind-mounts an
    nvidia node (`config_nodes` only has renderD128).
  - Android's Mesa GBM has no nvidia-drm backend: host Mesa EGL logs "pci id 10de:2786,
    driver (null) … failed to create dri2 screen".
  - NVIDIA's real GBM backend (`libnvidia-allocator`) is glibc-only.
- **NVIDIA GBM on the host**, for a future host-side allocator (`nvgbm.c`):
  - RENDERING gives `0x300000000e08014` (compressed block-linear). Intel cannot import it,
    and it is not in the Vulkan list.
  - LINEAR is allowed only *without* RENDERING. Intel GBM can import it, and the dma-buf can
    be mmapped.
  - For host allocation, use **Vulkan** (exportable image, `0x…60601x`) rather than GBM: it
    is the modifier set that both Vulkan and Hyprland accept.
- **Copy costs.** Measured on the 4070, Gen4 x16 link, 2560x1440 RGBA (14.7 MB), 200 reps:

| operation | ms/frame | throughput |
|---|---|---|
| NVIDIA vidmem → imported Intel LINEAR (`vkCmdCopyImage`) | 0.57 | 26 GB/s |
| clear-render directly into imported LINEAR (write-only) | 0.96 | 15 GB/s |
| same, into NVIDIA block-linear vidmem | 0.038 | — |

  A copy per frame costs about 0.6 ms per 1440p window: about 0.9 GB/s at 60 fps, 2.1 GB/s
  at 144 fps. That is acceptable. Rendering directly into system memory is worse once
  blending makes PCIe reads, so **render to vidmem, then blit once** if buffers must live in
  Intel memory.

## 5. Display path: hwcomposer → Hyprland

- `hwcomposer.waydroid` (`get_gralloc_type`: `minigbm*` → `GRALLOC_CROS`) sends each layer as
  `zwp_linux_buffer_params_v1.add(fd, stride, offset, modifier)` + `create_immed`. The
  modifier is the one minigbm stored in the cros handle.
- hwcomposer binds linux-dmabuf **v3**, so Hyprland skips its format/modifier check, which
  only applies to v≥4 clients.
- `create_failed` calls `abort()`, but the params object is destroyed right after
  `create_immed`, so failures go unnoticed.
- The `wl_shm` + `glReadPixels` (CPU copy) fallback is used only for non-dmabuf gralloc types.
- What Hyprland 0.57 accepts on NVIDIA:
  - Its renderer format list comes from NVIDIA EGL `eglQueryDmaBufModifiersEXT`, which
    returns `0x…606010–15`, `0x…e08010–15`, and LINEAR flagged external-only
    (`eglmods.c`). Hyprland drops external-only entries (`OpenGL.cpp`: "if the driver doesn't
    mark linear as external, add it… (e.g. nvidia)"), so it advertises no LINEAR. This
    matches the live `waydroid.modifiers.*` props exactly.
  - Import: `CDMABuffer` → `eglCreateImage`; on failure it retries with MOD_INVALID. Then
    `CGLTexture` always uses `GL_TEXTURE_2D` (`GLTexture.cpp`: "#TODO external only formats
    should be external as well").
- Tested with `egllin.c` on NVIDIA EGL:
  - `eglCreateImage(LINEAR)` succeeds.
  - `glEGLImageTargetTexture2DOES(GL_TEXTURE_2D)` fails with **GL_INVALID_OPERATION**, and
    the FBO is incomplete.
  - With `GL_TEXTURE_EXTERNAL_OES` it works.
  - `CGLTexture::ok()` only checks `m_texID > 0`, so Hyprland would accept the buffer and
    show nothing.
- Intel X-tiled: NVIDIA EGL rejects it. The Hyprland log has ~8.6k `eglCreateImageKHR …
  EGL_BAD_PARAMETER: requested buffer attributes are not supported`, which fits Waydroid
  sending Intel-tiled buffers, but I could not tie the lines to Waydroid: no Waydroid window
  was open and the log has no timestamps.

So the only modifiers Hyprland on NVIDIA can show zero-copy are NVIDIA block-linear.
LINEAR becomes displayable only with a Hyprland patch (bind external-only modifiers as
`GL_TEXTURE_EXTERNAL_OES`, which needs an external-sampler shader; `SH_FRAG_EXT` already
exists). Even with that patch, NVIDIA reads every frame across PCIe from system memory.

## Recommendation

**Stage A: unblock Venus now (assume LINEAR, as the dma-buf session does).**

1. Patch and overlay `libgbm_mesa_wrapper.so`: LINEAR behind a property, stride aligned to
   128 B, one clflush pass after allocation (§3).
2. In the vtest importer, reject foreign modifiers and pitch mismatches (§2), so a wrong
   layout fails loudly instead of corrupting memory.
3. Expect HWUI→Venus rendering to work but **nothing visible in Hyprland**. To see frames
   anyway, either:
   - patch Hyprland's `CGLTexture` (external-only LINEAR → `GL_TEXTURE_EXTERNAL_OES`); or
   - have hwcomposer use its shm/`glReadPixels` path (CPU copy; force it by building with
     `GrallocType::GRALLOC_DEFAULT` logic, or a hwcomposer patch).

   Both are debug crutches.

**Stage B: target design (plan.md phase 3).** Window buffers are allocated **on the host by
NVIDIA**:

- The vtest/virgl server creates exportable images with modifier `0x030000000060601x`, within
  the intersection of Vulkan and Hyprland lists, and passes the dma-buf fds back over
  SCM_RIGHTS.
- A minigbm "vtest" backend wraps them in cros handles carrying the real modifier.

Results:

- Venus imports device-local memory: coherent, no PCIe traffic.
- hwcomposer forwards the NVIDIA modifier, which Hyprland already advertises, so display is
  zero-copy with no Hyprland patch.
- Intel must then never touch these buffers:
  - Set `debug.renderengine.backend=skiavk` (or keep HWC device composition) so SurfaceFlinger
    does not GPU-compose on iris.
  - CPU-usage buffers (SW_READ/WRITE, camera, video) can stay LINEAR on Intel.
  - Intel GLES stays as the fallback only for apps that don't use Vulkan, until the GLES path
    (ANGLE/zink on Venus) lands.

## Stage A built: `gralloc.gbm.linear` (2026-11-05)

What was built:

- **Source:** `src/minigbm-wrapper/minigbm/` holds upstream `gbm_mesa_wrapper.cpp`, `.h` and
  `drv.h` (waydroid/android_external_minigbm `lineage-18.1` @ `e5a7769a4892`, the same code as
  the shipped vendor lib). It has a local git repo: commit 1 is upstream, commit 2 is the patch.
- **Patch:** [`patches/gralloc-0001-gbm_mesa-force-linear-for-nvidia.patch`](patches/gralloc-0001-gbm_mesa-force-linear-for-nvidia.patch).
  When `gralloc.gbm.linear` is true (read on every allocation), `gbm_mesa_alloc()`:
  - forces `GBM_BO_USE_LINEAR`, or the `{LINEAR}` modifier list when `waydroid.modifiers.*`
    props exist;
  - pads the allocation width so `stride % 128 == 0`. The buffer metadata keeps the real
    width, and minigbm sizes the planes from the returned stride;
  - mmaps the new dma-buf once, clflushes every 64-byte line and munmaps it, wrapped in
    `DMA_BUF_IOCTL_SYNC`;
  - logs `GBM-MESA-WRAPPER` errors if the result is not LINEAR or not 128-aligned.

  When the property is unset or 0, behaviour is unchanged.
- **Build:** `src/minigbm-wrapper/build.sh` (NDK r30, `x86_64-linux-android33-clang++`).
  - NDK shims stand in for `cutils/properties.h` (`__system_property_get`) and `log/log.h`
    (`__android_log_print`) under `src/minigbm-wrapper/include/`. The gbm/drm headers are
    copied there from the host.
  - Links the vendor image's own `libgbm_mesa.so` plus NDK liblog. libc++ is linked
    statically, and a version script exports only `get_gbm_ops` and `gbm_ops`, as the
    original does.
  - All 126 imported symbols resolve against the vendor/system libs of the image.
  - Output: `src/minigbm-wrapper/out/libgbm_mesa_wrapper.so`.
- **Host check:** the same source, built for glibc with an env-var property shim and run
  on host iris. With linear on, AB24, RG16, R8 and AB4H at widths 1080, 1366, 333, 2560
  and 17 all come out modifier 0 with stride % 128 == 0. With it off, the result is the stock
  X-tiled (`…01`). The cross-device test in §2 already showed NVIDIA renders correctly into
  such buffers after the flush.
- **Not yet tested inside Android.** The allocator service has not loaded it.

Deploy (the user runs this; it needs sudo and restarts Waydroid):

```
sh ~/projects/waydroid-nvidia/venus/deploy-gralloc-linear.sh
```

The script installs the lib to `/var/lib/waydroid/overlay/vendor/lib64/libgbm_mesa_wrapper.so`
(overlay, `mount_overlays = True`), adds `gralloc.gbm.linear = 1` under `[properties]` in
`waydroid.cfg`, and runs `waydroid upgrade --offline` and a restart. It then prints the prop,
SurfaceFlinger's gralloc buffers (expect `fourcc/mod:…/0` and stride a multiple of 128) and any
wrapper errors.

Expected side effects:

- All buffers go LINEAR, so Intel GLES rendering gets somewhat slower.
- Hyprland can't show LINEAR on NVIDIA (§5), so **expect the Waydroid window to stay black**
  until Stage B or a Hyprland/hwcomposer workaround. Use it to test Venus rendering
  (`dumpsys gfxinfo`, logcat), not the display.

Revert:

```
sh ~/projects/waydroid-nvidia/venus/deploy-gralloc-linear.sh off
```

This removes the property and the overlay file, then upgrades and restarts. A faster runtime
revert, without removing the lib, is to delete the `gralloc.gbm.linear = 1` line from
`/var/lib/waydroid/waydroid.cfg`, then run `sudo waydroid upgrade --offline` and restart. With
the property unset, the patched lib behaves like the stock one.
