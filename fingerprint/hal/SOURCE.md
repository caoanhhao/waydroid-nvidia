# Virtual fingerprint HAL: where the files come from

AOSP's example AIDL fingerprint HAL (`hardware/interfaces/biometrics/fingerprint/aidl/default`,
the sysprop-driven `FakeFingerprintEngine`), taken from an **Android 14** Cuttlefish build.
The tree is laid out as it sits under `/vendor`.

## Why Android 14 and not Android 13

The Android 13 build of this HAL (Cuttlefish `aosp-android13-gsi` build 16833088, TP1A.220624.019)
was checked first. It is the old stub engine: it reads **no** `vendor.fingerprint.virtual.*`
sysprops, and `authenticate` reports success with an empty token as soon as it is called, so it
cannot be driven from adb. The sysprop-controlled engine first shipped in Android 14, so the files
here come from Android 14. They run against Waydroid's Android 13 libraries plus one small compat
shim (see below).

## Source

| | |
|---|---|
| Build | ci.android.com, branch `aosp-android14-gsi`, target `aosp_cf_x86_64_phone-userdebug`, build **16849261** |
| Vendor fingerprint | `generic/aosp_cf_x86_64_phone/vsoc_x86_64:14/UP1A.231105.001/16849261:userdebug/test-keys`, patch 2023-11-05 |
| Artifact | `aosp_cf_x86_64_phone-target_files-16849261.zip`, 2,119,446,102 bytes |
| URL | `https://storage.googleapis.com/android-build/builds/aosp-android14-gsi-linux-aosp_cf_x86_64_phone-userdebug/16849261/19816d5568c0a7006b30080d9e7a53f3df73742e212a481cebaa99a260d0ae8b/1/aosp_cf_x86_64_phone-target_files-16849261.zip` (signed link from `https://ci.android.com/builds/submitted/16849261/aosp_cf_x86_64_phone-userdebug/latest/aosp_cf_x86_64_phone-target_files-16849261.zip`) |
| md5 / sha256 | `0261c76a5e2ed0c971ab64520157783a` (matches CI) / `662155396e1bd2072a014b3ec2747096690bb288f322609dee28964a64817cc5` |

## Files

| File | From | sha256 |
|---|---|---|
| `bin/hw/android.hardware.biometrics.fingerprint-service.example` | zip `VENDOR/bin/hw/…`, **patched**: added `DT_NEEDED libfingerprint_compat.so` (patchelf 0.17.2, `--add-needed`) | `4eba704a832526050eba9b96b7d4d225022487936ffaf5ef7dd2a88203cd054d` |
| `orig/android.hardware.biometrics.fingerprint-service.example` | zip `VENDOR/bin/hw/…`, unmodified (reference only, do not install) | `dfbd8379e1e2fecb40cb469c5e15f3ba83b5e97165ecbe98a2bf00dd0ac09764` |
| `etc/init/fingerprint-example.rc` | zip `VENDOR/etc/init/fingerprint-example.rc`, unmodified | `cb064d5c3c5c15df6e51993b2a1f81711711b1b029d25790595b21b5ad36fcde` |
| `etc/vintf/manifest/fingerprint-example.xml` | zip `VENDOR/etc/vintf/manifest/fingerprint-example.xml`, unmodified | `39393ef729b0511b025d05bf5e9fb6428675dd813475a950b68d129b1a6517c9` |
| `lib64/android.hardware.biometrics.fingerprint-V3-ndk.so` | zip `VENDOR/lib64/` | `08041a809d1c156edc98c065937f8e8e7b0df378740fc0b486cf1a4e9d51d115` |
| `lib64/android.hardware.biometrics.common-V3-ndk.so` | zip `VENDOR/lib64/` | `8fe5bfe098f916f4cf91cd6544779b1885c7b3bf5e5f72779dd6b5485ab5d961` |
| `lib64/android.hardware.biometrics.common.thread.so` | zip `VENDOR/lib64/` | `1b3d813e12e54cca009f4354f5182e2a9c98a0d799d8d26cb5fb851c5fb97e59` |
| `lib64/android.hardware.biometrics.common.util.so` | zip `VENDOR/lib64/` | `330f7155845c568e92c67aea18ccc4be67fe0163ea5b3e099a130cd1f9a17981` |
| `lib64/android.hardware.keymaster-V4-ndk.so` | zip `VENDOR/lib64/` | `f737b8cad4bd997d4cdefad7ea9925f00886d88c6fb5bf68a56236e4655d3370` |
| `lib64/libfingerprint_compat.so` | built here from `src/libfingerprint_compat.c`, NDK r30 (30.0.16248370) clang, `--target=x86_64-linux-android33 -shared -fPIC -O2`, stripped | `2bab0c95b55d2a9271efd7105db76e3d79fd0c19ffe14113967175044cf66fdc` |
| `src/libfingerprint_compat.c` | written here | `c4b8a47808a6241e88f0aa88faf79bec3169df786b7b7fcef21815ba21f9b7cf` |

### The compat shim

Checked against Waydroid's libraries, the binary has exactly one undefined symbol:
`android::base::WriteStringToFd(std::string_view, android::base::borrowed_fd)`, which
Android 14's libbase added; Waydroid's VNDK v33 `libbase.so` has only the `const std::string&`
overloads. Bionic resolves all symbols at load, so without the shim the HAL would not start.
`libfingerprint_compat.so` exports that one symbol and implements it as a `write()` loop. Its call
sites in the binary pass the arguments in `rdi` (data), `rsi` (size) and `edx` (fd), which matches
the shim's C signature. The HAL only calls it from `dump()`, i.e. `dumpsys`.

## Binder instance and init

- Instance: **`android.hardware.biometrics.fingerprint.IFingerprint/virtual`**. `strings` on the binary
  shows `/virtual` (at file offset 21289, `0x5329`) and no `/default`.
- The VINTF fragment declares AIDL `IFingerprint/virtual` **version 3**.
- `fingerprint-example.rc`: service `vendor.fingerprint-example`, `class hal`, `user nobody`,
  `group nobody`, `interface aidl …IFingerprint/virtual`, **`oneshot` + `disabled`**. Init never
  starts it at boot. servicemanager starts it on demand through the `interface aidl` line when a
  client waits for the declared instance. Add a `start`/remove `disabled` in an overlay if it
  should start at boot instead.
- SELinux: Waydroid's own `vendor_file_contexts` already labels this exact path
  `hal_fingerprint_default_exec`, and the `hal_fingerprint_default` domain exists in its policy. The
  Android 14 sysprop contexts (`vendor_fingerprint_virtual_prop`) do not exist in Android 13, which
  only matters if SELinux is enforcing in the container.

### Will Android 13's framework accept `virtual` / V3?

- Waydroid's `services.jar` (Android 13) contains no `biometric_virtual_enabled` setting and no
  virtual-instance filter. It iterates the declared `IFingerprint/` instances directly, so
  instance `virtual` should be picked up even with `ro.debuggable=0`. This comes from reading
  the dex strings and has not been run.
- Waydroid's FCM (`compatibility_matrix.7.xml`) lists AIDL `IFingerprint/default` version 2. AIDL
  versions are backward compatible: V3 serves a V2 client, and libvintf treats V3 as meeting `2`.
- If `virtual` turns out to be a problem: `/virtual` and `/default` are both 8 bytes, so the
  instance can be byte-patched at offset 21289 (same offset in `orig/` and the patched binary),
  together with `virtual` → `default` in the `.rc` `interface` line and the VINTF `<fqname>`.

## Dependencies, checked against Waydroid images

Checked with `readelf -d` and `nm -D` (defined and undefined, plus `readelf -V` symbol versions)
against `/var/lib/waydroid/images/{system,vendor}.img`, read with `debugfs`:

| NEEDED | Provided by |
|---|---|
| `android.hardware.biometrics.{fingerprint-V3-ndk,common-V3-ndk,common.thread,common.util}.so`, `android.hardware.keymaster-V4-ndk.so`, `libfingerprint_compat.so` | **shipped here** (vendor.img `/lib64` has none of them; VNDK v33 has only `keymaster-V3-ndk`) |
| `libbase.so`, `libc++.so` | `/system/apex/com.android.vndk.current` (VNDK v33) |
| `libbinder_ndk.so`, `libc.so`, `libm.so`, `libdl.so` | system / bionic (LLNDK) |

After adding the shim there are zero unresolved symbols. The required symbol versions are
`LIBC`, `LIBC_O`, `LIBBINDER_NDK`, `LIBBINDER_NDK30` and `LIBBINDER_NDK31`, all present in
Waydroid's libc and libbinder_ndk. Nothing has been run on the device yet.

## Sysprops the binary reads (from `strings`)

Persistent (`persist.vendor.fingerprint.virtual.`): `type` (strings: `rear`, `udfps`, `side`; prefer
`rear`, since udfps needs display/touch plumbing), `enrollments`, `authenticator_id`, `sensor_id`,
`sensor_location`, `sensor_strength`, `max_enrollments`, `detect_interaction`,
`navigation_guesture`, `lockout`, `lockout_enable`, `lockout_timed_threshold`,
`lockout_timed_duration`, `lockout_permanent_threshold`, `udfps.display_touch`,
`udfps.control_illumination`.

Runtime (`vendor.fingerprint.virtual.`): `enrollment_hit`, `next_enrollment`, `challenge`,
`operation_enroll_latency`, `operation_enroll_error`, `operation_authenticate_latency`,
`operation_authenticate_duration`, `operation_authenticate_fails`, `operation_authenticate_error`,
`operation_authenticate_acquired`, `operation_detect_interaction_latency`,
`operation_detect_interaction_duration`, `operation_detect_interaction_error`,
`operation_detect_interaction_acquired`.

Upstream usage pattern (from the AOSP README for this HAL; not yet tried here):
`setprop persist.vendor.fingerprint.virtual.type rear`; enroll with
`setprop vendor.fingerprint.virtual.next_enrollment <id>:<ms>,<ms>:true` and then start enrollment
in Settings; "touch" with `setprop vendor.fingerprint.virtual.enrollment_hit <id>`. Authenticate
only polls `enrollment_hit` for `operation_authenticate_duration` ms (default short), so either set
`enrollment_hit` first or raise that duration. `dumpsys android.hardware.biometrics.fingerprint.IFingerprint/virtual`
lists the HAL's own commands ("Virtual HAL commands").

Not included here: `/vendor/etc/permissions/android.hardware.fingerprint.xml` (the feature flag),
which the overlay session provides.
