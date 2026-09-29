# MatonOS Wayland compositor v1 — implementation in progress

This area contains a clean-room wlroots host and Android output bridge. Pinned
dependencies and the native shared library compile for bionic x86_64; the
Gradle APK and fresh-image QEMU screenshot remain outstanding. Dependencies
are built outside Soong by `build-compositor.sh`; all source trees and objects
stay under `out/`.
MatonOS is based on AOSP.

## Architecture and choices

- wlroots 0.20.2 is the latest stable release selected on 2026-09-29. It is
  MIT-licensed, 700,802 bytes compressed and 3,537,798 bytes unpacked across
  472 files. The archive SHA-256 and link are in [UPSTREAMS](UPSTREAMS).
- The compositor uses the wlroots headless backend plus Pixman. Each Java
  `WindowActivity` supplies a public `Surface`; JNI wraps it as an
  `ANativeWindow`, creates one `ASurfaceControl`, and commits that output's
  AHardwareBuffer through `ASurfaceTransaction_setBuffer`.
- The allocator requests Android gralloc AHardwareBuffers. At runtime it
  resolves `AHardwareBuffer_getNativeHandle` and
  `AHardwareBuffer_createFromHandle` from `libnativewindow.so`; the direct
  path validates the in-tree minigbm cros-gralloc handle and exports its
  dma-buf plane fds, offsets, strides, and modifier to wlroots. The
  `createFromHandle` entry point is resolved for completeness but the allocator
  keeps the original AHardwareBuffer as the owner of its native handle.
- If the native-handle entry point is absent, the allocator renders to a
  staging buffer, imports the NDK AHardwareBuffer into EGL, and uploads the
  pixels once with GLES before SurfaceControl presents that same allocation.
  If EGL import/upload fails, the output stays blank and the service remains
  alive.
- The service owns the Wayland display in its `:compositor` process. Each XDG
  toplevel gets its own wlroots scene and headless output; a Java callback
  starts a matching `WindowActivity` for new toplevels. A bundled Wayland SHM
  demo client connects over a socket under the app's private files directory.
  It draws an input-reactive colored window and uses xdg-shell, a virtual
  keyboard and a pointer seat. The host Activity forwards public key and
  motion events through Binder.
- The host app uses minSdk 30 because bionic's `memfd_create`, used by the
  Wayland shared-memory compatibility layer, is available from API 30; the
  MatonOS product targets a newer API.
- There is no window-management policy: Android Activities provide the visible
  windows. Xwayland, clipboard, IME,
  portals, audio, and SELinux polish are out of scope.

## Build integration

The Android Gradle host is under `host/`; its package is
`org.matonos.compositor`. The app uses an app-specific key through the normal
`tools/build-apps.sh` registry. The library CMake file consumes the native
wlroots build and generated xdg-shell protocol files. The NDK dependency set
is Wayland 1.24.0, wayland-protocols 1.48, Pixman 0.46.0, libxkbcommon 1.8.0,
libdrm 2.4.134, and static libffi 3.4.8. They are MIT/BSD licensed; exact
archive hashes, source sizes, and license records are in [UPSTREAMS](UPSTREAMS).

The shared image integration is listed exactly in
[SHARED-CHANGES.md](SHARED-CHANGES.md); those shared registries have not been
edited. The host app does not request privileged permissions.

## Dependencies and licensing

No GPL code is used. The imported wlroots snapshot and our allocator,
SurfaceControl glue, core, and test client use MIT terms in [LICENSE](LICENSE).
The bundled US XKB keymap is generated from xkeyboard-config data, whose
license notices are in [assets/XKB-LICENSE](assets/XKB-LICENSE). The pinned
dependency source archives and sizes are listed in [UPSTREAMS](UPSTREAMS).
Native ELF and APK sizes remain to be measured after those builds complete.

## Verification status and next steps

The native implementation is a C core, C allocator and output glue, a C SHM
client, and a thin C++ JNI shim. The pinned dependencies and
`libmaton_compositor.so` compile for bionic x86_64 with NDK r30/API 35; exact
ELF byte sizes are recorded in [UPSTREAMS](UPSTREAMS). The Gradle APK has not
yet been built because the shared app registry/import edits in
`SHARED-CHANGES.md` are pending coordinator integration. No image containing
this app, boot test, input test, resize test, or screenshot has passed. A
preceding coordinator batch stopped in another app's `npm ci`; current queued
builds are for other agents.

The dependency build was verified with
`MATON_BUILD_JOBS=4 bash linux/compositor/build-compositor.sh`. The native
shared library was verified with Android NDK r30's CMake toolchain targeting
`x86_64`/API 35 and Ninja `-j4`; Gradle app, AOSP image, and device runtime
verification are still pending.

After the coordinator adds the app import and the full app/image build succeeds:

1. Start one fresh QEMU VM with virgl, `-m 4096`, and an adb port of 5556 or
   higher; confirm no other agent VM is active and never use port 5555.
2. Open Maton Wayland, tap **Open Wayland test window**, and wait for the
   colored xdg-shell surface inside its Android window.
3. Type on the keyboard and move/click/scroll a mouse; the client changes
   color when it receives keyboard or pointer events.
4. Resize the Android window and confirm the client receives an xdg-shell
   size configure and redraws at the new size.
5. Save a screenshot from that fresh VM and record the serial log, image path,
   GPU path, and observed dimensions here.

Generic PCs with no graphics device must still boot. The compositor runs only
when its host Activity starts, fails closed to a blank surface if an optional
API or GPU path is unavailable, and never gates Android boot.
