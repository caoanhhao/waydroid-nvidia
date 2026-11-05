# Waydroid trên NVIDIA (Venus qua vtest)

Chạy Waydroid với tăng tốc GPU trên card NVIDIA. Driver `nvidia-open` của host vẫn giữ nguyên:
không cần nouveau, không cần gỡ driver NVIDIA.

Android không nạp được driver NVIDIA (vốn viết cho glibc), nên lệnh Vulkan được chuyển ra host:

```
App / SurfaceFlinger (Android)
  └─ Vulkan: Mesa Venus (vulkan.virtio.so)        HWUI = Skia Vulkan
  └─ OpenGL ES: ANGLE → Venus
        │  unix socket /data/vtest/vtest.sock
        ▼
virgl_test_server (host, đã vá, vtest v6)  ← dịch vụ systemd user `waydroid-vtest`
        ▼
NVIDIA Vulkan (nvidia-open) → RTX 4070 → Hyprland
```

Buffer cửa sổ do NVIDIA cấp phát trên host (định dạng block-linear) rồi chuyển vào Android
qua vtest. Nhờ vậy Hyprland hiển thị trực tiếp, không phải chép lại.

## Yêu cầu

| Thành phần | Đã thử với |
|---|---|
| Hệ điều hành | Arch Linux, kernel 7.2 |
| GPU NVIDIA | RTX 4070, `nvidia-open` 615.78.08, render node `/dev/dri/renderD129` |
| iGPU Intel | UHD 770, `/dev/dri/renderD128`, dùng cho buffer CPU (LINEAR) |
| Compositor | Hyprland 0.57 (Wayland) |
| Waydroid | 1.6.3, image LineageOS 20 (Android 13) x86_64, có sẵn ANGLE trong vendor |

Cần cả iGPU Intel: các buffer CPU đọc/ghi (con trỏ, chụp màn hình…) vẫn cấp phát trên Intel.

Kiểm tra số render node của máy bạn:

```bash
ls -l /dev/dri/by-path/
```

Nếu số khác `renderD128` (Intel) / `renderD129` (NVIDIA), sửa trong
`release/waydroid-vtest.service` (`--rendernode`) và `release/install.sh` (`gralloc.gbm.device`)
trước khi cài.

## Cài đặt

### Cách 1: dùng bản đã build sẵn (`release/dist/`)

Nếu `release/dist/` đã có file (xem `release/dist/VERSION`), chỉ cần hai bước.

1. Phần host, chạy với user thường, không sudo. Bước này cài server vào
   `~/.local/lib/waydroid-venus`, bật dịch vụ `waydroid-vtest`, và tự tắt Waydroid nếu nó đang chạy.

   ```bash
   sh release/install.sh
   ```

2. Phần Android, cần sudo. Bước này chép driver và gralloc shim vào overlay của Waydroid, đặt các
   prop trong `/var/lib/waydroid/waydroid.cfg`, rồi khởi động lại `waydroid-container`.

   ```bash
   sudo sh release/install.sh --system
   ```

### Cách 2: build từ mã nguồn

Cần thêm: Android NDK r30, `meson`, `ninja`, `git`, gcc trên host.

| Phần | Script | Mã nguồn |
|---|---|---|
| Venus cho Android | `venus/build-venus-android.sh` | `src/mesa`, nhánh `venus-vtest` |
| Server cho host | `venus/build-virglrenderer-host.sh` | `src/virglrenderer`, nhánh `vtest-dmabuf` |
| Gralloc shim | `src/minigbm-vtest/build-android.sh` | `src/minigbm-vtest` |
| Wrapper Intel LINEAR | `src/minigbm-wrapper/build.sh` | `src/minigbm-wrapper` |

Build cả bốn phần:

```bash
sh venus/build-venus-android.sh && sh venus/build-virglrenderer-host.sh && sh src/minigbm-vtest/build-android.sh && sh src/minigbm-wrapper/build.sh
```

Gom kết quả vào `release/dist/`. Lệnh này cũng ghi `VERSION` và `SHA256SUMS`.

```bash
sh release/build.sh
```

Sau đó cài như Cách 1. Các patch nằm trong `venus/patches/`, nếu cần áp lại lên mã nguồn gốc.

## Sử dụng hằng ngày

Dùng Waydroid như bình thường: không còn script nào phải chạy tay.

- **Mở:** từ menu ứng dụng, hoặc:

  ```bash
  waydroid show-full-ui
  ```

- **Tắt:**

  ```bash
  waydroid session stop
  ```

- Dịch vụ `waydroid-vtest` tự chạy khi bạn đăng nhập và tự khởi động lại nếu bị lỗi.

### Kiểm tra đang chạy trên NVIDIA

Dịch vụ phải ở trạng thái `active (running)`:

```bash
systemctl --user status waydroid-vtest
```

Ứng dụng Android phải vẽ bằng Vulkan:

```bash
sudo waydroid shell -- dumpsys gfxinfo com.android.systemui | grep Pipeline
```

Kết quả mong đợi: `Pipeline=Skia (Vulkan)`.

`nvidia-smi` phải liệt kê `virgl_test_server` và `virgl_render_server`:

```bash
nvidia-smi | grep virgl
```

## Xử lý sự cố

| Triệu chứng | Cách xử lý |
|---|---|
| Cửa sổ đen, kẹt ở boot animation, hoặc log có `no suitable EGLConfig found` / `!gpuCount` | Server chưa chạy lúc Android khởi động. Chạy lại server, rồi tắt và mở lại Waydroid (lệnh bên dưới). |
| Vừa khởi động lại `waydroid-vtest` trong lúc Waydroid đang chạy | Android mất kết nối GPU. Luôn tắt và mở lại Waydroid sau khi khởi động lại dịch vụ. |
| Không thấy con trỏ chuột trên cửa sổ Waydroid | Lỗi đã biết, xem bên dưới. Chuột vẫn bấm được. |
| Cần log | Xem các lệnh log bên dưới. |

Chạy lại server rồi mở lại Waydroid:

```bash
systemctl --user restart waydroid-vtest && waydroid session stop && waydroid show-full-ui
```

Log của server:

```bash
journalctl --user -u waydroid-vtest -e
```

Log của Android:

```bash
sudo waydroid shell -- logcat -d -b crash
```

Danh sách tombstone:

```bash
sudo waydroid shell -- ls -l /data/tombstones
```

## Gỡ cài đặt (quay về OpenGL trên Intel)

1. Phần Android:

   ```bash
   sudo sh release/uninstall.sh --system
   ```

2. Phần host:

   ```bash
   sh release/uninstall.sh
   ```

Waydroid sẽ quay về cấu hình Intel (`ro.hardware.egl = mesa`). Bản sao cấu hình gốc trước mọi
thay đổi nằm trong `backups/`.

## Hạn chế đã biết

- **Con trỏ chuột có thể không hiện.** Prop `persist.waydroid.cursor_force_shm = true` tránh
  được lỗi crash của hwcomposer, nhưng buffer con trỏ là buffer LINEAR của Intel, và Hyprland
  trên NVIDIA không hiển thị được loại này. Đang được sửa: nguyên nhân là ANGLE không tìm thấy
  `libnativewindow` trong hwcomposer.
- **Đồng bộ GPU bằng CPU.** Log có `failed to dup EGL native fence sync` ở mỗi frame, vì vtest
  chưa truyền fence thật. Hiệu năng chưa được đo. Đang được sửa bằng giao thức vtest v7.
- **Chỉ chế độ toàn màn hình (full UI) đã được thử.** Chế độ multi-window dùng đường EGL của
  hwcomposer nên có thể crash.
- **Ứng dụng OpenGL ES nặng (game) chưa được thử kỹ.** Ứng dụng dùng native bridge
  (`libhoudini`) có thể crash vì lý do riêng.
- **Server là bản vá riêng (vtest v6),** không dùng `virgl_test_server` của hệ thống.
  Không ghi đè lên `/usr/lib/virgl_render_server`.

## Cấu trúc thư mục

| Đường dẫn | Nội dung |
|---|---|
| `release/` | `build.sh`, `install.sh`, `uninstall.sh`, `waydroid-vtest.service`, `dist/` |
| `release/README.md` | Ghi chú kỹ thuật của bản release (tiếng Anh) |
| `venus/patches/` | Các patch cho Mesa, virglrenderer và minigbm |
| `venus/notes-*.md` | Ghi chép chẩn đoán từng vấn đề |
| `venus/tests/` | Chương trình thử trên host và bên trong Android |
| `src/` | Mã nguồn đã vá (Mesa, virglrenderer, minigbm shim/wrapper) |
| `HANDOFF.md`, `plan.md` | Lịch sử và kế hoạch của dự án |
| `backups/` | Cấu hình Waydroid gốc |
