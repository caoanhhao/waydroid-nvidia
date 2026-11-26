# Real sync-fd fences over vtest (protocol 7)

Status (2026-11-05 ~14:35):
- Works on the host. The tests export real NVIDIA sync_files.
- All four old/new client/server combinations work.
- Not yet installed in Waydroid.
- The before/after numbers inside Android are still to be measured. Use the commands in
  [Measuring in Android](#measuring-in-android) and fill in the table there.

This replaces patch 0001's fake export. Patch 0001 CPU-waits in `vtest_sync_export_syncobj`
and then returns -1, an already-signalled sync file.

## Pieces

| What | Where |
|------|-------|
| Server patch | `patches/vrend-0003-vtest-add-VCMD_SYNC_EXPORT_FD-protocol-version-7.patch`<br>On top of vrend-0002. Branch `vtest-fence` 9199d745, worktree `src/virglrenderer-fence`. |
| Mesa patch | `patches/1006-mesa-venus-vtest-export-real-sync-fds-vtest-protocol-vers.patch`<br>On top of c5af4b2 (`venus-vtest`). Branch `venus-fence` 49556d9, worktree `src/mesa-fence`. |
| Host server build | `src/virglrenderer-fence/build/{vtest/virgl_test_server, server/virgl_render_server, src/libvirglrenderer.so.1*}` |
| Android driver build | `src/mesa-fence/build-android/src/virtio/vulkan/libvulkan_virtio.so` (sha256 `bc55c18a…`) |
| Host Venus ICD (tests) | `src/mesa-fence/build-host` |
| Host test | `tests/vtest-sync-fd.c`, `tests/run-sync-fd-test.sh [new\|old-server] [iterations]` |
| Install / roll back | `deploy-fence.sh install\|rollback [--system] \| status` |
| Android measurement | `fence-measure.sh before\|after` → `fence-measure-<label>.txt` |

## Protocol: `VCMD_SYNC_EXPORT_FD = 41` (vtest version 7)

```
req  hdr { len = 3, id = 41 }
     [0] sync id   [1] value lo   [2] value hi
rsp  hdr { len = 1, id = 41 }
     [0] status: 0 SIGNALED   sync already >= value, no fd (client returns -1)
                 1 FD         a sync_file follows (SCM_RIGHTS)
                 2 UNAVAILABLE  no pending submit signals it / context cannot export:
                                client falls back to the CPU wait (0001 behavior)
```

### Server (`vtest_sync_export_fd`)

1. Finds the earliest pending `vtest_timeline_submit` that signals `(sync, ≥ value)`.
2. Calls the new `virgl_renderer_context_export_fence(ctx, ring_idx, fence_id, &fd)` →
   `virgl_context::export_fence`.
3. The proxy checks the shmem seqno first. If the fence has already signalled, it skips the
   round trip and returns -1.
4. Otherwise it sends the new render-server op `RENDER_CONTEXT_OP_EXPORT_FENCE {ring_index}`,
   with a reply `{ret}` plus an fd.
5. vkr (`vkr_queue_export_sync_fd`) does `vkQueueSubmit(queue, 0, NULL, export_fence)` on the
   ring's queue, then `vkGetFenceFdKHR(SYNC_FD)`.
   - The fence is created once per queue with `VkExportFenceCreateInfo(SYNC_FD)`. vkr already
     requires `VK_KHR_external_fence_fd` with SYNC_FD export for its own sync fences.
   - Exporting SYNC_FD resets the fence (copy transference), so the fence is reused.
   - The sync_file is `NVIDIA/nvidia.semaphore_surface`.

**Detach on export.** Venus resets the sync right after export: `vn_GetFenceFdKHR` and
`vn_GetSemaphoreFdKHR` both call `vn_renderer_sync_reset`. The still-pending vtest submit
would later set the sync back to 1. A reused fence would then look signalled early, and the
next export would wrongly answer SIGNALED. So on FD or SIGNALED, the server removes the sync
from every pending submit (`submit->syncs[i] = NULL`), and the signal and free paths skip
NULL. The test checks this ("fence stays reset after export").

### Precision

The sync_file covers *all* work submitted to that queue at export time, not just the batch
that signals the sync. It can signal later than strictly needed (if more work was queued in
between), never earlier.
- The exact variant would export from a semaphore signalled by the batch's own fence submit.
  That needs to know at submit time which syncs will be exported, and vn_renderer syncs carry
  no such flag. HWUI creates a new semaphore every frame, so an "exported before" heuristic
  would not work.
- In practice ANGLE and HWUI export right after their flush, so nothing extra is queued.

### Client (`vn_renderer_vtest.c`, Mesa vtest header)

- Requests protocol 7. With a v7 server, `export_syncobj` sends `VCMD_SYNC_EXPORT_FD`.
  UNAVAILABLE or an old server falls back to the 0001 CPU wait plus -1.
- New `vn_renderer_info::has_host_sync_fd` (set for v7). `vn_wsi_sync_wait` returns early
  when it is set.
  - Upstream, `vn_GetSemaphoreFdKHR` CPU-waits on every exported fd unless the renderer has
    implicit fencing (for guest fences a host compositor cannot see).
  - Without this change, HWUI's per-frame semaphore export would still block the render
    thread.
  - Our fds are host GPU fences, so the wait is not needed.
- `has_external_sync` stays true for every server version, as in 0001.

### Compatibility

The render-server protocol gained an op. The render server and `libvirglrenderer` /
`virgl_test_server` must come from the same build: an old render server rejects op 7 and
drops the context. They are shipped and installed together, so this is fine.

## Host test results (RTX 4070, nvidia-open 615.78.08)

`sh venus/tests/run-sync-fd-test.sh <mode> 5`. Each submit is 40 × `vkCmdFillBuffer` of
256 MiB: 30.3 ms by `vkWaitForFences`, about 24 ms by the sync fd.

| Client | Server | Export call (fence / semaphore) | fds | Result |
|--------|--------|------------------------------|-----|--------|
| fence (1006) | fence (vrend-0003) | **0.10 / 0.09 ms** | 10/10 real sync_files | PASS |
| fence (1006) | release v6 | 30.7 / 30.7 ms (CPU wait) | 0, returns -1 | PASS (fallback) |
| release (0001) | fence (vrend-0003) | 30.2 / 30.2 ms (CPU wait) | 0, returns -1 | PASS (negotiates v5) |

Checks on every real fd:
- `SYNC_IOC_FILE_INFO` works: one fence, `NVIDIA/nvidia.semaphore_surface`.
- It is not signalled right after export.
- It signals about 24 ms after submit, which is no earlier than the work.
- The re-used `VkFence` reads `VK_NOT_READY` after the exported work has finished, so there
  is no stale signal.

Side observation: the sync fd signals about 6 ms sooner than Venus's own `vkWaitForFences`
returns for the same work. Venus polls the vtest sync with `vn_relax` back-off.

## Not changed: import (sync fd → wait)

- An imported SYNC_FD (an acquire fence handed to Venus) is still waited on the CPU:
  - `vn_semaphore_wait_sync_fd` at submit;
  - `sync_wait` in `vn_WaitForFences` / `vn_GetFenceStatus`.
- On Waydroid these are mostly SurfaceFlinger/HWC release fences, which are usually already
  signalled, so the gain is expected to be small. Measure before doing it.

Design if it is needed (v8):
1. Use the existing `VCMD_SUBMIT_CMD2_FLAG_IN_FENCE_FD` (fd over SCM_RIGHTS, already parsed
   by the vtest server into `virgl_renderer_attach_fence`).
2. Add a proxy → render-server op that carries the fd.
3. vkr imports the fd into a temporary SYNC_FD `VkSemaphore` and submits an empty batch on
   the ring's queue that waits on it. This needs `VK_KHR_external_semaphore_fd` enabled on
   the host device; vkr only enables `external_fence_fd` today.
4. Ordering: Venus queue submits go through the ring, not the vtest socket. So the client
   must flush the ring (`vn_ring_wait_roundtrip`) before sending the in-fence. Otherwise the
   wait could land before earlier ring submits it depends on.

## Risk to check after install

- With 0001, SurfaceFlinger handed `-1` acquire fences to the Waydroid hwcomposer. Now they
  are real, pending NVIDIA fences.
- If the hwcomposer passes buffers to Hyprland without waiting on them, frames could show
  incomplete rendering. NVIDIA does not attach implicit fences to these dma-bufs.
- Watch for flicker or partial frames after `deploy-fence.sh install`. If you see them, roll
  back the Android part and check `hwcomposer.waydroid` for `sync_wait` on `acquireFenceFd`.

## Measuring in Android

Waydroid must be running and unlocked. All steps run as your user, except `--system`.

```
sh venus/fence-measure.sh before                 # release stack (0001)
sh venus/deploy-fence.sh install                 # host server (stops the session)
sudo sh venus/deploy-fence.sh install --system   # Android driver
waydroid show-full-ui                            # start again, unlock
sh venus/fence-measure.sh after
```

Roll back (each half on its own; the halves interoperate):

```
sh venus/deploy-fence.sh rollback
sudo sh venus/deploy-fence.sh rollback --system
```

| Metric | before (0001) | after (v7 fences) |
|--------|---------------|-------------------|
| Settings: janky frames / 50th / 90th / 99th percentile | no data (0 frames: still on splash after 2 s) | |
| SystemUI: frames, janky (%), missed vsync, slow issue draw | 817, 198 (24.2%), 59, 194 | |
| SystemUI: 50th / 90th / 95th / 99th percentile | 16 / 19 / 20 / 20 ms | |
| SF timestats: totalFrames / missedFrames; NotificationShade averageFPS | 878 / 0; 57.9 | |
| `failed to dup EGL native fence sync` count | 101 | |

Before: `fence-measure-before.txt`, 2026-11-05 14:25, 15 rounds, screen 2560x1068. After that
run the Settings wait was raised from 2 s to 6 s. Only Settings is affected, and it had no
data anyway.

Expected:
- The warning count drops to about 0. It stays only for fences that had already signalled at
  export, which still give -1.
- Render threads no longer block on the GPU for each frame, so the GPU percentiles and slow
  "issue draw commands" counts should go down.
