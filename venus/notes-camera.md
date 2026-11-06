# Camera preview is green (YV12 chroma planes at offset 0)

Status 2026-11-06: **fixed and verified.**
- `yv12-planes-test` PASS on the device for both ABIs (it failed before: U/V at offset 0,
  rowStride 0): V at 2211840, U at 2764800, chroma rowStride 1024 for 1920x1080.
- Aperture's preview (1920x1080 YV12) shows correct colours, checked by eye. No new
  tombstones; SurfaceFlinger, cameraserver and the camera provider kept running.
- The first install had only the 64-bit shim, and the preview stayed green: the camera
  provider is a **32-bit** process and loads `/vendor/lib/hw/`. Both ABIs are needed.
- Separate bug, not fixed: taking a photo fails. Locking any BLOB buffer fails in
  `gbm_map` (`createJpegLocked: could not lock 3145728 bytes`); the BLOB is allocated as a
  4096xN R8 texture but imported and mapped as total_size x 1.

## Symptom

- The camera app (Aperture) shows the USB webcam (UGREEN Camera 4K, `/dev/video0`, MJPEG)
  with correct shapes and brightness, but the whole picture is tinted bright green.
- Green is what YUV → RGB gives when both chroma planes are 0 (U = V = 0).

## Path

- Camera HAL: `android.hardware.camera.provider@2.7-external-service` (`ro.hardware.camera =
  v4l2`). It decodes MJPEG on the CPU and writes the frames into gralloc buffers it locks with
  `lockYCbCr`.
- The preview stream buffers are 1920x1080 HAL YV12 (`0x32315659`).
  - gralloc logs `Allocated: 1920x1080, stride: 2048, map_stride: 2048`.
  - minigbm_gbm_mesa allocates YV12 (`DRM_FORMAT_YVU420_ANDROID`) as one GR88 block, width/2
    aligned to 512, height × 1.5, and records it as a single R8 plane
    (`gbm_mesa_internals.cpp`). The cros handle only fills `offsets/strides/sizes[0]`.
- The consumer reads the buffer through Venus. Mesa patch 1007 expands a single-plane YV12
  buffer to the standard Android layout (`system/graphics.h`): V at stride × height, U after
  it, chroma stride = align(stride/2, 16). See `notes-tiktok.md`.

## Cause

- The gralloc4 mapper (`android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa.so`) still
  reports three PlaneLayouts (Y, Cr, Cb) for `DRM_FORMAT_YVU420_ANDROID`.
- It fills each plane's offset, stride and size from `cros_gralloc_buffer::get_plane_offset`,
  `get_plane_stride` and `get_plane_size`, which read the handle slots. For planes 1 and 2
  those slots are 0.
- So every CPU writer that locks through gralloc4 (the external camera HAL, Codec2 software
  decoders) gets Cr and Cb pointers at offset 0 with row stride 0. All chroma rows land on top
  of the first luma row.
- Mesa reads chroma where YV12 keeps it. Nothing was ever written there, so it stays 0: green.

Proof: `venus/mapper-yv12/yv12-planes-test` allocates a 1920x1080 YV12 AHardwareBuffer with
CPU write usage and calls `AHardwareBuffer_lockPlanes`. With the vendor mapper:

```
plane0 (Y) offset 0 pixelStride 1 rowStride 2048
plane1 (U) offset 0 pixelStride 1 rowStride 0
plane2 (V) offset 0 pixelStride 1 rowStride 0
expected V at 2211840, U at 2764800, chroma rowStride 1024: FAIL
```

## Fix

**gralloc4 mapper shim** (`venus/mapper-yv12/mapper-yv12-shim.c`, built by
`venus/mapper-yv12/build.sh`).

- Installed as `android.hardware.graphics.mapper@4.0-impl.minigbm_gbm_mesa.so` in both
  `/vendor/lib64/hw/` and `/vendor/lib/hw/` (x86_64 and x86 builds; the camera provider is
  32-bit). It `dlopen`s the vendor.img mapper, which `install.sh` extracts with debugfs as
  `...minigbm_gbm_mesa.real.so`, and forwards `HIDL_FETCH_IMapper`.
- The real mapper calls the three getters from `libminigbm_gralloc_gbm_mesa.so` through its PLT
  (only `JUMP_SLOT` relocations, RELA on x86_64 and REL on x86, `BIND_NOW`). The shim finds those slots in the real
  module's `DT_JMPREL`, `mprotect`s the RELRO page and points them at its own versions.
- For a buffer with format `YVU420_ANDROID`, `num_planes == 1` and plane > 0 they return the
  layout Mesa 1007 expects:
  - chroma stride = align(Y stride / 2, 16);
  - Cr (plane 1) at Y stride × height;
  - Cb (plane 2) at Cr + chroma stride × height / 2;
  - size = chroma stride × height / 2.
- Every other buffer and plane goes to the real getters. The handle, the allocator and the
  Vulkan path are unchanged.
- If the real mapper or its symbols are not found, the shim logs to `mapper-yv12-shim` and the
  mapper behaves as before.
- The fixed layout fits in the allocation: Y stride × (height + height/2) bytes.

Release integration: `release/build.sh` copies the shims into `dist/android/` (64-bit) and
`dist/android/lib/` (32-bit), `install.sh` extracts both `.real.so` and installs the shims,
`uninstall.sh` removes them.

## Tools

- `venus/mapper-yv12/build/{lib64,lib}/yv12-planes-test`: push to `/data/local/tmp` and run
  with adb; run both, since each ABI loads its own mapper. Exit 0 and
  `PASS` when the chroma planes are at the YV12 offsets.
- The overlay is applied when the container starts: after replacing the mapper, restart with
  `waydroid session stop` and `sudo systemctl restart waydroid-container`.
- Which mapper a process loads: `sudo waydroid shell -- grep mapper /proc/<pid>/maps`.
- With the screen locked, `am start -a android.media.action.STILL_IMAGE_CAMERA_SECURE` opens
  Aperture over the keyguard.
