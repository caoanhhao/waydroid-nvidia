# Where Winds Meet (com.netease.yyvn, NetEase Messiah engine)

Vulkan direct via Venus.

## Symptom (before the shim)

Loading screen stays blank; the main gameplay HUD renders but the 3D world does
not: `Enter the main hall`, `Dash`, `Burst`, `Tai Chi` and other HUD strings
float over a solid gradient, no terrain, no character, no icons. Logcat clean:
no `VK_ERROR`, no engine error.

## Finding 3: `driverID` is the real gate — FIXED

Walking the pNext chain of `vkGetPhysicalDeviceProperties2`, `Messiah` reads
`VkPhysicalDeviceVulkan12Properties.driverID` (and the legacy
`VkPhysicalDeviceDriverProperties`). As long as the driverID stays
`VK_DRIVER_ID_MESA_VENUS` (29), the engine takes the text-only fallback — even
when `vendorID` / `deviceID` / `deviceName` all look mobile.

The shim now walks `VkPhysicalDeviceProperties2.pNext` and, whenever the
override names Qualcomm (`0x5143`) or ARM (`0x13B5`), rewrites both legacy and
Vulkan 1.2 entries:

- `driverID` → `VK_DRIVER_ID_QUALCOMM_PROPRIETARY` or
  `VK_DRIVER_ID_ARM_PROPRIETARY`
- `driverName` → `"Qualcomm driver"` / `"ARM driver"`
- `driverInfo` → `"V@740.0 (Adreno)"` / `"0.0.0"` to match the rest of the
  spoofed identity

**Verified**: the epilepsy warning page draws with its full background
pattern + icon, the main menu plays the character-in-the-rain cinematic with
PBR lighting / particles / cloth, and "Resume" progresses through the save
loader to 100 %.

**Remaining**: when the world actually loads, terrain / characters / buildings
still do not draw — only sky + HUD text (same symptom as before, but now the
HUD labels are crisp: `Heavy Attack`, `Deflection`, `Tai Chi`, `Dash`, …).
Logcat clean — no VK_ERROR, no engine error, no OOM. HWUI reports ~37 frames
(the HUD overlay only), suggesting the engine's world render path produces a
command stream that draws nothing visible but does not fault.

The instance / device `vkCreate*` extension list the game asks for is all
things Venus already exposes on this build (27 device extensions, API
`1.1.0`), so there was never an extension gap to fix. The Samsung device-model
spoof idea (`ro.product.model`) is also not needed; `waydroid.cfg` cannot
propagate it anyway because it is baked into `/system/build.prop`.

## Finding 1: GPU name whitelist — NOT the bug

First working hypothesis was that Messiah drops into a text-HUD fallback when
the Vulkan device name is not on an internal mobile whitelist. We built a
per-process Vulkan HAL shim (`venus/wwm-vkhook/`) that rewrites
`VkPhysicalDeviceProperties.{vendorID,deviceID,deviceName}` for the processes
listed in `/vendor/etc/wwm_vkhook.conf`, with `com.netease.yyvn` mapped to
Qualcomm / `0x43050a01` / `Adreno (TM) 740`. ANGLE confirms the override in
the game process:

    I IJKMEDIA: [GLES2] Renderer = ANGLE (Qualcomm, Vulkan 1.3.354
                (Adreno (TM) 740 (0x43050A01)), venus-26.2.6)

With the override active the symptom partly changes — a real skybox (blue sky
with cumulus clouds) renders where before there was only a flat gradient — but
terrain, characters and HUD icons are still missing. The engine's gating does
not depend on the device name on this build; something else in the render path
is still dropping the geometry passes.

### pthread_once / zygote gotcha

First cut of the shim used `pthread_once` to load the config once per process.
Android zygote preloads the Vulkan HAL before forking app processes, so the
"once" fires in zygote (`cmdline="zygote"`, no match) and every forked app
inherits the "already done" state — the override never activates. The fix is
to key the "config loaded" state on `getpid()` and reload when the pid
changes; `ensure_cfg()` runs on every `vkCreateInstance` and
`vkGetInstanceProcAddr` entry.

The shim still loads and the loading cinematic (`Video/Splash_vn_oversea.mp4`
via ijkplayer) renders fine either way — because it is a pre-rendered MP4 the
engine plays over ANGLE, not a real-time 3D scene.

The shim sits where the Android Vulkan loader looks
(`/vendor/lib{64,}/hw/vulkan.virtio.so`), dlopens the real Venus driver kept
beside it as `vulkan.virtio_real.so`, and wraps its `CreateInstance` and
`GetInstanceProcAddr`. Config is `/vendor/etc/wwm_vkhook.conf`; every process
not listed sees Venus unchanged (SurfaceFlinger, HWUI, other apps).

Deploy: `sudo sh venus/wwm-vkhook/deploy.sh install` (rolls back the same way).

## Finding 4: anti-cheat flags emulator, but that is not the gate

NetEase's Pharos SDK (shipped inside yyvn) logs:

    PHAROS_JNI: property: is_ad_simulator=1
    UniSDK netease_global: risk_log "model:SM-S918B, cpu_abi:x86_64,
                                     system_version:13, is_rooted:1"
    UniSDK: extendFunc: unknown methodId: startEmulatorDetect

So the anti-cheat does flag the device as an emulator — rooted, x86_64,
`risk_event:"rootcheck_true"`, `"is_emulator":true` — and `is_ad_simulator`
stays `1` whatever we spoof. We tried, cumulatively via `waydroid.cfg` +
`waydroid upgrade --offline`:

- `ro.product.cpu.abi = arm64-v8a`,
  `ro.product.cpu.abilist = arm64-v8a,armeabi-v7a,armeabi,x86_64,x86`
  (ARM-first), matching `abilist32`/`abilist64`.
- `ro.build.tags = release-keys`, `ro.build.type = user`,
  `ro.build.fingerprint = samsung/.../user/release-keys`.
- `ro.build.flavor = dm3qxxx-user`, `ro.build.product = dm3q`,
  `ro.build.description = …`.
- `ro.bootloader = S918BXXU3BXDA`, `ro.hardware = qcom`,
  `ro.board.platform = kalama`, `ro.product.board = kalama`,
  `ro.boot.hardware = qcom`.
- `ro.boot.vbmeta.device_state = locked`,
  `ro.boot.verifiedbootstate = green`.

`is_ad_simulator=1` persists. Pharos reads something past these props —
likely `/proc/cpuinfo` (still says GenuineIntel to the native side even
after the Houdini bind-mount fix), sensor count (Waydroid exposes none),
`/dev/binder_*` names (anbox-binder), the Houdini presence itself, or a
JNI-level ABI probe that looks at the mapping rather than at
`Build.CPU_ABI`.

**But this is not what gates the world render.** The symptom is identical
with `is_ad_simulator=0` claimed vs. `is_ad_simulator=1` — same sky, same
HUD, no terrain. A real text-only emulator fallback would also skip the
day-night skybox and the character cinematic in the menu. It does not.
So the world render path is failing for a different reason, and
anti-cheat is not it. The cfg changes are kept because they reduce
noise and are cheap, not because they fix anything.

## Finding 5: Houdini cpuinfo bind-mount fix — real, but not the gate

Logcat before:

    W nativebridge: Failed to bind-mount /system/etc/cpuinfo.arm64.txt
                   as /proc/cpuinfo: No such file or directory

Fix: drop the stock files from the image into the system overlay:

    /system/lib64/arm64/cpuinfo → /var/lib/waydroid/overlay/system/etc/cpuinfo.arm64.txt
    /system/lib/arm/cpuinfo     → /var/lib/waydroid/overlay/system/etc/cpuinfo.arm.txt

(The names must be `cpuinfo.arm.txt` / `cpuinfo.arm64.txt` — that is what
libnativebridge2/Houdini looks for in `/system/etc/`, see
`libnativebridge/src/native_bridge.cc`.)

Verified: after an overlay-aware container restart (see also the
`mount_overlays = False` revert note below), the warning disappears from
logcat and Houdini sets `[houdini]: Houdini now in android app mode` for
the yyvn process. So arm64 apps get the ARM `/proc/cpuinfo` instead of
the host's `vendor_id: GenuineIntel`.

**But the world still does not draw.** Same symptom. So the cpuinfo gap
was real, the fix is kept (the overlay files stay), but it is not what
was gating the 3D render.

## Finding 6: `waydroid upgrade` silently flips `mount_overlays = False`

Every `sudo waydroid upgrade --offline` — which is the ONLY way for
`waydroid.cfg [properties]` to reach `waydroid_base.prop` — rewrites the
cfg and leaves `mount_overlays = False` behind. The symptom is dramatic:
SurfaceFlinger aborts with `no suitable EGLConfig found, giving up` and
the Waydroid window is a transparent rectangle. Guard against this in
the release path, or add a post-upgrade check. Set it back with:

    sudo sed -i 's/^mount_overlays = .*/mount_overlays = True/' \
        /var/lib/waydroid/waydroid.cfg
    sudo systemctl restart waydroid-container

(See also `release/install.sh` — it sets this on install but not on
`waydroid upgrade`.)

## Finding 2: still missing — the main 3D render path

Open symptoms with the shim active:

- Sky dome renders; chat panel renders; text HUD (`Enter the main hall`,
  `Dash`, `Burst`, `Tai Chi` …) renders over it.
- No terrain, no character, no world-space icons.
- Logcat has no `VK_ERROR`, no engine-side error line, no pipeline-compile
  log. The game just does not draw those passes.
- `MessiahNativeActivity.getAvailableMemory` is reported as a signed int cast
  of `ActivityManager.MemoryInfo.availMem`. With ~16 GiB available on the host
  it wraps to a large negative number. The engine logs it but does not appear
  to branch on it — the symptom predates the wrap.

What the extension dump already told us: `vkCreateInstance apiVersion=1.1.0`,
27 device extensions, all of which Venus already exposes on this build. No
extension gap. The engine is not asking for mesh shading / descriptor buffers
/ ray tracing. So the world render path is being skipped by a non-extension
decision in the engine, not by `vkCreatePipeline` returning a feature error.

Hypotheses worth trying next, roughly cheap → expensive:

1. **Device model spoof via `waydroid upgrade --offline`.** `waydroid.cfg
   [properties]` only takes effect when `waydroid upgrade` regenerates
   `waydroid_base.prop`; a session restart does not pick up changes (see
   `/usr/lib/waydroid/tools/helpers/lxc.py::make_base_props`). Patch 1010
   never applied this session either — that is why ANGLE printed
   `"Virtio-GPU Venus (NVIDIA GeForce RTX 4070)"` rather than the stripped
   form. Running `sudo waydroid upgrade --offline` would both pick up the
   deviceName patch AND let a `ro.product.model = SM-S918B` override reach
   the game. Cheap if the release install already backs up state.
2. **VK_LAYER_LUNARG_api_dump** to see what `vkCmdDraw*` actually runs and
   whether any `vkCreateGraphicsPipelines` silently gets 0 pipelines.
   Requires either `ro.debuggable=1` or `/data/local/debug/vulkan/` + per-app
   settings; can be enabled in Waydroid because it is `userdebug`.
3. **RenderDoc frame capture** once we know the game actually submits draws.
4. **Mesa patch to tweak what Venus reports to the game**: feature bits in
   `VkPhysicalDeviceFeatures2` the shim could also lie about (shader-storage
   16-bit, variable-pointers, subgroup extended types) once we have an
   API-dump trace.
5. **APK repack to disable `EnableBindlessGpuScene` /
   `HexGpuDrivenEnabled`** in `Platform.bin`. Last resort — requires signing.

## Historical note: system_server watchdog during scene load

Observed once before the pid fix: after a loading cinematic finished,
system_server's UI (`android.ui`) thread blocked in
`HardwareRenderer.nSetStopped` for 60 s and the watchdog killed system_server
(`WATCHDOG KILLING SYSTEM PROCESS: Blocked in handler on ui thread`). yyvn
then died with `DeadSystemException`.

HWUI there renders through ANGLE → Venus → vtest. Hypothesis: the game pushes
enough concurrent Vulkan work through vtest that an unrelated system_server
Vulkan call (swapchain stop) never gets scheduled within the 60 s budget. Not
reproduced since the shim-pid fix; only the loading cinematic was running back
then, not the 3D engine, so the GPU load profile was different. Revisit when
the 3D render path above actually draws.

## Waydroid overlay note

`mount_overlays = True` must stay set in `/var/lib/waydroid/waydroid.cfg`. If
it flips to `False`, the container ignores
`/var/lib/waydroid/overlay/vendor/**`, including the shim here and the
patched Venus driver, and SurfaceFlinger falls back to the stock
`vulkan.virtio.so` from vendor.img (1.4 MB, no Venus + NVIDIA patches) and
crashes with `ANGLE Display::initialize error 0: Internal Vulkan error (-3)`.
`release/install.sh` sets `True`; waydroid upgrades may silently revert it.

## Files

- `venus/wwm-vkhook/wwm_vkhook.c` — the shim.
- `venus/wwm-vkhook/wwm_vkhook.conf` — override list; one yyvn entry.
- `venus/wwm-vkhook/build.sh` — x86_64 + x86 build (NDK).
- `venus/wwm-vkhook/deploy.sh` — install / rollback / status on the Waydroid
  overlay (`/var/lib/waydroid/overlay/vendor`).
