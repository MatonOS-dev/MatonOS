# Linux app stack

MatonOS is based on AOSP. Each signed Flatpak stub owns its compositor,
Xwayland, private JNI session bus, JavaPortal, audio sockets and DNS forwarder.
`StubActivity` → `StubService` / `PerAppRuntime` → `V3Launcher` starts the
`linux/compat-proxy` loader, which runs the bionic Flatpak tools bundled in
the subsystem APK under the stub UID and inherited app policy.

linuxd owns installation, updates, store configuration, metadata and stub-signer
verification. Shared immutable app versions and runtimes live in
`/data/linux-app`; private HOME, cache and runtime sockets live in ordinary
PackageManager-owned stub data. See [launch](subsystem/V3-LAUNCH.md),
[storage](../install/linuxd/STORAGE-LAYOUT.md) and [Flatpak](flatpak/README.md).

The v2 privileged launch/sandbox layer failed on enforcing SELinux and was
retired completely. There is no fallback, archived source fixture or compatibility
mode. Its uinput pads, Downloads bind grants, GTK settings, global machine-id,
shared socket tree and seccomp-notify SysV supervisor are gone. SysV IPC in the
v3 proxy and app-openable shared-folder integration remain gaps; kernel
CONFIG_SYSVIPC stays disabled. Historical handovers are retained as records.
