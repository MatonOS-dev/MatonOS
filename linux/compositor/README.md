# Per-app compositor host

Each signed stub runs PerAppRuntime, the wlroots native core, Xwayland,
SessionBus, JavaPortal, SessionAudio and DNS forwarding in its own process and
UID. The host APK supplies the shared library and native assets; it does not
coordinate another app's Linux launch. V3Launcher runs the compat-proxy loader
from the host nativeLibraryDir. See [v3 launch](V3-LAUNCH.md),
[stub format](STUBS.md) and [DNS](DNS-FORWARDER.md).

StubService creates `<dataDir>/runtime` itself, canonicalizes the path and
sets mode 0700. Wayland, bus, portal-backend, pulse and X11 sockets stay below
that PM-owned app data directory. No privileged runtime preparation, shared
socket namespace, registration supervisor or shared compositor remains.

Native sources and pinned dependencies are built outside Soong with
build-compositor.sh/build-xwayland.sh, using scratch/output under out/.
The in-process JNI broker remains because SessionBus loads it; its standalone
entry, privileged session control and unused SysV supervisor are deleted.
Closing the final explicit app window ends the stub runtime; minimizing or
recreating an activity preserves it. Relaunch apps after host updates during
pre-alpha. MatonOS is based on AOSP.

Local javac, native broker and compat-proxy results are recorded in
out/pc-logs/agents/v2-retire-result.md. Fresh-image enforcing validation remains:
verify Wayland and X11 rendering, portals, audio, DNS, close/reopen and updates,
and denial of another UID opening the app's sockets. Repeat on the supported
PC test machines. SysV emulation and broad shared-folder access remain gaps.

### Round 17: fullscreen modes and runtime children

Mapped fullscreen X11 configure requests now reach Xwayland unchanged; Android
still owns the presentation size. Windowed windows continue to use Android's
size constraints. Previously `on_xtrack_configure` discarded mapped requests
and `configure_window` forced every resizable drawable to the Android size.
Xwayland's `xwl_window_should_enable_viewport` requires the drawable to match
its per-client emulated RandR mode, so that policy prevented its viewport
scaling. The compositor already exposes wp_viewporter and the presenter uses
scene source/destination geometry. Xwayland server creation now explicitly
selects RandR emulation as well. No game-specific treatment is involved.
The 1280x720 GL viewport symptom is consistent with this mismatch; the actual
ETR race still requires a freshly built image to verify.

wlroots double-forks Xwayland. The runtime becomes a child subreaper before
creating it and pins its executable-matched adopted child with a pidfd. Native
shutdown kills that pinned process and attempts reaping for at most 100 ms.
The last-window grace callback explicitly stops the native runtime before
killProcess, which otherwise skips Service.onDestroy. Grace, minimization,
activity recreation and multiple-window policy remain intact. Lazy creation
still allows a clean server on the next per-app runtime launch. A native stop
also checks for a child that started without reaching the ready callback.

Validation: NDK r30/API 35 compositor library build; all host Java sources
compiled against SDK 36 (one existing deprecated API warning);
`native/tests/fullscreen_mode_test.py`, `geometry_request_test.py`, and the C
`xwayland_lifetime_test.c` passed. The lifetime test executes a real double
fork and verifies adoption, pidfd termination and reaping. No adb or image
build was performed. Existing runtime_review_test.py has a stale 0711 runtime
mode assertion while current maton_core_start deliberately uses 0700.

Coordinator fresh-image checks: launch an SDL2 X11 game at 1600x900, select
1280x720 fullscreen and start gameplay; verify full-window scaling and aligned
pointer coordinates, then restore native mode/windowed mode. Quit normally,
wait the ten-second last-window grace, and check the old Xwayland/stub PIDs
are gone; relaunch and check that a new display/socket works. Repeat closing
one of two windows, minimize/restore and closing during Xwayland startup.
On the Ryzen/RX6600 PC, Surface Pro 3 and ProDesk, repeat the same checks
without changing game-specific settings or host device permissions.
