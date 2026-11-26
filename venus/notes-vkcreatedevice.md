# HWUI `vkCreateDevice` failure through Venus (vtest): cause and fix

Date: 2026-11-05. Mesa `src/mesa`, branch `venus-vtest`, commit `a4f948e`
(patch `venus/patches/0002-venus-global-priority-fallback.patch`, on top of 0001).

## Symptom

`F VulkanManager: Assertion failed: mCreateDevice(mPhysicalDevice, &deviceInfo, nullptr, &mDevice)`
in `VulkanManager::initialize()`.

Only **com.android.launcher3** hits it. In `crash-log-adb.txt` there are 181 launcher3 aborts with this
message. SystemUI and Settings already get past `vkCreateDevice` and abort later, at the
`has_dma_buf_import` assertion (see "Next abort").

## Why Venus logged nothing

`vn_log()` logs at `MESA_LOG_DEBUG`. Mesa's default max log level is `INFO` unless it is a
`-Dbuildtype=debug` build (`src/util/log.h`), so `mesa.vn.debug` was parsed (`debug = 0x7`)
but every line was filtered out. The property mapping itself works: `VN_DEBUG` is looked up as
`debug.mesa.vn.debug`, then `vendor.mesa.vn.debug`, then `mesa.vn.debug` (`os_misc.c`).

To see Venus logs, also set the log level. No root is needed for `debug.*` props:

```
adb -s 192.168.240.112:5555 shell setprop debug.mesa.log.level debug
```

For the setting to survive restarts, add `mesa.log.level = debug` to the `[properties]` section
of `waydroid.cfg`. Logs show up under the logcat tag `MESA-VIRTIO`.

## Cause

With logging on, launcher3 shows:

```
D MESA-VIRTIO: renderer instance version 1.4.363
D MESA-VIRTIO: vn_CreateDevice: VK_ERROR_NOT_PERMITTED
F VulkanManager: Assertion failed: mCreateDevice(...)
```

Launcher3 (QuickStep) asks HWUI for a high-priority render context, so HWUI chains
`VkDeviceQueueGlobalPriorityCreateInfo{ VK_QUEUE_GLOBAL_PRIORITY_HIGH }` on its graphics queue.
SystemUI and Settings don't, which is why only launcher3 fails. Venus exposes
`VK_KHR/EXT_global_priority` and forwards the struct unchanged. The host NVIDIA driver then
refuses HIGH/REALTIME for a process without `CAP_SYS_NICE` (`virgl_render_server` runs as the
user) and returns `VK_ERROR_NOT_PERMITTED`. The spec allows that error. HWUI does not handle it.

So the host (NVIDIA) rejects the request, not the guest's extension or feature filtering. No
extension, YCbCr or protected-memory request is involved.

## Fix (patch 0002, `src/virtio/vulkan/vn_device.c`, `vn_device_init`)

If the host `vkCreateDevice` returns `VK_ERROR_NOT_PERMITTED`, the fix retries once with every
queue that asked for a priority above MEDIUM set back to the default priority. For those queues
it drops `pNext`. That is safe because the global-priority struct is the only
`VkDeviceQueueCreateInfo` pNext that the Venus encoder puts on the wire. The host frees the
failed device object, so the retry can reuse the same object id. The fix logs
`global priority not permitted; using default` when `VN_DEBUG(INIT)` is set. Hosts that have
the privilege still get the elevated priority.

Other options I considered:
- Hide `VK_KHR/EXT_global_priority`. HWUI chains the struct regardless of extension support on
  this Android version, so this is fragile, and it would also take the feature away from
  privileged hosts.
- Give the host `CAP_SYS_NICE`
  (`sudo setcap cap_sys_nice+ep /usr/lib/virgl_render_server`). This would also work without a
  Mesa change, but it means changing system security settings. The patch is the cleaner default.

## Next abort

This happens once a process is past `vkCreateDevice`. SystemUI and Settings already show it:

```
F libc: ../src/virtio/vulkan/vn_physical_device.c:2733: vn_GetPhysicalDeviceImageFormatProperties2:
        assertion "physical_dev->instance->renderer->info.has_dma_buf_import" failed
  #02 vulkan.virtio.so (vn_GetPhysicalDeviceImageFormatProperties2)
  #04 libhwui.so VulkanSurface::Create(ANativeWindow*, ...)
  #05 VulkanManager::createSurface  #06 SkiaVulkanPipeline::setSurface
```

`VulkanSurface::Create` queries image format support with
`VkPhysicalDeviceExternalImageFormatInfo{AHB}` / `VkAndroidHardwareBufferUsageANDROID`. The
Venus AHB path asserts that the renderer can import dma-bufs, and vtest can't
(`has_dma_buf_import = false`). That is the dma-buf import work the other session is doing in
`src/mesa-dmabuf` + `src/virglrenderer`.

Verified 11:41 after `sh venus/iterate.sh` (overlay driver dated 11:40, log in `venus/crash-log-0002.txt`):
- Every launcher3 start logs `D MESA-VIRTIO: global priority not permitted; using default`.
- The `mCreateDevice` assertion count is 0.
- All 63 aborts, from both launcher3 and SystemUI, are now the `has_dma_buf_import` assertion above
  (`#04 VulkanSurface::Create` -> `#05 VulkanManager::createSurface`).

So `vkCreateDevice` now succeeds for every HWUI process, and dma-buf import support in vtest is the next blocker.
