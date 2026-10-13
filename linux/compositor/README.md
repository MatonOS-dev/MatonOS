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
