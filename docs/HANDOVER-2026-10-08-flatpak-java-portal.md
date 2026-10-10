# Handover: Java Flatpak portal for Brave — 2026-10-08

## Active task and latest decision

The latest user decision is to **ship the stock Flatpak portal**. linuxd's
verified launch chain starts one portal service under the app's UID and
connects it to that app's private session bus. The portal implements
`org.freedesktop.portal.Flatpak`, including Spawn, SpawnSignal and SpawnExited.
Java continues to implement desktop portals that map to Android APIs.

This supersedes the earlier plan to implement Flatpak spawning protocols in
Java. APK versus APEX packaging remains to be settled; choosing the stock
service does not by itself choose its packaging location.

The portal must receive the private bus address and correct app environment,
be authorized to own its service name before the payload starts, and be
supervised with the app runtime. Preserve the app UID, SELinux/MLS isolation,
capability clearing and Binder restrictions. Stock portal child launches must
work through the adapted Flatpak/Bubblewrap path under those restrictions;
starting the service alone does not demonstrate functioning Spawn.

`org.freedesktop.portal.Flatpak.UpdateMonitor` will eventually connect to
MatonOS's own update infrastructure. Shipping the stock portal does not yet
supply that integration.

The existing `flatpak-env-wrapper.c:start_app_portal()` is an unfinished
implementation in linuxd's launch chain: it gates the child, drops identity,
registers the PID on bus-control, execs the portal, and waits for readiness.
Reconcile this work with the in-process broker and test the complete path.
The VM test below now demonstrates stock-portal spawning; production validation remains open.

## Current source state

Paths below are relative to the device repository.

- `linux/compositor/host/app/src/main/java/org/matonos/compositor/` contains
  the Java wire and transport work: `PortalWire`, `DBusReader`, `DBusWriter`,
  `DBusSignature`, `PeerConnection`, `PortalChannel`, and `LocalSocketChannel`.
  Several of these files are untracked; preserve them when resuming.
- `PortalBackend.java` currently implements desktop Settings, Inhibit and
  OpenURI operations, caller-scoped handles, properties and introspection.
  It does **not** yet dispatch the Flatpak object or implement Spawn.
- `JavaPortal.java` runs that backend over the authenticated MBP1 socket and
  handles Android integration and received FDs. Extend the existing layer.
- `SessionBus.java` still starts the native JNI broker (`NativeBroker`) in
  the app process and connects it to JavaPortal. The portal semantics/wire
  work is Java; the entire bus has not been converted to Java.
  Its policy permits talking to `org.freedesktop.portal.Flatpak`; that alone
  does not implement or own the service.
- `linux/dbus-broker/portals.c` currently forwards desktop object paths.
  Flatpak service ownership and `/org/freedesktop/portal/Flatpak` routing
  need to be reconciled with the Java implementation.
- `FlatpakLauncher.java` exposes the existing bridge launch path. It is not
  yet a complete Flatpak Spawn implementation with argv, environment,
  FD mappings, PID ownership and exit notifications.

The per-app runtime lives under `/data/matonos/linux/tmp/<uid>`; see
[the runtime socket handover](HANDOVER-2026-10-07-per-app-runtime-sockets.md).
Retain per-app UID isolation, private bus ownership, and the Linux payload's
`matonos_linux_app` domain and Binder restrictions. A Java portal hosted by
an Android stub can use Android APIs; that does not grant its Linux children
Android/Binder authority.

## Unfinished stock-portal implementation

These changes are still in the working tree and are relevant again under the
latest stock-portal decision. Review and finish them rather than assuming
they work; this documentation change does not alter code.

- `linux/flatpak/Android.bp` adds `matonos-flatpak-portal-prebuilt`,
  `matonos-app-flatpak` and `matonos-app-bwrap` packaging to the APEX.
- Their expected files have now been staged in `linux/flatpak/prebuilt/static/x86_64/`
  by the VM test turn. A full Soong/APEX image build has not been exercised.
- `linux/flatpak/apex_file_contexts` contains labels for those helpers.
- `linux/flatpak/flatpak-env-wrapper.c` adds `system_flatpak_version()` and
  `start_app_portal()`, forks the stock portal, registers its PID through
  session control, and calls the APEX executable before the app launch.
  The source still invokes this path; it has not been verified end to end.
- `linux/flatpak/app-session.h` adds a portal PID; the untracked
  `linux/flatpak/matonos-app-tool.c` provides unprivileged helper entry points.
- `linux/dbus-broker/broker.c` has owner-UID credential changes and portal
  registration work; keep the useful isolation fixes while replacing the
  stock portal assumption.
- `systembridge/sepolicy/system_ext/private/matonos_system_bridge.te`
  includes policy added for nested native portal launches. Review it against
  the final launch mechanism instead of assuming it is sufficient.

External work from the interrupted session also exists in
`/home/hanro50/matonos/repos/flatpak` and
`/home/hanro50/matonos/repos/MatonOS_apexs`.
The session copied helper sources into `MatonOS_apexs/flatpak/helpers/`, ran
`flatpak/build-helpers.sh` with `/home/hanro50/Documents/android-ndk-r30`,
and compiled applet binaries into `/mnt/data/flatpak-debug/`.
Those artifacts are experimental, not evidence of the final architecture.

## Build and VM evidence

`/mnt/data/flatpak-debug/portal-host-build.log` ends with `BUILD SUCCESSFUL`
and staging of `prebuilt/apps-built/MatonWaylandHost.apk`. This verifies that
host build, not a successful Flatpak Spawn call or Brave launch.

The supplied session transcript records:

- `adb -s 127.0.0.1:5555 install -r .../MatonSystemBridge.apk` failed with
  `INSTALL_FAILED_INVALID_APK`: persistent apps are not updateable.
- `matonos-linuxd` was pushed to `/data/local/tmp/matonos-linuxd-fixed`,
  chmod/chcon applied, bind-mounted over `/system_ext/bin/matonos-linuxd`,
  and restarted through `ctl.restart`.
- The bridge APK was later pushed, and the interrupted session reported a
  bind mount over the system APK. Do not infer that PackageManager loaded
  the new bridge or that the mount persists across reboot.

These are historical session observations. Those observations predate the new VM test described below. The user's VM is `127.0.0.1:5555`;
preserve it. Temporary bind mounts do not replace building a durable image.

## Resume order and acceptance criteria

1. Read `CLAUDE.md`, `NOTES.md`, this handover and the current diffs. Preserve
   existing work. The Java-only Flatpak implementation plan is superseded.
2. Finish one stock portal per app in linuxd's verified launch chain. Confirm
   UID/domain/capability/seccomp setup, private bus environment, readiness,
   broker PID registration and service ownership. Handle repeat launches and
   teardown without duplicate services or orphaned children.
3. Settle packaging and supply its binaries. Current APEX declarations refer
   to absent prebuilts. Check the unprivileged applet/Flatpak/Bubblewrap path
   used by the stock portal against the real app permissions and sandbox.
4. Build the affected helpers and host. Verify GetNameOwner, properties,
   real Spawn with FD transfer, PID-scoped signals, exit notification,
   caller isolation and app-runtime teardown on the VM. Then verify Brave
   startup and child process creation. Record results and logs here.
5. Keep Java desktop portals for Android integration. Plan UpdateMonitor's
   integration with our updater separately; do not claim it already exists.

The initial portal test exposed a subsequent crash. The crash follow-up below
records the keyboard fix and successful rendered Brave launch; production
policy validation remains open.

## VM deployment and Brave test — 2026-10-08 follow-up

The user authorized installing/testing on `127.0.0.1:5555`. The portal is now
installed temporarily and the **user confirmed Brave launched**. The VM was
already SELinux permissive and remains so; no reboot or image overwrite.

Deployment:

- Copied the currently mounted APEX bin contents into
  `/data/local/tmp/portal-test-bin`, added the stock Bionic portal and two
  applet entries, and refreshed the wrapper. Applied executable labels and
  bind-mounted the entire directory over `/apex/com.matonos.flatpak/bin`.
  This layers over the earlier individual helper mounts; reboot removes it.
- Installed the staged MatonWaylandHost APK with `adb install -r`, then
  bind-mounted `/data/local/tmp/MatonWaylandHost-portal.apk` over the system
  shared-library APK so existing stubs load the updated broker. The APK
  must be mode 0444: a writable mount initially caused Android's writable
  dex rejection; chmod fixed it.
- Kept the previously deployed linuxd/bridge. No persistent init service
  was added: linuxd's verified wrapper starts the portal for the app.
- Added `--ozone-platform=wayland` in Brave's app-owned
  `/data/matonos/linux/home/10122/.var/app/com.brave.Browser/config/brave-flags.conf`
  (UID/GID 10122, mode 0600). The latest wrapper expects a per-app
  `wayland-0-x11` socket, while the compositor currently supplies an X11
  directory FD/name. Without the flag Brave exited due to missing DISPLAY.
  An initial flags file in the deeper Brave profile directory was ineffective
  and has been removed.

Fixes made during the test:

1. The broker rejected portal PID registration after UID drop: non-dumpable
   `/proc` ownership did not match the app. The portal child now restores
   dumpability after dropping identity/capabilities, before broker inspection
   and exec. Added startup-stage diagnostics to the wrapper.
2. The stock portal constructs a minimal CLI environment, dropping
   `FLATPAK_BWRAP`. Its child CLI fell back to the initial-launch bwrap shim,
   which injected `matonos-app-exec` and failed the trusted-domain check.
   `matonos-app-tool.c` now pins `FLATPAK_BWRAP` to `matonos-app-bwrap` for
   the CLI entry. Nested children execute through the unprivileged multicall
   bwrap and retain the app domain.

Observed portal: PID 14338 at the final capture, UID/GID 10122, label
`u:r:matonos_linux_app:s0:c122,c256,c512,c768`. An earlier portal capture
confirmed CapEff/CapPrm/CapAmb zero, NoNewPrivs=1 and Seccomp=2. Trace evidence
shows the stock portal executing `matonos-app-flatpak run --sandbox` for
Brave's zypak helper with parent PID exposure and no network.

**Remaining runtime issues:** the user saw Brave launch, but the final process
capture contains the stub/supervisor/portal and no Brave Linux payload.
The log ends with repeated `DMA_BUF_IOCTL_IMPORT_SYNC_FILE: EPERM` and a
crashpad ptrace error; sustained browsing is not yet established. The Binder
filter currently rejects the entire ioctl type `'b'`, which also covers
DMA-BUF ioctls. Audit a precise Binder filter before claiming working GPU
synchronization; do not disable Binder isolation. The portal also warns about
an instance `pid` file race. Desktop portal UnknownInterface and missing system
bus messages remain. SpawnSignal/SpawnExited, multi-launch reuse and enforced
SELinux have not been independently verified in this turn.

Evidence under `/mnt/data/flatpak-debug/`: `portal-start.trace`,
`portal-launch.log`, `portal-second-launch.log`, `portal-final-state.txt`,
`brave-portal.png`, and `portal-tests.log`. The screenshot was taken after
the Linux payload exited and does not itself prove a rendered Brave window.
Only this turn's temporary strace processes were stopped/detached.

Refreshed device prebuilts: wrapper, stock portal, app-flatpak and app-bwrap;
updated `prebuilt/static/SOURCE` hashes. Synced modified wrapper/applet source
into the external helper tree. APK/APEX packaging is still an architectural
choice; the VM overlay uses the current APEX paths for testing.

## Crash follow-up — keyboard and DMA-BUF fixes

The user reported that Brave launched and immediately crashed. Two independent
issues were diagnosed and fixed on VM 5555:

- `linux/flatpak/session-binder-filter.h` now owns the seccomp filter used by
  `app-session.h`. It allows the exact DMA-BUF SYNC, EXPORT_SYNC_FILE and
  IMPORT_SYNC_FILE encodings while denying the rest of Binder's ioctl family.
  IMPORT_SYNC_FILE has exactly the same encoding as legacy
  BINDER_SET_IDLE_TIMEOUT; that timeout setting is allowed, but
  BINDER_WRITE_READ (transactions) and the other Binder control commands are
  still blocked. The NDK-built `tests/session_binder_filter_test.c` passed
  on the VM, checking actual kernel errno results for all current Binder
  control/transaction commands except that deliberate collision, plus a
  32-bit WRITE_READ encoding and the DMA-BUF commands.
- This removed DMA-BUF EPERM errors but did not fix the crash. A process/signal
  trace showed SIGSEGV at address NULL. NDK LLDB attached as root caught the
  fault in `libxkbcommon.so.0.11.0:xkb_state_update_mask`, with a null state
  argument. WAYLAND_DEBUG showed `wl_keyboard.keymap(0, fd, 0)` followed by
  enter/modifiers: the compositor advertised a keyboard without a keymap.
- `linux/compositor/native/compositor_core.c` now constructs its XKB context
  with `XKB_CONTEXT_NO_DEFAULT_INCLUDES`, since the embedded US map is
  self-contained and Android has no desktop XKB include tree. Keyboard
  initialization is checked; the compositor fails startup if the map cannot
  be installed instead of advertising an unusable keyboard.

The host APK build passed (`brave-keymap-build.log`), and it was installed and
bind-mounted from `/data/local/tmp/MatonWaylandHost-keymap.apk` (0444) over the
system shared-library APK. The refreshed wrapper was deployed into the
existing `/data/local/tmp/portal-test-bin` overlay and staged in device
prebuilts; the new filter header and wrapper/app-session sources were synced
into the external MatonOS_apexs helper tree. SOURCE hashes record the refresh.

The debugger temporarily froze Brave at the fault; the user noticed, and the
debugger was detached immediately. No debugger or strace should remain
attached. The temporary WAYLAND_DEBUG app override was removed, and
`--disable-gpu` was removed from the test flags. Only the Wayland flag remains.
The user permits relaxing Binder filtering **if** access can be limited to
what a stock Android app may do; no broad Binder transaction access has been
enabled. App UID alone does not establish equivalent SELinux restrictions.

After deployment, Brave rendered its new-tab page and menu with the GPU
flag restored to normal. The user subsequently confirmed that GPU
acceleration works in Brave on VM 5555. Main PID 24250 and both zygotes were alive at the
first successful capture. `brave-keymap-fixed.png` is the rendered-window
proof. This supersedes the earlier screenshot and immediate-crash result.
Remaining checks: longer browsing sessions, X11 passthrough, enforced SELinux,
portal lifecycle/reuse and updater integration. Missing desktop portal/system
bus warnings remain; a later frame-latency warning did not prevent rendering.

Evidence: `brave-crash.trace`, `brave-crash-session.txt`,
`brave-crash-maps.txt`, `brave-wayland-debug.log`, `brave-keymap-build.log`,
`brave-keymap-fixed.png` under `/mnt/data/flatpak-debug/`.

## Brave X11 test — 2026-10-08

The user requested testing Brave's X11 mode on VM 5555. Result: **not a
usable X11 browser session**. Wayland was restored after the test.

- Saved the app's existing flags to `/data/local/tmp/brave-before-x11.flags`
  and temporarily selected `--ozone-platform=x11`.
- The wrapper expects `/data/matonos/linux/tmp/10122/wayland-0-x11`, but the
  current host creates sockets in its private `files/x11/` directory and
  passes linuxd an FD/name. The wrapper no longer consumes that FD/name.
  Root temporarily bind-mounted a selected Brave-session Xwayland socket
  into the expected per-app path, using an NDK syscall helper because
  toybox mount tried to configure a loop device for the socket.
- A temporary app override set MATON_X11_SOCKET to that path so it reached
  bwrap's bundled environment. These are test accommodations, not a completed
  production socket bridge. Initial attempts failed with missing DISPLAY.
- Tracing the compositor's lazy Xwayland startup found a concrete packaging
  omission: `CANNOT LINK EXECUTABLE ... library "libgcrypt.so" not found`.
  The build uses libgcrypt for SHA1, but the VM image does not ship it for
  Xwayland. Its Bionic libgcrypt and dependency libgpg-error exist in
  `out/matonos/flatpak-ndk/prefix/{lib,lib64}`. Temporarily adding them to an
  overlay of `/system_ext/lib64/xwayland` allowed startup and xkbcomp execution.
- A test using an older session's socket caused incorrect session routing.
  Binding the newly created current-session socket allowed Brave's X11 main
  process and zygotes to run, and Xwayland logged new X11 surfaces. The window
  still ended as a black surface; a usable rendered X11 browser was not
  verified. This is insufficient to claim X11 rendering or GPU acceleration.
- Removed the test socket bind/placeholder, MATON_X11_SOCKET override and
  xwayland library overlay. Restored the saved Wayland flags (UID 10122,
  mode 0600) and relaunched Brave. Stopped this turn's strace. No production
  source or packaging changes were made by this test.

Evidence under `/mnt/data/flatpak-debug/`: `brave-xwayland-start.trace`
(linker failure and later successful Xwayland/xkbcomp exec),
`brave-x11-launch.log`, `brave-x11-compositor.log`,
`brave-x11-final-processes.txt` and `brave-x11.png` (black window).

Next X11 work: ship the complete Xwayland dependency closure, finish the
app-scoped socket/FD handoff, and investigate window routing/presentation
with the current per-app compositor architecture. Retest with enforced
SELinux; this VM remains permissive. The test root mounts must not become
part of the production design.

## Xwayland fixed: per-app compositor only — 2026-10-08

**Latest verified result:** the user confirmed that X11 Brave works and that
GPU acceleration also works in X11 mode. This supersedes the earlier black
window test. Brave is intentionally left in X11 mode on VM 5555 for the user
to inspect; its flags file currently contains `--ozone-platform=x11`.

The user clarified that a shared host compositor should never have existed.
That shared rendering path has now been removed:

- `CompositorService.java` is a Java launch/DNS coordinator only. It does not
  load compositor JNI, create display sockets or parse Wayland/X11 traffic.
  Native callbacks/rendering methods and the shared ICompositor interface
  were removed. IEmbeddedSession now exposes launch/status/listener cleanup;
  rendering and input stay local to the stub's PerAppRuntime.
- `jni_shim.cpp` no longer exports the shared CompositorService native entry
  points. MainActivity's test window uses an activity-owned local compositor
  in WindowActivity, separate from the launch coordinator; it serves no other
  apps. The standalone demo was rebuilt but not exercised in this turn.
- `PerAppRuntime.java` initializes a lazy Xwayland session in the app's own
  compositor, UID and callback path, creates `<runtime>/x11`, and publishes
  `wayland-0-x11 -> x11/X0` (or the allocated display number).
  NativeCompositor.nativeXwaylandInit's Java return type now correctly matches
  the JNI boolean result. Native runtime teardown removes the alias.
- `flatpak-env-wrapper.c` accepts only the canonical `x11/X<number>` alias
  target under this verified app's runtime and checks that the target is a
  socket owned by its UID. It explicitly passes MATON_X11_SOCKET through
  Flatpak's CLI environment options so the bwrap shim can bind it. The old
  compositor directory FD is no longer passed by FlatpakLauncher.
- `compositor_core.c` makes same-UID Xwayland sockets mode 0600. A root test
  dropped to Firefox's UID 10120 and confirmed EACCES connecting to Brave's
  X11 socket at UID 10122. No manual socket bind or app environment override
  is needed for the final implementation.
- `tools/stage-xwayland.sh` now installs the Bionic libgcrypt and libgpg-error
  DSOs from the Flatpak build into the staged system_ext library tree. The
  existing device.mk copy rules include them on the next image build.

Validation/deployment:

- Host APK built successfully (`xwayland-per-app-build.log`), installed and
  bind-mounted from `/data/local/tmp/MatonWaylandHost-per-app-x11.apk` (0444)
  over the system shared-library APK.
- The latest wrapper was deployed in the existing APEX bin directory overlay
  and staged into device prebuilts, with updated SOURCE hashes. Wrapper and
  helper headers were synced to the external MatonOS_apexs helper tree.
- The VM's `/system_ext/lib64/xwayland` is temporarily overlaid with
  `/data/local/tmp/xwayland-test-libs`, which includes the existing graphics
  forwarders plus the two missing crypto DSOs. Production staging places the
  crypto libraries in `/system_ext/lib64`; the overlay is a VM test deployment.
- Brave PID 17873 rendered its version page and menus with
  `--ozone-platform=x11`; Xwayland PID 17923 ran as UID 10122, opened
  `/dev/dri/renderD128`, and emitted its X11 surfaces from the stub's own
  compositor thread. The coordinator PID 17833 had no compositor/wlroots/
  Wayland-server library in its maps. Current active X11 sockets were all
  under `/data/matonos/linux/tmp/10122/x11/`.
- Xwayland inherited the stub's Android identity (this permissive VM labels
  stubs `zygote` because its seapp configuration is stale), with CapEff=0;
  it is distinct from the Flatpak payload's `matonos_linux_app` domain and
  Binder/seccomp boundary. Enforced SELinux and final image policy still need
  validation. No broad Binder access was granted to the Linux payload.
- Java portal/backend/connection tests passed, and the other-UID X11 socket
  DAC check passed. No debugger or strace remains attached.

Evidence under `/mnt/data/flatpak-debug/`: `brave-per-app-x11.png`
(rendered X11 version page/menu), `per-app-x11-logcat.txt`,
`per-app-x11-isolation.txt`, `per-app-x11-dac-test.txt`,
`per-app-x11-tests.log`, `xwayland-per-app-build.log`, `xwayland-stage.log`.
The user independently confirmed X11 rendering and GPU acceleration.

Remaining work: build a durable image/APEX including these staged outputs;
validate enforcing policy, multi-app lifecycle/teardown and longer sessions.
The VM deployments still disappear on reboot. UpdateMonitor integration and
additional Android desktop portals remain separate unfinished work.

## Keeping this handover current

The user explicitly requested that handover docs stay up to date. Update this
file and `HANDOFF.md` when implementation decisions, blockers, build results,
VM deployment state or next steps change, and before stopping or handing off.
Separate source edits, successful builds and verified runtime behavior.
