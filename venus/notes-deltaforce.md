# Delta Force (Garena, `com.garena.game.df`) crash: no ASTC on the RTX 4070

## Symptom

The game crashed about 13 s after launch, while loading the Login map. The
tombstone shows a `SIGSEGV` in `libhoudini.so` on `RenderThread`. The ARM
backtrace that houdini prints to logcat shows the fault inside `libUE4.so`
with a null register.

## What it is not

- ARM translation: libhoudini (`ro.dalvik.vm.native.bridge=libhoudini.so`)
  runs the arm64-only game fine up to the crash.
- Anti-cheat, networking, ANGLE: the GLES contexts (GCloud/INTL device probing)
  work. The game renders through UE4 4.24 VulkanRHI directly on Venus.

## Cause

The UE4 log (`/sdcard/Android/data/com.garena.game.df/files/UE4Game/DeltaForce/DeltaForce/Saved/Logs/DeltaForce.log`)
is obfuscated: XOR every byte with `0x5c`. Decoded, it shows the cause:

```
LogVulkanRHI: Error: EPixelFormat(50) is not supported with Vk format 157
... (every ASTC format 157-183 and ETC2 147-151)
LogTexture: Error: Texture2D /Engine/EngineResources/WhiteSquareTexture... is unknown [raw type 0] which is not supported.
```

The APK's assets (`split_obbassets.apk` → `assets/main.obb.png`, a zip) contain only
`*-Android_ASTCClient.pak`. The NVIDIA desktop Vulkan driver reports
`textureCompressionETC2 = false` and `textureCompressionASTC_LDR = false`, so
every texture is dropped. The RHI then crashes on the null resources.

## Fix: ASTC LDR emulation in Venus (Mesa patch 1008)

`src/virtio/vulkan/vn_astc.c` in `src/mesa` (branch `venus-vtest`). When the
renderer lacks `textureCompressionASTC_LDR`:

- The feature is advertised. ASTC formats report the RGBA8 properties, masked to
  sampling, linear filtering, blit-src and transfer. There is no linear or buffer
  support. Image format queries allow only 2D, optimal tiling, sampled/transfer
  usage and 1 sample.
- `vkCreateDevice`: the feature is removed before the call reaches the
  renderer. A chained `VkPhysicalDeviceFeatures2` is patched in place for the
  call and then restored.
- ASTC images, views and `vkGetDeviceImageMemoryRequirements` use
  `R8G8B8A8_UNORM`/`_SRGB` on the renderer. ASTC entries in a format list are
  rewritten. `vn_image::astc_emu_format` keeps the app format.
- `vkCmdCopyBufferToImage(2)` into such an image: the source buffer's memory
  (`vn_buffer::astc`, recorded at bind) is read on the CPU. It is decoded with
  `_mesa_unpack_astc_2d_ldr` (already in `mesa_util`) into a host-visible
  staging buffer owned by the command buffer, and the copy is re-encoded from
  that buffer. Decoding happens at record time, which is fine for UE4 and
  ANGLE because they fill staging buffers before recording. One 8 MiB chunk is kept
  across resets; larger chunks are freed on reset or free.
- `VN_DEBUG=no_astc_emu` disables it.

Not covered: ETC2, ASTC HDR/3D, reading an ASTC image back with
`vkCmdCopyImageToBuffer` (it returns RGBA8 texels), and uploads from a buffer
the guest never mapped (skipped with a log line). VRAM use of ASTC textures
grows 4-9x.

This also changes ANGLE: GLES apps now see `GL_KHR_texture_compression_astc_ldr`.

## Test

- Host: `sh venus/tests/run-astc-test.sh` uses a private vtest server and a host
  Venus build in `src/mesa/build-host` (clang; gcc 16 ICEs in `vk_image.c`).
  It uploads 6x6 void-extent blocks into a 30x20, 2-layer image with padded rows,
  reads back RGBA8, and prints PASS.
- Waydroid: `sudo sh venus/deploy-astc.sh install` (backup
  `vulkan.virtio.so.pre-astc`, `rollback` restores it). The game no longer logs any
  `is unknown [raw type 0]`. It renders the TiMi logo and the loading screen, and
  then reaches "Downloading additional resources" (1456 MB) and the notification
  permission prompt. It ran for more than 4 minutes without a tombstone.

## Left open

- On the first run the resource download stayed at 0.0 MB/s; a later run got through it to the Login screen.
  That is networking in the container (see the waydroid-net work), not the GPU.
- "Freeze" reports: Android 13 opens the assistant when the Meta key is pressed
  and released alone (`adb shell input keyevent KEYCODE_META_LEFT` reproduces it).
  Hyprland's Super key reaches Android when Waydroid has focus, and the Gemini
  "floaty" activity covers the game. UE4 pauses: 0% CPU and no new buffers. If
  the overlay opens before the first frame, the window stays black. BACK
  resumes it. In that state the game presents at 25 fps on the Login screen
  (`dumpsys SurfaceFlinger --latency` on the `(BLAST)` layer), with no device
  lost and no vtest errors. It is not a GPU issue.
  `adb shell settings put secure assistant ""` turns the assistant off.
- `release/dist` still holds the old driver until `sh release/build.sh` and both
  install steps run.

## Login: the game refuses emulators (no fix)

The Login screen renders at 25 fps and keeps window focus. Two things happened
there:

- Mouse clicks do nothing. Waydroid's `wayland_pointer` sends `SOURCE_MOUSE`
  (0x2002) events, and this UE4 build only handles touch:
  `adb shell input mouse tap` changes nothing, `input touchscreen tap` works.
  Waydroid can turn mouse input into touch for a package:
  `waydroid prop set persist.waydroid.fake_touch com.garena.game.df`.
- With touch, pressing a login button (here the "..." more-logins button,
  `OnThirdPartLoginBtnClick`) shows "For fair play, avoid using emulators.
  Download the PC client at df.qq.com." with only EXIT GAME. The UE4 log says why:

  ```
  LuaMLogin: Error: [LoginUtil] GetClientType emulatorName:TencentAoYYB
  LuaMLogin: Error: [LoginInterface][OnThirdPartLoginBtnClick] is simulator
  ```

  This is the game's own policy check before login (it classifies the client as an
  emulator). It is not a GPU, driver or network failure. Getting around it would
  mean defeating the game's emulator and anti-cheat detection, so this project
  does not do that. The mobile build of Delta Force cannot be logged into from
  Waydroid. Use the PC client.

The ASTC emulation (patch 1008) is still worth keeping: it fixes the launch
crash and benefits other ASTC-only games and GLES apps.
