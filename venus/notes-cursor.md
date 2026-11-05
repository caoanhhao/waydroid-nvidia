# Mouse cursor over the Waydroid window, and the hwcomposer EGL crash

Status 2026-11-05 15:41: **fixed with the composer wrapper, verified with the pointer over the
window.**
- `test-cursor-fix.sh apply` ran at 15:38 (`venus/cursor/test-cursor-fix-apply.txt`):
  - the composer (pid 95) runs with `LD_PRELOAD=libnativewindow.so`;
  - the wrapper (4688 B) and `.real` (88016 B) are in `/vendor/bin/hw`;
  - `cursor_force_shm` is false.
- `waydroid show-full-ui`, unlocked, pointer moved over the window:
  - a hypruse window capture shows the Android arrow at the pointer position (Hyprland only
    overlays a cursor the client has set);
  - the composer is still pid 95 after the cursor path ran;
  - no composer or SurfaceFlinger coredumps after 15:30:09, and no new tombstones from them.
  - the user confirmed the pointer is visible (15:42).
- Unrelated crash seen at 15:39: com.android.vending RenderThread SIGFPE in
  `android::BlobCache::clean()` (HWUI Vulkan pipeline cache store, `GrVkResourceProvider::storePipelineCacheData`).
  It's not in the composer or the cursor path.

Earlier history:
- Root cause confirmed inside Android with the reproducer (results at the end).
- The 15:25 "fixed" was wrong. The window sat on a hidden workspace with no pointer over it, so
  the shm cursor path never ran.
  - When the user unlocked and moved the pointer over the window, the composer crashed again
    (host coredumps 15:26–15:27, `pc 0`) and SurfaceFlinger aborted.
  - The composer's environment in its core dump has **no LD_PRELOAD**, and its auxv says
    `AT_SECURE 0`. So init never applied the `.rc` service override; the linker did not drop the
    variable.
  - Why init ignored the override is not known (no init log, adb was down). The last `override`
    parsed wins, so Waydroid's `init.waydroid.rc` may be parsed after `/vendor/etc/init`.
- New fix: a wrapper binary over the composer's path (below), which doesn't depend on init.
  Install it with `venus/cursor/test-cursor-fix.sh apply`, then verify with the pointer over a
  **visible** window.
- `release/` is back to the workaround (release-0001 was reverse-applied, 15:3x).
  `release-0002` adds the wrapper to it.

## Cause

The bug is in ANGLE, and only processes that don't link libnativewindow hit it. Venus, the
shim and the handle are not involved.

- Android 13 ANGLE (`external/angle` android13-release, `renderer/vulkan/android/AHBFunctions.cpp`)
  resolves `AHardwareBuffer_acquire/describe/release` with `dlsym(dlopen(nullptr), ...)`.
  - On bionic, a `dlopen(nullptr)` handle only searches the executable and its DT_NEEDED tree.
  - That tree includes LD_PRELOAD libraries, but not libraries loaded later with dlopen.
- `/vendor/bin/hw/android.hardware.graphics.composer@2.1-service` doesn't have libnativewindow in
  that tree.
  - The full tree, from vendor.img/system.img: composer@2.1, composer@2.1-resources, mapper
    2/3/4, graphics.common, libbase, libbinder, libcutils, libfmq, libhardware, libhidlbase,
    libhwc2on1adapter, libhwc2onfbadapter, liblog, libsync, libutils, libvndksupport, libc++.
  - libui and libnativewindow only arrive later, through `dlopen("hwcomposer.waydroid.so")`.
- So `AHBFunctions::valid()` is false in the composer.
  - `HardwareBufferImageSiblingVkAndroid::initImpl` fails right away with
    `ANGLE_VK_CHECK(functions.valid(), VK_ERROR_INITIALIZATION_FAILED)`, which surfaces as
    EGL_BAD_PARAMETER.
  - The error path (`Image::onDestroy` → `ExternalImageSibling::onDestroy` →
    `HardwareBufferImageSiblingVkAndroid::onDestroy`) calls `functions.release(...)`, which is
    NULL. That gives `pc 0`, called from `egl::Image::onDestroy+97` through tail calls: tombstone
    18 in `crash-log-stageB6.txt`.
- Every `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` in the composer crashes this way, whatever
  the buffer.
  - SurfaceFlinger links libgui/libui/libnativewindow, so it works.
  - The `venus/tests` binaries link libandroid, so they work.
  - This is why `android-ahb-test` passed with the same buffer kinds.
- Ruled out:
  - WRAP_HANDLE: the handle the composer wraps is the mapper-imported one, and Venus reads the
    layout from the cros handle.
  - 32/64-bit: the composer is 64-bit.
  - Venus rejecting a handle with no metadata: ANGLE fails before it calls Vulkan.
  - The EGL config or context.

The composer reaches this path:
- with `persist.waydroid.cursor_force_shm=false` (the default; the name is inverted): every
  cursor image goes through `egl_render_to_pixels`
  (`hardware/waydroid/hwcomposer/egl-tools.cpp`);
- in single-window and multi-window modes, for inactive-window snapshots.

## Fix

Run the composer with `LD_PRELOAD=libnativewindow.so`. Preloaded libraries count as children of
the executable, so ANGLE's lookup succeeds. libnativewindow is LL-NDK, so a vendor process may
load it.

- `venus/cursor/composer-preload-wrapper.c`, built to `venus/cursor/build/composer-preload-wrapper`
  (NDK r30, API 33; needs only libc/libdl). It sets `LD_PRELOAD` (prepending to any existing value)
  and `execv`s `/vendor/bin/hw/android.hardware.graphics.composer@2.1-service.real` with the
  original argv, so argv[0] stays the service path.
- Overlay layout:
  - `/var/lib/waydroid/overlay/vendor/bin/hw/android.hardware.graphics.composer@2.1-service`: the
    wrapper;
  - `...-service.real`: the composer extracted from the user's vendor.img with debugfs.
- init and `init.waydroid.rc` are untouched: the service still runs the same path with the same
  user, groups and capabilities.
- The core showed `AT_SECURE 0` for this service, so bionic honours LD_PRELOAD across the exec.
- Then delete `persist.waydroid.cursor_force_shm`, so it defaults to false. The cursor then goes
  to Hyprland as **wl_shm**: the composer reads it back with `glReadPixels`, and Hyprland copies
  shm cursors on the CPU. A dma-buf cursor (`=true`) is a CPU-drawn Intel LINEAR buffer, which
  Hyprland on NVIDIA can't texture (gralloc-findings §5).
- No Mesa, shim or virglrenderer change is needed.
- Release integration: `venus/patches/release-0002-composer-wrapper-instead-of-rc.patch`, against
  the current `release/` (release-0001 reverted; `patch --dry-run -p1` passes).
  - `build.sh` copies the wrapper into `dist/android/`.
  - `install.sh` extracts `.real` from vendor.img, installs the wrapper and drops `cursor_force_shm`.
  - `uninstall.sh` removes both files.
  - README wording is updated.
  - Apply only after it is verified.

Dropped: `venus/cursor/zz-waydroid-hwcomposer-nativewindow.rc`, an init `service ... override`
with `setenv`. init didn't apply it; kept for reference only.

## Test

`venus/tests/android-wrap-handle-test.c`, binary `venus/tests/build/android-wrap-handle-test`:
- It mimics `egl_init` + `egl_render_to_pixels`:
  - a pbuffer RGBA8888 config, an ES2 context and a pbuffer surface on a separate thread;
  - `GraphicBuffer(WRAP_HANDLE, w/h/droid_format/pixel_stride from the cros handle,
    USAGE_HW_TEXTURE)` through libui's C++ symbols;
  - `eglCreateImageKHR` with `EGL_IMAGE_PRESERVED_KHR`, then `glReadPixels(GL_BGRA_EXT)`.
- Like the composer, it links only libc/libdl and dlopens EGL/GLES/libnativewindow RTLD_LOCAL. It
  prints what ANGLE's lookup sees: `AHardwareBuffer_acquire in the global group`.
- Expected:
  - without LD_PRELOAD: the address is `0x0` and the test crashes (SIGSEGV, pc 0), for both
    `SRC=wrap` and `SRC=ahb`;
  - with `LD_PRELOAD=libnativewindow.so`: `image ok`, `pixel[0] 0xffff0000` (opaque red read back
    as BGRA), PASS.
- `sh venus/cursor/test-cursor-fix.sh test` installs it to `/data/local/tmp` and runs all four
  combinations. It needs a running session and doesn't restart anything.

Results, 14:49, via adb, Waydroid running (`venus/cursor/test-cursor-fix.txt`):

| LD_PRELOAD | SRC | ANGLE's lookup | result |
|---|---|---|---|
| none | wrap | `0x0` | SIGSEGV, `pc 0` ← `egl::Image::onDestroy+97` ← `egl::Display::createImage+531` (identical to tombstone 18) |
| none | ahb | `0x0` | same SIGSEGV |
| libnativewindow.so | wrap | resolved | `image ok`, `pixel[0] 0xffff0000`, PASS |
| libnativewindow.so | ahb | resolved | PASS |

The cursor-like buffer is an Intel LINEAR buffer: drm AB24, mod 0, 25x32, pixel_stride 64,
droid_format 1, usage 0xb33. WRAP_HANDLE with usage 0x100 makes no difference.

Test bug fixed on the way: `eglDebugMessageControlKHR` called before `eglInitialize` crashes in
libEGL's `__glExtFwd0`, so the test now calls it after init.
