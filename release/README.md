# Waydroid on the RTX 4070 (Venus over vtest)

Waydroid (LineageOS 20 / Android 13) renders on the NVIDIA RTX 4070 while the host keeps
`nvidia-open`:

- Android Vulkan: Mesa Venus (`vulkan.virtio.so`) → unix socket
  `~/.local/share/waydroid/data/vtest/vtest.sock` (`/data/vtest/vtest.sock` in Android) →
  patched host `virgl_test_server` (vtest protocol v6) → NVIDIA Vulkan.
- Android GLES (SurfaceFlinger, apps): ANGLE on Venus. HWUI: Skia Vulkan.
- Window buffers: allocated on the host as NVIDIA block-linear dma-bufs through vtest
  (gralloc shim `libgbm_mesa_wrapper.so`; other buffers go to the Intel wrapper, LINEAR).

| file | where it goes |
|---|---|
| `dist/android/vulkan.virtio.so` | overlay `/vendor/lib64/hw/` |
| `dist/android/libgbm_mesa_wrapper.so` (vtest shim) | overlay `/vendor/lib64/` |
| `dist/android/libgbm_mesa_wrapper_intel.so` (Stage A wrapper) | overlay `/vendor/lib64/` |
| `dist/host/bin/*`, `dist/host/lib/*` | `~/.local/lib/waydroid-venus/` |
| `waydroid-vtest.service` | `~/.config/systemd/user/`, enabled for `default.target` |

## Build

The sources are built by `venus/build-venus-android.sh` (Mesa `src/mesa`, branch `venus-vtest`),
`venus/build-virglrenderer-host.sh` (`src/virglrenderer`, branch `vtest-dmabuf`),
`src/minigbm-vtest/build-android.sh` and `src/minigbm-wrapper/build.sh`. Then:

```bash
sh release/build.sh
```

This copies the pieces into `release/dist/` and writes `VERSION` and `SHA256SUMS`.

## Install

```bash
sh release/install.sh
```

```bash
sudo sh release/install.sh --system
```

The first command runs as your user. It stops a running Waydroid session, installs the server,
and enables and starts `waydroid-vtest.service`. The second installs the overlay files, sets the
`waydroid.cfg` properties, removes debug properties, and runs `waydroid upgrade --offline` and
restarts `waydroid-container`.

After that, use Waydroid as usual: the app menu, `waydroid show-full-ui`, and
`waydroid session stop`. The service starts at login. To update after a rebuild, run
`build.sh` and both install commands again.

Check:

```bash
systemctl --user status waydroid-vtest
```

```bash
sudo waydroid shell -- dumpsys gfxinfo com.android.systemui | grep Pipeline
```

The second command should print `Pipeline=Skia (Vulkan)`. `nvidia-smi` should list
`virgl_test_server` and `virgl_render_server`.

## Uninstall (back to Intel GLES)

```bash
sudo sh release/uninstall.sh --system
```

```bash
sh release/uninstall.sh
```

## Known issues

- **The server must run before Android starts.** Without it SurfaceFlinger aborts (`no suitable
  EGLConfig found`). Restarting the service while Waydroid runs kills Android's GPU clients,
  so restart Waydroid after it (`waydroid session stop`, then open it again).
- **Fences are CPU waits.** `failed to dup EGL native fence sync` is logged on every frame
  (Mesa patch 0001). Performance has not been measured.
- **Multi-window mode is untested.** It uses the composer EGL path fixed for the cursor
  (below) for snapshots, but it has not been tried. Full-UI mode works.
- **Only GLES in SurfaceFlinger is tested.** GLES apps beyond SurfaceFlinger have not been tested.
- **The render node is fixed.** It is hardcoded as `/dev/dri/renderD129` (the 4070), and Intel
  is `renderD128`. If the numbering changes, edit the unit and `gralloc.gbm.device`.

Fixed: the mouse pointer shows over the Waydroid window. Android 13 ANGLE looks up
`AHardwareBuffer_*` only in the executable's own dependencies, and the hwcomposer does not
link libnativewindow, so its EGL cursor path crashed. The overlay replaces
`/vendor/bin/hw/android.hardware.graphics.composer@2.1-service` with a wrapper that execs the
vendor.img binary (copied next to it as `.real`) with `LD_PRELOAD=libnativewindow.so`, and
`persist.waydroid.cursor_force_shm` is no longer set. (An init `.rc` service override did not
work: init did not apply it.) Details: `venus/notes-cursor.md`.

Fixed: video apps (TikTok) no longer bring down the composer and SurfaceFlinger.
- Software video decoders output YV12 buffers, which gralloc allocates through Intel LINEAR.
  The hwcomposer used to pass them to Hyprland, which can't import them. That closed its
  Wayland connection, the composer aborted, and SurfaceFlinger aborted after it.
- The overlay now installs a shim as `/vendor/lib64/hw/hwcomposer.waydroid.so`. It loads the
  vendor.img module (copied as `hwcomposer.waydroid.real.so`) and sends SurfaceFlinger every
  layer whose format/modifier Hyprland does not advertise.
- SurfaceFlinger can now import those buffers because of Mesa patch 1007: gralloc describes
  YV12 as one plane, and Venus now builds the three-plane layout itself.
- Each kind of layer that gets redirected is logged to `/data/waydroid_hwc_format_shim.txt`
  (`~/.local/share/waydroid/data/` on the host).
- `waydroid prop set persist.waydroid.hwc_unsupported drop` hides those layers instead, and
  `off` restores the vendor behaviour.

Details: `venus/notes-tiktok.md`.

Fixed: the Play Store crashed on every launch (SIGFPE in libhwui `BlobCache::clean()`).
HWUI tags each app's Skia shader cache with the GPU driver; after the switch from GLES to Vulkan
the old cache fails that check, and Android 13's `BlobCache::clear()` keeps the old byte count.
An app whose old cache was over 512 KiB then crashes before it saves a new one. Both
`--system` scripts now delete `code_cache/com.android.skia.shaders_cache` for every app, and apps
rebuild them. Details: `venus/notes-blobcache.md`.

Details: `venus/notes-dmabuf.md`, `HANDOFF.md`, patches in `venus/patches/`.
