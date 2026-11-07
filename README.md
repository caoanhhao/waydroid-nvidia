# Waydroid on NVIDIA (Venus over vtest)

Run Waydroid with GPU acceleration on an NVIDIA card. The host keeps its `nvidia-open` driver:
no nouveau, no need to remove the NVIDIA driver.

Android cannot load the NVIDIA driver (it is built for glibc), so Vulkan commands are forwarded
to the host:

```
App / SurfaceFlinger (Android)
  └─ Vulkan: Mesa Venus (vulkan.virtio.so)        HWUI = Skia Vulkan
  └─ OpenGL ES: ANGLE → Venus
        │  unix socket /data/vtest/vtest.sock
        ▼
virgl_test_server (host, patched, vtest v6)  ← systemd user service `waydroid-vtest`
        ▼
NVIDIA Vulkan (nvidia-open) → RTX 4070 → Hyprland
```

Window buffers are allocated by NVIDIA on the host (block-linear format) and passed into Android
through vtest. Hyprland shows them directly, without a copy.

## Requirements

| Component | Tested with |
|---|---|
| OS | Arch Linux, kernel 7.2 |
| NVIDIA GPU | RTX 4070, `nvidia-open` 615.78.08, render node `/dev/dri/renderD129` |
| Intel iGPU | UHD 770, `/dev/dri/renderD128`, used for CPU buffers (LINEAR) |
| Compositor | Hyprland 0.57 (Wayland) |
| Waydroid | 1.6.3, LineageOS 20 (Android 13) x86_64 image, with ANGLE in vendor |

The Intel iGPU is required too: buffers the CPU reads or writes (cursor, screenshots…) are still
allocated on Intel.

Check your machine's render node numbers:

```bash
ls -l /dev/dri/by-path/
```

If they differ from `renderD128` (Intel) / `renderD129` (NVIDIA), edit
`release/waydroid-vtest.service` (`--rendernode`) and `release/install.sh` (`gralloc.gbm.device`)
before installing.

## Install

### Option 1: use the prebuilt files (`release/dist/`)

If `release/dist/` already has files (see `release/dist/VERSION`), two steps are enough.

1. Host part, as your normal user, without sudo. This installs the server into
   `~/.local/lib/waydroid-venus`, enables the `waydroid-vtest` service, and stops Waydroid if it
   is running.

   ```bash
   sh release/install.sh
   ```

2. Android part, with sudo. This copies the driver and the gralloc shim into the Waydroid
   overlay, sets the properties in `/var/lib/waydroid/waydroid.cfg`, and restarts
   `waydroid-container`.

   ```bash
   sudo sh release/install.sh --system
   ```

### Option 2: build from source

Also needed: Android NDK r30 (in `/opt/android-ndk`, or set `NDK`), `meson`, `ninja`, `git`, gcc
on the host, and an installed Waydroid image (the wrapper links against its vendor libraries).

Upstream sources are not in this repository. On the first run each build script clones them into
`src/` at a fixed version and applies the patches from `venus/patches/`:

| Part | Script | Source |
|---|---|---|
| Venus for Android | `venus/build-venus-android.sh` | Mesa `mesa-26.2.6` + `0001`, `0002`, `1001`–`1004`, `1007`–`1009` → `src/mesa`, branch `venus-vtest` |
| Host server | `venus/build-virglrenderer-host.sh` | virglrenderer `1.3.0` + `vrend-0001`, `vrend-0002` → `src/virglrenderer`, branch `vtest-dmabuf` |
| Gralloc shim | `src/minigbm-vtest/build-android.sh` | `src/minigbm-vtest` (in this repo) |
| Intel LINEAR wrapper (x86_64 + x86) | `src/minigbm-wrapper/build.sh` | waydroid minigbm `lineage-18.1` @ `e5a7769` + `gralloc-*` → `src/minigbm-wrapper/minigbm` |
| hwcomposer format shim | `venus/hwc-yuv/build.sh` | `venus/hwc-yuv` |
| gralloc4 mapper YV12 shim | `venus/mapper-yv12/build.sh` | `venus/mapper-yv12` |

Build all parts:

```bash
sh venus/build-venus-android.sh && sh venus/build-virglrenderer-host.sh && sh src/minigbm-vtest/build-android.sh && sh src/minigbm-wrapper/build.sh && sh venus/hwc-yuv/build.sh && sh venus/mapper-yv12/build.sh
```

Collect the results into `release/dist/`. This also writes `VERSION` and `SHA256SUMS`.

```bash
sh release/build.sh
```

Then install as in Option 1.

## Daily use

Use Waydroid as usual: there are no scripts to run by hand.

- **Start:** from the app menu, or:

  ```bash
  waydroid show-full-ui
  ```

- **Stop:**

  ```bash
  waydroid session stop
  ```

- The `waydroid-vtest` service starts when you log in and restarts itself if it fails.

### Check that it runs on NVIDIA

The service must be `active (running)`:

```bash
systemctl --user status waydroid-vtest
```

Android apps must draw with Vulkan:

```bash
sudo waydroid shell -- dumpsys gfxinfo com.android.systemui | grep Pipeline
```

Expected output: `Pipeline=Skia (Vulkan)`.

`nvidia-smi` must list `virgl_test_server` and `virgl_render_server`:

```bash
nvidia-smi | grep virgl
```

## Fingerprint via adb

Optional: a fake under-display fingerprint sensor that you drive from adb. It is AOSP's virtual
fingerprint HAL (`fingerprint/hal/SOURCE.md`); the sensor is a spot on the screen, and a "finger"
is a long press on that spot.

1. Install (stops Waydroid), then start Waydroid again:

   ```bash
   sudo sh fingerprint/install.sh
   ```

2. Android needs a screen lock before it allows fingerprints. If there is none, set one in
   Settings > Security, or:

   ```bash
   adb shell locksettings set-pin 1234
   ```

3. Enroll a finger. This opens Settings and goes through its screens for you; enter the PIN in
   the Waydroid window when asked, or pass it as `FP_PIN`:

   ```bash
   FP_PIN=1234 sh fingerprint/fp.sh enroll
   ```

4. Lock the screen (screen off and on, like a power key press):

   ```bash
   sh fingerprint/fp.sh lock
   ```

   Do not lock with Lockdown from the power menu: Android then refuses fingerprints until the
   password is entered.

5. Touch the sensor with an enrolled finger (unlocks), with an unknown one (rejected), or force a
   lockout:

   ```bash
   sh fingerprint/fp.sh touch
   ```

   ```bash
   sh fingerprint/fp.sh bad
   ```

   ```bash
   sh fingerprint/fp.sh lockout
   ```

`sh fingerprint/fp.sh status` shows the enrolled fingers and the sensor state. The script connects
adb to the container itself (or uses `$ANDROID_SERIAL`). Like on a phone, the PIN is needed once
after every Android start before a fingerprint can unlock (see the option below), and after a
lockout or Lockdown; `touch` says which one applies. The sensor sits at
`180:560` (x:y in display pixels); for another spot run `sh fingerprint/fp.sh place`, then restart
Waydroid. Remove everything with `sudo sh fingerprint/uninstall.sh`.

### Optional: unlock without the PIN after Android starts

For automation that cannot type the PIN, an overlay turns off `config_strongAuthRequiredOnBoot`,
so `fp.sh touch` unlocks right after Android starts. After that, anyone with adb can unlock
Android. Apps that keep keys locked until the PIN is entered still wait for it, and a lockout or
Lockdown still asks for the PIN.

Install (stops Waydroid), then start Waydroid again:

```bash
sudo sh fingerprint/boot-unlock/install.sh
```

Remove it (`fingerprint/uninstall.sh` removes it too):

```bash
sudo sh fingerprint/boot-unlock/uninstall.sh
```

The APK is prebuilt in `fingerprint/boot-unlock/`; `sh fingerprint/boot-unlock/build.sh` rebuilds
it from the source there (needs Android build-tools and a running Waydroid).

## Troubleshooting

| Symptom | Fix |
|---|---|
| Black window, stuck at the boot animation, or `no suitable EGLConfig found` / `!gpuCount` in the log | The server was not running when Android started. Restart the server, then stop and start Waydroid (command below). |
| `waydroid-vtest` was restarted while Waydroid was running | Android lost its GPU connection. Always stop and start Waydroid after restarting the service. |
| Need logs | See the log commands below. |

Restart the server and reopen Waydroid:

```bash
systemctl --user restart waydroid-vtest && waydroid session stop && waydroid show-full-ui
```

Server log:

```bash
journalctl --user -u waydroid-vtest -e
```

Android crash log:

```bash
sudo waydroid shell -- logcat -d -b crash
```

List of tombstones:

```bash
sudo waydroid shell -- ls -l /data/tombstones
```

## Uninstall (back to OpenGL on Intel)

1. Android part:

   ```bash
   sudo sh release/uninstall.sh --system
   ```

2. Host part:

   ```bash
   sh release/uninstall.sh
   ```

Waydroid goes back to the Intel setup (`ro.hardware.egl = mesa`). Copies of the original
configuration, from before any change, are in `backups/`.

## Known limitations

- **GPU sync is done on the CPU.** `failed to dup EGL native fence sync` is logged on every
  frame, because vtest does not pass real fences yet. Performance has not been measured. A fix
  through vtest protocol v7 is in progress.
- **Only full-UI mode has been tested.** Multi-window mode has not been tried.
- **Heavy OpenGL ES apps (games) are not well tested.** Apps that use the native bridge
  (`libhoudini`) can crash for their own reasons; TikTok's `libhoudini` crash is one of them
  (`venus/notes-tiktok.md`).
- **The server is a separately patched build (vtest v6),** not the system `virgl_test_server`.
  It does not overwrite `/usr/lib/virgl_render_server`.

Fixed issues (the mouse cursor, video apps such as TikTok, the Play Store crash) are described
in `release/README.md`. The green camera preview is described in `venus/notes-camera.md`.

## Layout

| Path | Contents |
|---|---|
| `release/` | `build.sh`, `install.sh`, `uninstall.sh`, `waydroid-vtest.service`, `dist/` |
| `release/README.md` | Technical notes for the release |
| `venus/patches/` | Patches for Mesa, virglrenderer and minigbm |
| `venus/notes-*.md` | Diagnosis notes, one per issue |
| `venus/hwc-yuv/` | hwcomposer format shim and the YV12 import test |
| `venus/mapper-yv12/` | gralloc4 mapper shim for YV12 plane layouts and its test |
| `venus/tests/` | Test programs for the host and inside Android |
| `src/` | Own code: gralloc shim (`minigbm-vtest`), Intel wrapper build (`minigbm-wrapper`); the build scripts clone the upstream sources here |
| `fingerprint/` | Virtual fingerprint HAL (`hal/`), feature flag (`overlay/`), optional boot unlock (`boot-unlock/`), `install.sh`, `fp.sh` |
| `plan.md` | Project plan |
| `backups/` | Original Waydroid configuration |
