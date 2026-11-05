# Mouse cursor over the Waydroid window, and the hwcomposer EGL crash

Status 2026-11-05 ~14:40. The root cause was found by static analysis of the installed images and
the Android 13 ANGLE source. Not yet run inside Android: Waydroid was stopped during this work.
Run `venus/cursor/test-cursor-fix.sh test`, then `apply`.

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

Add `setenv LD_PRELOAD libnativewindow.so` to the composer service. Preloaded libraries count as
children of the executable, so ANGLE's lookup succeeds. libnativewindow is LL-NDK, so a vendor
process may load it.

- File: `venus/cursor/zz-waydroid-hwcomposer-nativewindow.rc`, installed through the overlay as
  `/var/lib/waydroid/overlay/vendor/etc/init/zz-waydroid-hwcomposer-nativewindow.rc`.
  - It is a full `service vendor.hwcomposer-2-1 ... override` block. Every line except `setenv`
    is copied from `/system/etc/init/init.waydroid.rc`.
  - init parses /system/etc/init before /vendor/etc/init and sorts files in each directory, so
    this file wins. The stock vendor rc is a non-override duplicate and is ignored.
- Then set `persist.waydroid.cursor_force_shm` back to false (delete the key). The cursor then
  goes to Hyprland as **wl_shm**: the composer reads it back with `glReadPixels`, and Hyprland
  copies shm cursors on the CPU. A dma-buf cursor (`=true`) is a CPU-drawn Intel LINEAR buffer,
  which Hyprland on NVIDIA can't texture (gralloc-findings §5).
- No Mesa, shim or virglrenderer change is needed.
- Release integration: `venus/patches/release-0001-composer-preload-libnativewindow-for-cursor.patch`
  (diff of `release/build.sh`, `install.sh`, `uninstall.sh`; not applied). `build.sh` copies the
  .rc into `dist/android/`, `install.sh` installs it and drops `cursor_force_shm`, and
  `uninstall.sh` removes it.

Possible caveat: bionic ignores LD_PRELOAD for AT_SECURE processes. The service runs as `user
host` with `capabilities SYS_NICE`. init grants that as ambient capabilities, which shouldn't set
AT_SECURE. `test-cursor-fix.sh check` prints the composer's environment, and the absence of
tombstones confirms it.

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

Results: (pending)
