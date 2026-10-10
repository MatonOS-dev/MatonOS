# Per-app runtime (RFC — Stage 0)

Drafted 2026-10-07. Supersedes the shared-system-service shape. Implementation is
staged (see §4); Stages 2–3 are native + policy and need a build/VM test each.

**Progress (2026-10-07).** Build-verified via `build-apps.sh` (compositor APK +
broker `.so`):
- The broker is a JNI library (`maton_broker_jni.c`, `NativeBroker.java`); the
  compositor core is exposed per-app (`NativeCompositor.java`, shared-body JNI).
- `PerAppRuntime` hosts the **session bus, portal and compositor core** in the
  app's process/UID; `StubService` owns it; `StubActivity` drives windows,
  Surface and input from it and no longer asks the host to create a session.
- Bus + Wayland socket paths are threaded through the launch chain
  (`openSession` → `launchOwnedFlatpak` → `launchGraphical` → linuxd → wrapper
  `DBUS_SESSION_BUS_ADDRESS` / `WAYLAND_DISPLAY`).
- `CompositorService` no longer owns the app's bus or session.

**Remaining (native + policy; needs the AOSP/apex build):**
- linuxd's Wayland relay is **deleted**; wrapper consumes the app's absolute
  `WAYLAND_DISPLAY`; verify via Soong + `flatpak-env-wrapper` prebuilt build.
- SELinux: `matonos_linux_stub` domain (via `seapp_contexts` for `flatpak.*`),
  runtime-socket type via `type_transition`, `neverallow` privileged→stub
  transition, and GPU access confirmed (`appdomain` grants `gpu_device`).
- **Resolved:** socket typing; GPU/zero-copy access.
- **Deferred (real work, not risky-cleanup):**
  - **X11**: the relay's `-x11` socket is gone, and a per-app core would have to
    `exec` Xwayland as the app UID (the exec restriction this whole change was
    built to avoid). Treat as Wayland-only until X11 is re-plumbed deliberately.
  - **StubService lifetime**: started with `startService` from the foreground
    activity; a foreground service (+ its `foregroundServiceType` in the
    StubGenerator manifest) is needed to survive teardown.
  - Rebuild the `flatpak-env-wrapper` prebuilt (source changed).
  - Obsolete C broker's `run/wayland-` monitor check is now stale.


## 1. Defect

The runtime is a **system service**. `org.matonos.compositor` is a privileged
(`priv_app`) foreground service (`CompositorService`) that runs:

- the native compositor core — one `maton_core_start()` with N
  `maton_core_add_session()` clients;
- the native session-bus broker — an **exec'd** binary (`SessionBus`);
- `JavaPortal` — the portal/D-Bus parser;

and serves **every** app. So the parsers of untrusted Wayland and D-Bus run
**privileged and multi-tenant**: any compositor/broker/portal flaw is cross-app
and privilege-escalating. The intended per-app service exists only as an empty
`StubService` ("reserved … for later portal and D-Bus integration"). `NOTES.md`
already says each stub runs its *own* mini-broker and in-process portal; the
implementation drifted.

## 2. Target

Each stub hosts its own runtime **in its own process/UID**, using the shared
library it already pulls (`org.matonos.linuxhost`):

- **native compositor core** (JNI library, in-process) — one per app, renders
  into the stub's own `Surface`;
- **session-bus broker** (JNI library, in-process) — one per app;
- **portal** (`JavaPortal`, already in the shared library) — in the stub process.

The compositor app remains only the **shared runtime library** plus a **thin
privileged bridge** (Surface handoff, launch via `ISystemBridge`→linuxd, DNS).
**No app bytes cross a privileged process.**

## 3. Components and files

| Area | Change |
| --- | --- |
| `stub/StubService.java` (no-op today) | Becomes the per-app runtime host: instantiate `SessionBus` + `JavaPortal` + the native compositor core in the stub process; implement `IEmbeddedSession`. |
| `stub/StubActivity.java` | Bind to its **own** `StubService` (in-process), not `org.matonos.compositor/.CompositorService`. |
| `CompositorService.java` | Shrink to the thin role (Surface/launch/DNS), or fold into `systembridge`; stop owning compositor/broker/portal. |
| `native/jni_shim.cpp`, `native/compositor_core.c` | One core per app (per process); drop the N-session core model. |
| `linux/dbus-broker/main.c`, `broker.c` | Turn the exec'd broker into a JNI library (`System.loadLibrary`) callable in-process. Marked obsolete as a service; its bus logic is reused as a lib. |
| `install/linuxd/FlatpakManager.c`, `socket-relay.h` | Delete the Wayland relay (`GraphicalChild`, accept/relay threads); the app talks to its own in-process compositor socket. |
| `linux/flatpak/*` (wrapper, bwrap, app-session) | Use the in-app session sockets; no relay path, no shared `run/` namespace. |
| sepolicy | Per-app runtime domain; remove `matonos_flatpak_socket_file`/relay types; `neverallow` privileged → app-data exec; allow the app domain to `map`/`execute` the shared-library JNI libs. |

## 4. Stages

- **Stage 0 — this RFC.** Lock target + domain model.
- **Stage 1 — Java service split.** `StubService` hosts portal + session;
  `StubActivity` binds in-process; `CompositorService` reduced. (Shared library;
  no native change.)
- **Stage 2 — JNI-ify the broker and the per-app compositor core; delete the
  linuxd relay.** Native.
- **Stage 3 — sepolicy domains + `run/` namespace cleanup.** Policy.

## 5. Hard blockers (why staged)

1. **Android/SELinux won't let an untrusted app `exec` app-data binaries.** The
   broker and compositor core must become **JNI libraries** loaded from the
   shared-library APK, and `matonos_linux_app` must be allowed to `map`/
   `execute` them. This is the pivotal constraint.
2. **GPU/Surface/input per app-UID** — each in-process compositor needs
   EGL/GLES + `Surface` (+ likely dmabuf/gralloc); scope by MAC.
3. **Lifecycle** changes from one core/N sessions to one core **per app**,
   started/stopped with the stub and torn down on process death.
4. Each stage needs a **build + VM test**; native and policy changes cannot be
   validated outside a real build.
