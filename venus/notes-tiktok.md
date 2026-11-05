# TikTok takes down the composer and SurfaceFlinger (YV12 video layers)

Status 2026-11-05 17:05: **fixed and verified.**
- The shim and Mesa 1007 are installed: `hwcomposer.waydroid.so` and `vulkan.virtio.so` in the
  overlay are byte-identical to `release/dist`.
- TikTok (`com.ss.android.ugc.trill`) was cold-started after a session restart:
  - the YV12 layer was redirected to SurfaceFlinger (shim report, 17:01:44);
  - the composer, SurfaceFlinger and TikTok kept running, with no new tombstones after
    tombstone_39 (16:43:49, before the Mesa fix);
  - the For You feed shows video frames in the Waydroid window (hypruse capture, 17:03).
- Not checked: smooth playback over time, and frame rate.

## Symptom

Opening TikTok at 16:18–16:19 and 16:28–16:29 (tombstones in
`~/.local/share/waydroid/data/tombstones`):
- `composer@2.1-service` SIGABRT in `hwcomposer.waydroid.so hwc_wayland_thread+410`. The
  hwcomposer records why its Wayland connection died in
  `/data/waydroid_hwc_wayland_error.txt`: `socket error: Invalid argument`, at exactly those
  times.
- Then SurfaceFlinger aborts with HIDL `DEAD_OBJECT` in `HidlComposer::setLayerHandleInfo`, a
  consequence of the composer dying.
- Hyprland logs a pair of `eglCreateImageKHR … EGL_BAD_MATCH: requested LINUX_DRM_FORMAT is not
  supported` for each crash.

The crash comes about 0.6 s after `SplashActivity` starts, while TikTok's splash/feed video
plays. The splash video doesn't appear on every launch, so a launch without it does not
reproduce the crash.

## Cause 1: hwcomposer sends Hyprland a YV12 buffer

- The layer is 720x1280 HAL YV12 (`droid_format 0x32315659`), usage 0x933. It is the output of
  a software Codec2 decoder.
  - gralloc gives it to the Intel minigbm path (CPU usage, LINEAR).
  - The cros handle's DRM format is minigbm's private `DRM_FORMAT_YVU420_ANDROID` (fourcc
    `9997`, 0x37393939), modifier 0.
  - Recorded by the shim below: `compositor can't import 720x1280 format 9997 …`.
- `persist.waydroid.use_subsurface = true`, so hwcomposer.waydroid runs in a compositing mode.
  It turns every layer into `HWC_OVERLAY` and sends each cros buffer to Hyprland as
  `zwp_linux_buffer_params_v1.create_immed`, with the handle's format/modifier unchanged.
  - Source: `hardware/waydroid` lineage-20, `gralloc_handler.cpp` `create_buffer_cros`.
  - Offsets checked in the shipped binary: format at handle+0x5c, modifier at +0x64.
- Hyprland/NVIDIA cannot import fourcc `9997`, so the buffer fails.
  - With `create_immed`, the next use of that `wl_buffer` id makes libwayland fail with
    EINVAL.
  - The dispatch loop ends, and `hwc_wayland_thread` calls `abort()`.

## Cause 2: SurfaceFlinger can't import YV12 either

The first fix (shim alone) moved the crash: SurfaceFlinger aborted in `RenderEngine`
`AutoBackendTexture::makeImage`, with `Unable to generate SkImage. isTextureValid:0` (tombstones
37 and 39).

The reproducer `venus/hwc-yuv/android-yv12-import-test.c` (`test-hwc-shim.sh yv12test`) narrowed
it down:
- `vkGetAndroidHardwareBufferPropertiesANDROID` is fine: external format
  `G8_B8_R8_3PLANE_420_UNORM`, SAMPLED, BT.601 narrow.
- `vkCreateImage` with the external format succeeds (Venus defers the real creation).
- `vkAllocateMemory` (AHB import) returns `VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT`.
  ANGLE's `HardwareBufferImageSiblingVkAndroid::initImpl` turns every failure into
  `EGL_BAD_PARAMETER`.
- The cros handle says **1 plane**, stride 1024, size 1966080.
  - minigbm gbm_mesa allocates YV12 as one GR88 block: width/2 aligned to 512, height × 1.5.
    It sets `num_planes` from R8 (`gbm_mesa_internals.cpp`).
  - Mesa passes `drmFormatModifierPlaneCount = 1` for a 3-plane format, and NVIDIA rejects it.
- NVIDIA itself supports the format. Host check: `G8_B8_R8_3PLANE_420_UNORM` with modifier
  LINEAR, dma-buf import, SAMPLED is supported, and `vkCreateImage` with the 3-plane Android
  YV12 layout succeeds.

## Fix

1. **Mesa patch 1007** (`venus/patches/1007-mesa-vulkan-android-expand-single-plane-YV12.patch`,
   `src/mesa/src/vulkan/runtime/vk_android.c`, uncommitted on venus-vtest).
   - When gralloc reports `DRM_FORMAT_YVU420` with one plane, expand it to the Android YV12
     layout (`system/graphics.h`):
     - Y stride = buffer stride;
     - chroma stride = align(stride/2, 16);
     - V at stride × height;
     - U at V + chroma stride × height/2.
   - The height comes from the AHB description or the ANB create info.
   - The existing U/V swap for G8_B8_R8 then applies.
   - Verified: the reproducer imports YV12 and samples a red frame back as `254 0 0 255`
     (PASS).
2. **hwcomposer format shim** (`venus/hwc-yuv/hwcomposer-format-shim.c`, built by
   `venus/hwc-yuv/build.sh`).
   - Installed as `/vendor/lib64/hw/hwcomposer.waydroid.so`. It `dlopen`s the vendor.img
     module, which `install.sh` extracts with debugfs as `hwcomposer.waydroid.real.so`.
   - It wraps the HWC1 device's `prepare`/`set`. A cros layer (not the cursor, not the client
     target) whose (format, modifier) is not in `waydroid.modifiers.<fourcc>.*` gets
     `HWC_FRAMEBUFFER` + `HWC_SKIP_LAYER`. That is the same thing HWC2On1Adapter does for
     client layers.
     - hwcomposer.waydroid's compositing modes then let SurfaceFlinger compose everything
       between the first and last skipped layer into the client target, which is NVIDIA
       block-linear.
   - The vendor binary is unchanged, and the cursor-egl composer wrapper is untouched.
   - `persist.waydroid.hwc_unsupported`:
     - `client` (default);
     - `drop`: the layer is not shown; set clears its handle, which hwcomposer skips;
     - `off`: vendor behaviour.

     Change it with `waydroid prop set`, no sudo needed.
   - It logs each kind of layer once to logcat (`hwc-format-shim`) and to
     `/data/waydroid_hwc_format_shim.txt` (readable on the host, no adb needed).
   - This also covers CPU-written RGBA layers (Intel LINEAR, modifier 0), which Hyprland on
     NVIDIA accepts but cannot show (gralloc-findings §5). The cursor is excluded.

Release integration: `venus/patches/release-0004-hwcomposer-format-shim.patch`, applied on top
of release-0002. It changes `build.sh` (copies the shim), `install.sh` (extracts `.real.so` and
installs the shim), `uninstall.sh` and README. `release/dist` was rebuilt at 17:05 with 1007.

## Not caused by the GPU work: TikTok SIGSEGV in libhoudini

- Tombstones 25, 28, 35, 36 and 38: TikTok's `tp-fixed-*` worker threads hit a null dereference
  inside `/system/lib64/libhoudini.so`, which does the ARM → x86 translation.
- No graphics library is on the stack.
- It also happens when no composer or SurfaceFlinger crash occurs, and none occurred after the
  fix (16:48–17:05).
- This is a houdini / TikTok-native issue, separate from Venus. It was not compared with a
  non-Venus setup.

## Tools

- `venus/hwc-yuv/test-hwc-shim.sh` (run as root):
  - `logs`: logcat/props/dumpsys into `out/`;
  - `watch`: stream logcat to `out/logcat-live.txt` (logd buffers are small);
  - `apply`/`revert`: the shim;
  - `venus`/`venus-revert`: src/mesa Venus, with the old library kept in `out/`;
  - `yv12test`: the reproducer, plus an RGBA control.
- Android's network (adb) kept dying after composer crashes in this session, so everything goes
  through `waydroid shell` and files under `/data`.
