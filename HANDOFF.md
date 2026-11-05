# Waydroid on RTX 4070 (nvidia-open host)

Last updated: 2026-11-05 ~14:35 (+07)

## Goal and status

Make Waydroid render on the NVIDIA RTX 4070 while the host keeps the proprietary
`nvidia-open` driver. **Reached:**
- The Android UI renders on the 4070 and shows on Hyprland.
- systemui reports `Pipeline=Skia (Vulkan)`.
- SurfaceFlinger GLES runs on ANGLE over Venus on the RTX 4070.
- Window buffers are NVIDIA block-linear (`0x0300000000606014`), allocated by the host.
- Waydroid starts and stops with the normal commands.

Screenshots: `venus/screenshots/stageB-lockscreen-1402.jpg`, `venus/screenshots/release-reopen-1424.jpg`.
Not yet tested: a full host reboot.

## Machine facts

- Arch Linux, kernel 7.2.14, Hyprland on the RTX 4070 (`nvidia-open` 615.78.08, monitor `DP-2` on `card2`).
- iGPU Intel UHD 770: `card1` / `renderD128`. RTX 4070: `card2` / `renderD129`.
- Waydroid 1.6.3, LineageOS 20 (Android 13, API 33), x86_64.
- No passwordless sudo: the user runs the sudo steps.

## How it works

Android cannot load NVIDIA's glibc driver, so Vulkan is forwarded (design: [plan.md](plan.md)):

- **Vulkan:** Mesa Venus (`vulkan.virtio.so`) in Android → vtest unix socket
  (`~/.local/share/waydroid/data/vtest/vtest.sock`, `/data/vtest/vtest.sock` in Android) →
  patched host `virgl_test_server` → NVIDIA Vulkan.
- **vtest protocol extensions:**
  - v5 `VCMD_RESOURCE_IMPORT_BLOB`: dma-buf import.
  - v6 `VCMD_ALLOC_DMABUF_IMAGE`: window buffers allocated by NVIDIA on the host.
- **gralloc:** a shim (`src/minigbm-vtest`) gets window buffers from the host. CPU-usage
  buffers go through the Intel minigbm wrapper, forced LINEAR (`src/minigbm-wrapper`).
- **GLES:** all of it runs on ANGLE over Venus (`ro.hardware.egl = angle`), so Intel never
  renders into NVIDIA buffers.
- **Cursor:** drawn by the composer's EGL path into shm (`cursor_force_shm` unset). The overlay
  replaces the composer binary with `composer-preload-wrapper`, which execs the vendor.img
  binary (`.real`) with `LD_PRELOAD=libnativewindow.so` so ANGLE's image import works there.
  Verified 15:38 with the pointer over the window (`venus/notes-cursor.md`).

## Install / uninstall

See [release/README.md](release/README.md):

```
sh release/build.sh                   # collect built pieces into release/dist
sh release/install.sh                 # host: vtest server + systemd user service waydroid-vtest
sudo sh release/install.sh --system   # Android: overlay files + waydroid.cfg props
```

Uninstall:

```
sudo sh release/uninstall.sh --system
sh release/uninstall.sh
```

## Sources (not in git, see .gitignore)

| Path | What |
|------|------|
| `src/mesa` | Mesa 26.2.6, branch `venus-vtest` c5af4b2: patches 0001, 0002, 1001 (dma-buf v5), 1002–1004 |
| `src/virglrenderer` | virglrenderer 1.3.0, branch `vtest-dmabuf`: vrend-0001/0002 |
| `src/minigbm-vtest`, `src/minigbm-wrapper` | gralloc shim (Stage B) and LINEAR Intel wrapper (Stage A) |
| `src/mesa-fence`, `src/virglrenderer-fence` | WIP worktrees: real sync-fd fences (vtest v7) |

Rebuild:
- Mesa: `venus/build-venus-android.sh` (cross file `venus/android-x86_64.cross`).
- virglrenderer: `venus/build-virglrenderer-host.sh`.

Every change is also exported as a patch in `venus/patches/`.

## Notes

- `venus/notes-vkcreatedevice.md`: patch 0002 (global-priority fallback).
- `venus/notes-dmabuf.md`: vtest v5/v6, gralloc shim, ANGLE fixes 1002–1004, cursor workaround.
- `venus/gralloc-findings.md`: buffer layouts NVIDIA can import, and the Hyprland display path.
- `venus/notes-cursor.md`: cursor fix (composer wrapper).
- `venus/notes-tiktok.md`: YV12 video layers (hwcomposer shim, Mesa patch 1007).
- `venus/notes-blobcache.md`: Play Store SIGFPE from stale HWUI shader caches (cleared by the
  `--system` scripts).
- `venus/notes-fence.md`: open work (below).

These notes are lab logs. Scripts they mention that were replaced by `release/` have been removed.

## Open work

- **Fences** (`notes-fence.md`, patches 1006 / vrend-0003): real sync fds over vtest v7
  replace patch 0001's CPU wait, and the `failed to dup EGL native fence sync` warning.
  Before/after measurement in progress (`venus/fence-measure.sh`, `venus/deploy-fence.sh`).

## Backups and changes outside the project

- `backups/waydroid.cfg.bak`, `backups/env.lua.bak`: originals before any change.
- `~/.config/hypr/config/custom/env.lua`: NVIDIA env vars are skipped when the kernel
  cmdline has `waydroid.gpu=nouveau`. This is left over from the abandoned nouveau
  approach and has no effect on a normal boot.
