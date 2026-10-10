# Handover 2026-10-07 (afternoon)

**Store install of a Flatpak works end to end on the VM** (Firefox: stub →
"install me" → stage as stub UID → publish → launcher shown). Launching then
fails in dbus-java (below). Details: `docs/FLATPAK-STUB-HANDOVER.md` (not yet
updated — this file supersedes it for today).

**NOTHING IS COMMITTED** except 87e7c57 (linuxd include fix). Uncommitted:
device tree (git status) + MatonOS_apexs `flatpak/helpers/install/linuxd/FlatpakStore.c`.
Last image with all of it: built 15:29, tested OK through install.

## New install design (user decisions today)

- Stub talks to linuxd **directly** (no compositor/bridge relay):
  `ILinuxd.installSelf(packageName, operationId)` / `updateSelf(...)`.
  Stub looks linuxd up via `ServiceManager` reflection (only hidden API use,
  user-approved). Activities: `InstallActivity` (action
  `org.matonos.linuxhost.INSTALL`) and new `UpdateActivity`
  (`org.matonos.linuxhost.UPDATE`); store passes extra
  `org.matonos.linuxhost.OPERATION_ID`. Store module: `startStubInstall(ref, op)`,
  new `startStubUpdate(ref, op)` (no store UI for update yet).
- linuxd verifies (`install/linuxd/StubVerify.c`): binder UID owns
  `/data/user/<u>/<pkg>` (stat owner), pkg is `flatpak.*`, base.apk v3/v2
  signer cert == per-device stub cert, ref+remote read from the APK's own
  (stored) manifest, manifest package == pkg. Bridge hands linuxd the cert via
  `call("set_stub_signer", {certificate: hex})` → `/data/matonos/linux/store/stub-signer.der`.
  Runtime UID = owner of `/data/user/<u>/org.matonos.linuxruntimes`.
- Staging runs inside the binder call (stub must be foreground), publish async;
  completion event carries `ref` → bridge shows launcher.
- Removed: `IEmbeddedHost.installSelf`, `ISystemBridge.installFlatpakStub`,
  bridge deployStub/stage/installSelf.

## Bugs fixed today (all found on the VM)

- Staging ownership: linuxd owns apps/<uid> + staging (explicit 0711, umask!),
  builds op dir, `ostree init` + seeds remote/keyring + verifies, then
  `hand_over_tree` (chmod before chown, group system, setgid) to stub UID;
  publish `reclaim_tree`s it back to system. Removal: child as UID empties,
  linuxd rmdirs.
- Bridge reconcile deleted hidden stubs → now only removes stubs whose
  launcher was enabled.
- Stubs lacked INTERNET → generator adds it. Store helper dropped `inet` gid
  (`dnsproxyd` is root:inet) → `setgroups(1,{3003})`; prebuilt rebuilt
  (`linux/flatpak/prebuilt/static/x86_64/matonos-flatpak-store`, SOURCE hash
  updated; other helpers byte-identical).
- Metadata runtime ref lacks `runtime/` prefix → added.
- Stub's --user repo had no remote → `ensure_user_remote` seeds it.
- Publish re-staged as UID 0 → removed (prepare already staged).
- `seal_object_files` failed on symlink objects → lsetxattr, skip chmod.
- Bridge lacked `CHANGE_COMPONENT_ENABLED_STATE` → added to manifest.
- Stub couldn't load `libmaton_dns_forwarder.so` (shared-library classloader
  has no native path) → **DnsForwarder rewritten in pure Java** (sock_diag
  owner check, rate limit, `DnsResolver.rawQuery`). Native lib still built
  but unused (remove from CMake later).
- Earlier: prepare took runtime UID from bridge hint when marker missing.

## Next steps

1. **Launch blocker:** dbus-java 5.2.2 `SASL.<clinit>` calls
   `Collator.setDecomposition(FULL_DECOMPOSITION)`; Android ICU throws
   "Bad mode: 2" → compositor `java-portals` thread crashes, "Cannot open
   application session". User was asking how much dbus-java does for us:
   ~690 lines use it (JavaPortal, PortalWire, PortalExportedObjects,
   MatonLocalTransportProvider): Settings/Inhibit/OpenURI/Request/Session/
   Properties portals over a DirectConnection. Options: fork fix (catch the
   exception; forks rule) or replace with own small D-Bus wire code. Decide
   with user.
2. Commit everything (device tree + MatonOS_apexs helper) once user agrees.
3. Foreground service for install (user: store must be able to start several
   installs back to back): stub `InstallService` (dataSync FGS + notification,
   generator manifest + `FOREGROUND_SERVICE_DATA_SYNC`), activity just starts
   it and finishes. Not started.
4. Test update me, permission-manifest replacement, kill mid-install.
5. SELinux (zones work): app→linuxd binder/service find, linuxd reading
   /data/user/*/ and /data/app, netlink_tcpdiag for stubs, relabels.
6. Update docs/FLATPAK-STUB-HANDOVER.md, APEX.md, FORKS.md.

Test recipe: copy image to `~/matonos/vm/bionic-test/`, set ESP loader
default `matonos-debug-permissive.conf` (mtools, offset 2048*512), boot
`tools/run-qemu-live.sh -i … -g std -a 5571`, `setprop
persist.vendor.maton.sleep_idle_s 0`, open store (monkey), tap Install on
Firefox (1045,422). linuxd stderr is lost — use `strace -f -p $(pidof matonos-linuxd)`.
