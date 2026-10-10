# Handover — per-app runtime sockets in `tmp/<uid>` (2026-10-07)

## Decision

The per-app runtime — the Wayland socket and the session bus — lives in the
app's own directory:

```
/data/matonos/linux/tmp/<uid>/
  wayland-0        # Wayland socket (stub's in-process compositor core)
  bus              # session bus (stub's in-process broker)
```

The **app creates `<uid>/` itself** (it runs as the app UID) and creates the
sockets there. Paths are **fixed**, so nothing is negotiated across the launch
chain: no directory fd, no `busAddress` / `waylandDisplay` strings.

The sandbox receives the directory (identity bind) and
`XDG_RUNTIME_DIR=/data/matonos/linux/tmp/<uid>`, so the Flatpak payload finds
`$XDG_RUNTIME_DIR/wayland-0` and `unix:path=$XDG_RUNTIME_DIR/bus` on its own.

This supersedes the earlier "shared `run/` relay namespace" design. The relay
and the `run/wayland-*` / `run/session-bus-*` names are gone.

## What changed

Java (stub shared library + compositor app):
- `stub/StubService.java` — runtime dir `<files>/runtime` -> `/data/matonos/linux/tmp/<uid>`.
- `stub/StubActivity.java` — stop computing/passing `busAddress`/`waylandDisplay`.
- `aidl/.../IEmbeddedHost.aidl` — `openSession` drops `busAddress`/`waylandDisplay`.
- `CompositorService.java` — `openSession` signature; `EmbeddedSession` drops
  `busAddress`/`waylandDisplay`/`directory`; `launch()` no longer passes them.
- `FlatpakLauncher.launch` — drops the runtime-dir fd and the bus/wayland strings.

Native:
- `linux/flatpak/matonos-bwrap.c` — identity-binds `/data/matonos/linux/tmp/<uid>`
  and sets `XDG_RUNTIME_DIR` in the sandbox.
- `linux/flatpak/flatpak-env-wrapper.c` — fixed session (`graphical=owned`),
  `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY=wayland-0`/`DBUS_SESSION_BUS_ADDRESS` fixed;
  deleted the `run/session-bus-*` matching, `MATON_SESSION_DIRECTORY_FD`, and the
  X11 fd plumbing.
- `install/linuxd/FlatpakManager.{c,h}` — `flatpak_manager_launch_graphical`
  drops `runtime_directory_fd`/`bus_address`/`wayland_display`; no fd 198, no
  `WAYLAND_DISPLAY`/`DBUS_SESSION_BUS_ADDRESS` env.
- `install/linuxd/MatonosLinuxd.cpp` — `launchGraphical` AIDL signature updated.

AIDL / bridge:
- `ILinuxd.aidl` (both copies) and `ISystemBridge.aidl` signatures updated.
- `systembridge/.../SystemBridgeService.java` — signatures; drop fd close.

Policy:
- `matonos_system_bridge.te` — new `matonos_linux_runtime_file` type for the
  `tmp/<uid>` runtime tree. `matonos_linux_stub` is an app domain, so it cannot
  use the `core_data_file_type` homes; this type is a plain `data_file_type`
  (`mlstrustedobject` for MLS). linuxd creates the dir, the stub creates the
  sockets, the sandbox app connects, `matonos_bwrap` may `mounton`.
- `MatonosLinuxd.rc` — `/data/matonos/linux/tmp` is a tmpfs mounted with
  `context=u:object_r:matonos_linux_runtime_file:s0` (mode 0711, so only linuxd
  creates `<uid>`). Deliberately **no `file_contexts` entry**: a type under
  `/data/` must be `core_data_file_type` (build test) and must not be
  `app_data_file_type`, both of which conflict here, so the mount context types
  the tree instead.
- linuxd gains a binder command `prepareRuntime(packageName)`: a signed stub
  (via `stub_verify_caller`) asks linuxd to create/claim
  `/data/matonos/linux/tmp/<uid>`; the stub calls it (like `installSelf`) before
  binding its sockets. This replaces the earlier "the app creates its own dir at
  1773" (which allowed squatting).

Build infra:
- `buildinfra/Android.bp` — removed the obsolete `matonos-apk-session-broker`
  extract pipeline (genrule + `python_binary_host` + `prebuilt_bin` + the
  `required` entry); deleted `buildinfra/extract-apk-broker.py`. The broker is a
  JNI DSO now, so there is no standalone `/system_ext/bin` binary.
- `linux/dbus-broker/build-android.sh` — strip `libmatonos-dbus-broker.so`
  instead of the removed PIE `matonos-dbus-broker`.

## Not done / must verify on the next build

- **Prebuilt helpers.** `flatpak-env-wrapper` and `matonos-bwrap` ship as
  prebuilts under `linux/flatpak/prebuilt/static/x86_64/` and embed the old
  paths. Rebuild them (MatonOS_apexs `flatpak/build-helpers.sh`) and refresh
  `prebuilt/static/SOURCE`.
- **Soong not exercised here.** linuxd, the bridge and the AIDL were edited but
  not compiled in this environment; build `matonos-linuxd` + `MatonSystemBridge`.
- **SELinux at creation.** `file_contexts` labels `tmp/[0-9]+` at restorecon
  only. Confirm (permissive off / check AVCs) that:
  - `matonos_linux_stub` can `mkdir` `/data/matonos/linux/tmp/<uid>` and create
    the sockets there (parent `/data/matonos/linux/tmp` must be searchable and
    writable for the app UID), and
  - `matonos_linux_app` can search that dir and `connectto` the sockets.
- **Ordering.** The stub creates the dir at service start, before linuxd's
  `prepare_linux_data` (launch). `tmp/<uid>` must not depend on that; if it does,
  create it earlier (install/boot).
- **Stale broker check.** `linux/dbus-broker/broker.c:925` still requires a
  `registration.monitor` prefix of `/data/matonos/linux/run/wayland-`; the broker
  is obsolete and the per-app path passes `monitor=NULL`, but clean it up.
- **X11.** Still deferred (needs `exec` of Xwayland as the app UID). The wrapper
  now looks for `X11` at `/data/matonos/linux/tmp/<uid>/wayland-0-x11`.
- **Standalone (non-stub) compositor path** is unchanged and still uses the
  compositor app's own `files/wayland` directory.

## Build / test steps

```sh
# App + shared library (build-verifiable here)
MATON_APPS_ONLY="matonos-wayland-host" MATON_APPS_FORCE=1 tools/build-apps.sh

# Full image (picks up linuxd/bridge/AIDL/policy + prebuilt rebuild)
# then: install a Flatpak stub, launch it, and inside the sandbox check
#   ls -l "$XDG_RUNTIME_DIR"        # wayland-0, bus
```

## Files touched

```
device/maton/pc_x86_64/
  install/linuxd/FlatpakManager.c, FlatpakManager.h, MatonosLinuxd.cpp
  install/linuxd/aidl/org/matonos/systembridge/ILinuxd.aidl
  systembridge/aidl/org/matonos/systembridge/{ILinuxd,ISystemBridge}.aidl
  systembridge/src/org/matonos/systembridge/SystemBridgeService.java
  linux/compositor/host/app/src/main/aidl/org/matonos/compositor/IEmbeddedHost.aidl
  linux/compositor/host/app/src/main/java/org/matonos/compositor/CompositorService.java
  linux/compositor/host/app/src/main/java/org/matonos/compositor/FlatpakLauncher.java
  linux/compositor/host/app/src/main/java/org/matonos/compositor/stub/{StubActivity,StubService}.java
  linux/flatpak/flatpak-env-wrapper.c
  linux/flatpak/matonos-bwrap.c
  systembridge/sepolicy/system_ext/private/matonos_system_bridge.te
```
