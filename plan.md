# Waydroid render on RTX 4070 while the host keeps nvidia-open

Status: phase 1 partly done.

## Phase 1 findings (2026-11-05)

- Host side works: `virgl_test_server --venus --no-virgl --multi-clients --rendernode
  /dev/dri/renderD129` + host Mesa 26.2.6 Venus (`vulkan-virtio`) lists
  `Virtio-GPU Venus (NVIDIA GeForce RTX 4070)` via `vulkaninfo`.
- NVIDIA driver exposes every extension Venus needs (dma_buf, drm_format_modifier,
  queue_family_foreign, fd memory/fence/semaphore).
- Android side fails: with `ro.hardware.vulkan=virtio`, `mesa.vn.debug=vtest` and
  `mesa.vtest.socket.name=/data/vtest/vtest.sock`, the vendor image's Venus (Mesa 26.0.8)
  connects (server logs `client context created`) and disconnects at once; `cmd gpu
  vkjson` lists no physical device and logcat shows nothing from Venus. Likely cause:
  the Android build of Venus rejects a renderer without dma-buf import (vtest has none),
  which phase 2 has to add anyway.
- Fix: cross-built Mesa 26.2.6 Venus with the NDK (`venus/build-venus-android.sh`) and
  loaded it through the Waydroid overlay (`venus/deploy-venus-overlay.sh`). Android's
  `cmd gpu vkjson` now lists `Virtio-GPU Venus (NVIDIA GeForce RTX 4070)` (GPU0) and the
  Intel iGPU. The stock vendor Venus (Mesa 26.0.8) was the blocker.
- Next: render through it. Switch HWUI to Vulkan (`debug.hwui.renderer=skiavk`) and check
  whether app window buffers (AHardwareBuffer) work over vtest, or need the dma-buf work
  from phase 2.

## Outcome

Android apps inside Waydroid render (GLES + Vulkan) on the RTX 4070 through the
host's proprietary `nvidia-open` driver, and frames reach Hyprland without CPU copies.

## Why a rewrite is needed

- Waydroid runs Android in an LXC container. Android userspace (bionic) cannot load
  NVIDIA's glibc libraries, and NVIDIA ships no Android driver for desktop GPUs.
- Today the vendor image only has Mesa drivers: iris (Intel, works), nouveau (needs the
  nouveau kernel driver), SwiftShader (`vulkan.pastel.so`, CPU), and Venus
  (`vulkan.virtio.so`).
- Venus already has a **vtest** transport (unix socket, reads `mesa.*` props), so the
  command stream can reach a host `virgl_render_server` that runs on NVIDIA Vulkan.
- The missing piece: Android window buffers (gralloc / AHardwareBuffer) must be memory the
  host GPU renders into. Venus only imports virtio-gpu blobs, and vtest has no dma-buf
  import, so presentation fails without code changes.

## Architecture

```
Android app ─GLES─► ANGLE or zink ─Vulkan─► Venus (vtest backend, patched)
                                              │ unix socket (bind-mounted into container)
                                              ▼
                        host virgl_render_server ─► NVIDIA Vulkan (RTX 4070)
gralloc (new "vtest" allocator) ─► host allocates exportable images ─► dma-buf fd (SCM_RIGHTS)
hwcomposer.waydroid ─► same dma-buf ─► Hyprland (already on NVIDIA, zero-copy)
```

## Phases

1. **Feasibility, no code.** Install `virglrenderer` (needs Venus enabled) on the host,
   run `virgl_test_server --venus` with its socket shared into the container, set
   `mesa.vn.debug=vtest` and `mesa.vtest.socket.name`, then run an NDK-built
   `vulkaninfo` via adb. Pass when it reports `Virtio-GPU Venus (NVIDIA GeForce RTX 4070)`.
2. **Buffer sharing.** Patch Mesa's Venus vtest renderer to export/import host blobs as
   dma-bufs; patch virglrenderer's vtest server to pass fds. Verify an image rendered
   on the host can be read in Android.
3. **gralloc + GLES.** Add a minigbm/gralloc backend that allocates through vtest and
   returns dma-bufs. Choose GLES path: ANGLE (already in the image as `libEGL_angle.so`)
   or zink. Pass when SurfaceFlinger reports GLES on Venus/NVIDIA and the UI draws.
4. **Packaging.** Cross-build Mesa with the Android NDK, ship it through Waydroid's
   overlay (`mount_overlays = True`, `/var/lib/waydroid/overlay/vendor/...`), plus a host
   systemd user service for the render server. Keep the Intel config as fallback.

## Requirements on the host

- `sudo pacman -S virglrenderer meson` (confirm Venus is enabled in Arch's build;
  otherwise build virglrenderer with `-Dvenus=true`).
- Android NDK (~2 GB download) for cross-building Mesa and test tools.
- About 15 GB disk for Mesa/virglrenderer builds (76 GB free).

## Risks

- Phase 2/3 change Mesa and virglrenderer internals; this is the same problem
  cuttlefish/crosvm solve with virtio-gpu, so expect days of work, not hours.
- NVIDIA dma-buf export needs matching DRM format modifiers between host allocation,
  Android consumers, and Hyprland.
- vtest was designed for testing; throughput may be below a real virtio-gpu path.

## Rollback

Disable the overlay and restore the Intel props
(`ro.hardware.egl = mesa`, `gralloc.gbm.device = /dev/dri/renderD128`), then
`sudo waydroid upgrade --offline`.
