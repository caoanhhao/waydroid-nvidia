# Play Store SIGFPE in libhwui `BlobCache::clean()`

2026-11-05. Fix: `venus/patches/release-0003-clear-hwui-shader-caches.patch` (release scripts).
Venus is not changed.

## Symptom

The Play Store dies on every launch, 1 s to 1 min after start (tombstones 14:27, 14:27, 14:30,
15:39, 15:42, and reproduced at 15:47):

```
pid: 1873, tid: 2947, name: RenderThread  >>> com.android.vending <<<
signal 0 (SIGFPE), code 1 (FPE_INTDIV)
  #00 libhwui.so android::BlobCache::clean()+130
  #01 libhwui.so android::BlobCache::set(...)+640              (update-existing-entry branch)
  #03 libhwui.so ShaderCache::store(SkData const&, SkData const&, SkString const&)+310
  #04 libhwui.so GrVkResourceProvider::storePipelineCacheData()+446
  #05 libhwui.so ShaderCache::onVkFrameFlushed(GrDirectContext*)+69
  #06 libhwui.so VulkanManager::finishFrame(SkSurface*)+893
```

The divisor (`rsi`) is 0.

## Root cause: Android 13 libhwui, not the driver

Disassembly of `/system/lib64/libhwui.so` (BuildId 65cccbf3…; symbols from `.gnu_debugdata`):

- BlobCache layout: `+0 mMaxTotalSize`, `+8 mMaxKeySize`, `+0x10 mMaxValueSize`, `+0x18 mTotalSize`,
  `+0x20 mRandState`, `+0x28 mCacheEntries`.
- `ShaderCache::initShaderDiskCache` creates `FileBlobCache(maxKey 1 KiB, maxValue 2 MiB,
  maxTotal 1 MiB)`.
- `clean()`: `while (mTotalSize > mMaxTotalSize / 2) { i = nrand48() % mCacheEntries.size(); … }`.
- `BlobCache::clear()` is inlined in `unflatten()` and in `ShaderCache::validateCache()`. It only
  destroys the entries (`mCacheEntries.clear()`) and **never writes `mTotalSize`**. Later AOSP
  versions reset it.

`validateCache()` hashes the driver identity HWUI passes in: for Vulkan, `VkPhysicalDeviceProperties::driverVersion`;
for GL, the GL strings. It compares the hash with the one stored in the file. On a mismatch it calls
`clear()`, so `mTotalSize` keeps the old file's size (up to 1 MiB) with no entries behind it. When the next
`set()` pushes the total over 1 MiB, `clean()` removes every real entry. If the stale count is over
512 KiB the loop is still true, and `% size()` divides by zero.

Why the Play Store crashes every time instead of once:

1. Its `user_de/0/com.android.vending/code_cache/com.android.skia.shaders_cache` was written under
   the old GL renderer and is 959 464 bytes, over 512 KiB (measured with sudo find, 16:06).
2. Each launch fails validation, so the stale count is over 512 KiB.
3. Its first Vulkan pipeline-cache store (`onVkFrameFlushed`) goes over 1 MiB, about 100 ms after the
   "validation fails" log line.
4. HWUI saves the file 4 s after the first store, so the crash comes first and the file keeps the old
   identity. The next launch repeats all of this.

Logged with `setprop debug.hwui.level 2` (kDebugCaches):

```
15:47:24.833 4610 4650 W OpenGLRenderer: ShaderCache::validateCache cache validation fails
15:47:24.973 4610 4650 F DEBUG   : ... Cmdline: com.android.vending  (SIGFPE, uptime 1s)
```

## Venus was checked and is not at fault

- `driverVersion`: `vn_physical_device_sanitize_properties()` replaces the host value with
  `vk_get_driver_version()`, the Mesa version. The identity is the same across Venus rebuilds and
  NVIDIA driver updates, and changes only with a Mesa version bump.
- `vn_GetPipelineCacheData()` returns the host data plus a correct 32-byte
  `VkPipelineCacheHeaderVersionOne`. Neither the size nor the header is odd. NVIDIA caches are large,
  which only makes the 1 MiB limit come sooner. With a consistent `mTotalSize`, that is harmless.

## Other apps

With `debug.hwui.level 2`, Jelly, Documents, Clock and others also logged "cache validation fails" on
their first launch after the switch. They did not crash, because their old caches were under 512 KiB.
Relaunching Jelly 3 times (with saves in between) logged no more failures, so the identity is stable.
Every app gets the miscount once; only apps with an old cache over 512 KiB crash, and they crash on
every launch. The same thing can happen in reverse after uninstalling (Vulkan to Intel GLES), and
after a Mesa version bump.

## Fix: delete the caches when the driver changes

`release-0003` adds `clear_shader_caches()` to `install.sh --system` and `uninstall.sh --system`. It
deletes `code_cache/com.android.skia.shaders_cache` under `data/` and `user_de/` of
`~$SUDO_USER/.local/share/waydroid/data` (`find -maxdepth 4`, depth 3 for `data/<pkg>/…` and depth 4
for `user_de/<u>/<pkg>/…`). Apps rebuild their caches. This was tested on a fake tree: it removes only
`code_cache/com.android.skia.shaders_cache`, not `cache/` and not the EGL `com.android.opengl.shaders_cache`.
(The file name has the `_cache` suffix; the first draft matched `com.android.skia.shaders` and missed it.)

Rejected options:
- Patching libhwui in the overlay. A binary patch of the inlined `clear()` is tied to one system.img
  BuildId, and `waydroid upgrade` would pair it with a newer framework.
- `ro.boot.qemu` (HWUI's `runningInEmulator` turns off the disk cache). It changes emulator behavior
  across the whole system.
- A Venus change. Venus can't touch HWUI's byte count; making `GetPipelineCacheData` lie about its size
  only moves the crash.

## One-off fix for the current install (sudo)

```
sudo find ~/.local/share/waydroid/data/data ~/.local/share/waydroid/data/user_de \
  -maxdepth 4 -path '*/code_cache/com.android.skia.shaders_cache' -printf '%s %p\n' -delete
```

It is safe while Waydroid runs: running apps keep their in-memory cache and write a new file with
the Vulkan identity.

## Verification (16:08–16:10)

The one-off delete removed 17 caches. Four were over 512 KiB: systemui (917 728), vending
(959 464), com.ss.android.ugc.trill (TikTok) and launcher3. TikTok would probably have hit the
same crash on its first Vulkan launch. systemui and launcher3 were running and had already passed
validation, so for them the delete only drops a valid cache.

Play Store, launched twice with `debug.hwui.level 2`:
- run 1: one "validation fails" (no file yet, so nothing stale: harmless), no SIGFPE, alive at 30 s;
- run 2: no validation failure, no SIGFPE, alive at 30 s.

`debug.hwui.level` was reset afterwards.
