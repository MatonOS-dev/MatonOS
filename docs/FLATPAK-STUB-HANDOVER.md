# Flatpak stub install handover

Updated: 2026-10-06 (evening). Read this before touching Flatpak installs.

## Flow (decided)

1. **Store** (`rn-apps/flathub`) calls the bridge: `add_flathub`, then
   `install {ref, operationId, icon}`.
2. **Bridge** generates a signed stub (`flatpak.<app-id>`) and installs it via
   PackageInstaller. The stub's launcher activity ships **disabled**, so the
   app stays hidden until its Flatpak is installed. The stub carries the full
   ref (app ID, arch, branch) and remote; Flatpak resolves runtime and deps.
3. **Store** waits for the stub (`MatonOS.startStubInstall(ref)`, up to 60 s),
   finds `org.matonos.compositor.stub.InstallActivity` by the
   `org.matonos.linuxhost.INSTALL` action + `FLATPAK_REF` metadata, and starts
   it ("install me": unprivileged, no permission, idempotent).
4. **InstallActivity** → compositor `IEmbeddedHost.installSelf(ref)` → bridge
   `installFlatpakStub(uid, ref)`: verifies the stub, returns at once if the
   Flatpak is already installed (linuxd `installed_for_uid`), otherwise stages
   and installs. The same activity resumes installs interrupted by a crash or
   power cut. The stub never installs on launch and chooses nothing.
5. **linuxd stage**: `matonos-flatpak-store` drops to the **stub UID** and pulls
   into `/data/matonos/linux/apps/<stub>/staging/<operationId>` (remote config
   + keyring seeded from the runtime installation). Network use and DNS are the
   stub's. `apps/<uid>` stays linuxd's (system 0711); staging dirs are
   `<uid>:system 0750` so linuxd (no DAC override) can read them; staging
   trees are removed by a child running as the owning UID (after publish, on
   failure, at boot). Operation IDs: `[A-Za-z0-9_-]{1,64}`, unique.
6. **linuxd install** publishes from that staging dir into the stub's `--user`
   installation (runtime into the runtime app's `--system` one).
7. On the successful completion event the bridge **enables the launcher** (and
   replaces the stub if the Flatpak's permissions changed). Reconcile also
   enables launchers of installed stubs, covering lost events.

## Flatpak stack (switched 2026-10-06)

`com.matonos.flatpak` ships the **bionic (NDK)** build from
MatonOS-dev/MatonOS_apexs `flatpak/` (`fetch.sh` + `build.sh`, all inputs
pinned): one multicall `matonos-flatpak` (flatpak + ostree + bwrap, 11.2 MB,
needs only libc/libm/libdl), helpers linking the system bionic,
`matonos-app-exec` fully static (runs inside the sandbox). Flatpak =
MatonOS-dev/flatpak `matonos/v26.10` (AppStream, FUSE/revokefs, gdk-pixbuf
removed; sends `User-Agent: flatpak/<version>` because Flathub 403s
`libostree/…`). glib/ostree/bwrap are unpatched upstream; bionic gaps come from
MatonOS-dev/bionic-fill. The musl recipe is retired. The APEX manifest
declares `requireNativeLibs` libc/libdl/libm. Helper sources live in the device
tree; copy changes to `MatonOS_apexs/flatpak/helpers/`, run
`flatpak/build-helpers.sh`, then copy outputs to
`linux/flatpak/prebuilt/static/x86_64/` and update `prebuilt/static/SOURCE`.

DNS: bionic resolves through netd as the calling UID (Private DNS, VPN,
firewall apply), so installs need no resolv.conf or forwarder. App sandboxes
(glibc runtimes) still use the per-app forwarder.

## Fixed 2026-10-06 (each found on the VM)

- Wrapper `TMPDIR`: host-side CLI work uses `apps/<uid>/tmp` of the
  installation it operates on, else linuxd's `/data/matonos/linux/cache`
  (Android's `/tmp` is shell-owned → GPG "Unable to configure context").
- StubGenerator: string-pool length prefixes were written outside the pool
  (every stub's resources.arsc/manifest was corrupt); entries are now aligned
  by apksig (`setAlignmentPreserved(false)`; PackageManager error -124).
- Bridge reads stub metadata with `MATCH_DISABLED_COMPONENTS` (the launcher is
  disabled until installed → "Unverified stub").
- Per-UID staging merged from codex/installer-staging (worktree removed), with
  the ownership fixes above.

## State at handover

Last VM test (image before c6bad78): store → add_flathub ✅ → stub created
hidden ✅ → store started "install me" ✅ → bridge rejected ("Unverified stub",
fixed in c6bad78). The image with c6bad78 + per-UID staging (9cda540) was
building at handover and is **not yet tested**.

Test recipe: copy the image to `~/matonos/vm/bionic-test/`, set the ESP
`loader/loader.conf` default to `matonos-debug-permissive.conf` (SELinux issues
are next week's work), boot with `tools/run-qemu-live.sh -g std -a 5571`,
`setprop persist.vendor.maton.sleep_idle_s 0`, capture logcat **without**
clearing it once the user starts, press Install in the store. The Bluetooth
abort in the VM log (`hci_backend_aidl.cc:43`) is unrelated.

Builds: only via `out/pc-logs/agents/coord-build.sh` (see `CLAUDE.md`); ccache
is on by default now. A change without Android.bp edits rebuilds in ~15 min.

## Next steps

1. Test the new image end to end: staging as the stub UID, publish, launcher
   enabled, app launches; then update, permission-manifest replacement, and
   "install me" after killing linuxd / the VM mid-install.
2. **Keep the stub in the foreground during "install me"** (foreground service
   + progress notification): Android's background firewall blocks network and
   DNS for app UIDs that are not in the foreground (verified on the VM).
3. Store: progress/result per operation; UI for resume and failed installs
   (a hidden stub is left behind).
4. Future linuxd: remove dead stubs (hidden, never installed) at boot.
5. SELinux (next week): denials seen for linuxd (link/ioctl in the Flatpak
   repo), compositor/priv_app, crash_dump; the `zones` request is still queued.
6. The C D-Bus broker (`linux/dbus-broker/`) is dead (D-Bus = Java) but is
   still started by the compositor as the session bus; replacing it is an open
   decision.
7. Device tree: replace `linux/flatpak/stage-static-musl.sh`, update
   `linux/flatpak/APEX.md` and `FORKS.md` for the bionic stack.
8. The linuxd command allowlist could be tightened (only `install`, `regen`,
   `run` exposed) — review separately.
