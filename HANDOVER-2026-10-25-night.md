# Handover 2026-10-25 night (~00:10)

User approved overnight work: "do the VM checks, iterate; if it fails park it till morning, continue with something else".

## Decisions tonight (user)
- **Namespaces = dead end** for anything entering/reshaping APP namespaces (setns, pivot_root, per-app mounts, root
  dirs, mountd-spawned app procs): AOSP neverallows (app.te:600 caps incl. cap_userns, app.te:746 fs ops). Recorded in
  linux-subsystem-app DEAD-ENDS.md #3 and memory dead-ends #5.
- **NOT dead:** a system daemon mounting read-only EROFS **globally** under /mnt/matonos. VM-verified (below).
- Rewrite code restored on subsystem main (5 reapply commits); translation + site rewrite is the file path until EROFS mounts land.
- **Pack splits wrap EROFS images** (stored, 4096-aligned, signed with the APK). Once mounts work, the whole pack-fd
  routing (fd window, per-fd traps, memfd copies, offset translation) gets deleted — that's the payoff.
- mountObb mod (vold/framework) was granted as an AOSP-patch exception, then **superseded**: our own daemon needs no
  AOSP patch. Not upstreamable anyway.
- FD window: proxy owns top 1/2 (≤60Ki) / 1/3 (≤120Ki) / 1/4 of the real soft NOFILE, no rlimit lying, soft raised
  by window size, app fds skip it (EMFILE where native would). Software only handling fds <1024 = its bug.
- Anti-cheat: fd layout low risk; rewriter (libc patching) is the risk → disable rewriting when EAC/BattlEye runtime
  detected (PROTON_EAC_RUNTIME / PROTON_BATTLEYE_RUNTIME or their libs). Not built yet. Test with Epic EOS samples.

## Pushed: subsystem main (~/matonos/repos/linux-subsystem-app), all 108 compat-proxy host tests green each time
64ea313 temp-gc test fix · 08624ba DEAD-ENDS · a5ccb6b store-marker cache · d6d0ca5 filter number-only fast path
(seccomp action cache) · 36729c1 fstat no /proc lookup for non-regular · 9ca475b ranged site-cache invalidation +
fcntl first · 7906cea native writes · 0b7de4d ioctl native unless rewritten · 9d4093d/0fb7d70/7646c63 fd window ·
c1600ec native reads outside window (+ per-fd trap filters for low pack fds).
Host numbers: flatpak --version 60→15 ms; read 3600→441 ns (native 381); write ~3500→404 (345); flatpak run true 0.66→~0.56 s (native 0.34).
Known gap: SysV IPC arena fds stay low (moving them hangs site-rewrite-exec; cause unknown; harmless).

## VM checks (v46 disk VM, kernel 7.2.7, enforcing) — ALL PASS
File-backed EROFS `fsoffset=8192` from inside a larger file (no loop) at /mnt/matonos/test with context=oemfs;
already-running camera2 saw it via propagation (master:44; zygote MS_SLAVE|MS_REC, /mnt shared:7); probe as
untrusted_app read/stat/mmap PROT_EXEC OK. Only denial: lnk_file read on oemfs (own type allows it).
Neverallow audit for the daemon: none applies (only /apex mounts, proc/bpffs mountons restricted; contextmount_type
must never be writable — RO EROFS ok; apps may find non-protected services, read/exec new types).

## In progress (device tree main, UNCOMMITTED, VM OFF for Soong)
- `erofsd/`: AIDL org.matonos.erofs.IErofsMounter (pinStubCertificate, mountSplit(apk, entry, token)), daemon
  erofsd.cpp (caller UID → package via IPackageManagerNative.getNamesForUids; split must be /data/app/[~~x/]<pkg>-y/*.apk;
  caller signed with pinned stub cert via hasSha256SigningCertificate; pin only by org.matonos.linux signed with
  subsystem cert sha256 4596a6af…ff58; libziparchive finds stored 4096-aligned entry with EROFS magic; mount
  erofs ro,nodev,nosuid,fsoffset,context=matonos_erofs_file at /mnt/matonos/<uid>-<n>; unmount on token death),
  matonos-erofsd.rc, Android.bp. device.mk: PRODUCT_PACKAGES += matonos-erofsd.
- Policy appended to sepolicy/system_ext/private/matonos_controllers.te (section "matonos-erofsd"), file_contexts
  (+3 lines), NEW service_contexts. preflight/checks.py: added service_contexts to the system_ext/private fixed set
  (the 5-file rule had no recorded reason) — **user to confirm**.
- Build running/ran: `MATON_BUILD_COORDINATOR=1 tools/build.sh -m matonos-erofsd selinux_policy`, log
  out/pc-logs/erofsd-build.log. Check it first; fix errors.

## Next
1. Fix build → full image v47 → boot VM (start cmd in HANDOVER-2026-10-24-morning.md:115, setsid nohup) → test
   daemon with a test app/stub: pin, mountSplit, read/exec as app, unmount on death.
2. Design multi-pack views: several EROFS pack images → one /app view (daemon-mounted read-only overlayfs?) —
   check neverallows first. Subsystem: rework erofs-pipeline (lsa-trim) to keep per-pack splits each wrapping EROFS.
3. Then delete pack-fd routing in the proxy; wrap daemon in updatable APEX (mountd-apex branch scaffolding, 8ce4420 local).
4. Anti-cheat guard. Device tree: mountd/signing private-key copy deleted (originals in ~/.config/matonos-keys).

## Update ~00:40 (user decisions, see memory erofsd-architecture)
- erofsd ships in an APEX bound to the subsystem app (the subsystem updates it). Stubs call the daemon directly. Splits and delta updates stay, each wrapping a compressed EROFS image; every APK gets an EROFS file-index container.
- Pin is in memory only and re-shared by the subsystem every boot (/data/misc/matonos_erofs and its policy removed). Still to build: a subsystem activity that stubs start and wait on until the pin is set.
- Mounts are at /mnt/matonos/<owner app uid>/<n> and shared, so runtime mounts are reused across stubs. A stub may mount any split whose package is signed with the pin. Token death only releases a hold; a 10-minute sweep unmounts mounts unused for a whole period or whose APK is gone. Cross-app reads are fine. Later the Flatpak tool binaries (flatpak, bwrap) may move onto these mounts.
- Preflight fixed-SELinux-file-set check deleted (arbitrary limit).
- Every test runs enforcing.
- Build at 00:40 is on pre-change sources → rerun `MATON_BUILD_COORDINATOR=1 tools/build.sh -m matonos-erofsd selinux_policy`.

## ~01:30 — BLOCKER: image v47 fails the full neverallow check (parked for the user)
- `-m selinux_policy` passed, but the full image's `sepolicy_neverallows` check failed (log out/pc-logs/image-v47.log:466).
- **system/sepolicy/private/domain.te:2111:** only init, kernel, vold, zygote, update_engine (+ fastbootd in recovery, overlay_remounter on userdebug) may `mount/remount/relabelfrom/relabelto` any fs_type except sdcard_type/fusefs_type. So matonos_erofsd can NOT mount EROFS, with or without context= (labeledfs relabelfrom is also blocked).
  The 10-24 VM check passed only because the mount ran as root shell (su). "Global daemon mount NOT dead" was wrong for a custom domain.
- Fixed (uncommitted): isolated_app excluded from find/binder_call (isolated_app_all.te:87), proc:file rule dropped (coredomain.te:145).
- **Options for the user:**
  A) erofsd validates the request, then init mounts. erofsd sets its own property type, an `on property:` trigger runs init's `mount erofs ${...}` (init expands properties). Stays inside policy, no AOSP patch. Async, needs one mount at a time + a completion property.
  B) vold does the mount (the mountObb-style AOSP patch was granted as an exception earlier, then superseded).
  Recommendation: A, if init's mount builtin takes fsoffset/context in its options string (check system/core/init/builtins.cpp do_mount).
- Subsystem cca3cf8 (local, unpushed): ErofsPin + ErofsPinActivity + pin on BOOT_COMPLETED. Device tree erofsd still uncommitted. No VM test run.
- Checked: init's `do_mount` (system/core/init/builtins.cpp:481) passes the last argument through as the options string, so `mount erofs <apk> <dir> ro nodev nosuid fsoffset=N,context=...` works, and `umount` exists too. Option A is feasible. Stopped here for the user's decision; VM off.

## Morning: user chose A
- erofsd: mount()/umount2 replaced by property requests (target, offset, mount=<id> | utarget, umount=<id>, then wait for done=<id>). The APK is passed as the /mnt/matonos/.src/<id> symlink (92-byte property limit). Success is checked with statfs EROFS magic. SYS_ADMIN dropped.
- Triggers in erofsd/matonos-erofsd.rc (system_ext; APEX rc may hold 'on' only in vendor APEXes). Policy: init gets the mount/relabel rules; matonos_erofs_prop is set_prop'd by erofsd.
- Build check: `-m matonos-erofsd,selinux_policy,sepolicy_neverallows` (log out/pc-logs/erofsd-build3.log).

## ~08:50 VM test, v47 (disk VM rebuilt; v46 disk kept as disk-v46.img), enforcing — PASS
- erofsd runs as u:r:matonos_erofsd, service matonos.erofs registered, /mnt/matonos + .src labeled matonos_mnt_dir.
- Init path, driven as root: mount trigger → `erofs ro,context=matonos_erofs_file,nosuid,nodev,fsoffset=8192`. As an app (runas_app, debuggable probe org.matonos.erofsprobe): ls, cat and exec of a script on the mount all OK. Unmount trigger OK. No related denials.
- Pin from shell (uid 2000) rejected. A null-token mount from the app is rejected by binder.
- Not tested yet: the daemon's own mount path (needs a stub signed with the per-device stub key + an assets/matonos/*.erofs entry, i.e. the erofs-pipeline rework), and the subsystem's pin at boot (the image still ships the 10-18 subsystem prebuilt; refresh it).
- Note: mkfs.erofs must use --all-root (host uid 1000 showed up as system on the mount).
- Next: move the triggers into a vendor APEX (public prop type), then the pipeline rework.
- 09:14 hourly: APEX/erofsd/policy/neverallow build running (Soong analysis, a memory-stall warning); VM off on purpose for Soong; mountd docs removed (fc5d535); waiting on the user: subsystem own domain vs keep erofsd (recommended keep).
- 09:40 user decision: no APEX. erofsd + triggers stay in the image (55a63b2 as tested); vendor-APEX attempt reverted (a vendor daemon can't call PackageManager). The API is frozen, keep the daemon tiny. Next: EROFS pipeline rework, then the full stub test.
- 10:14 hourly: erofs-layers pass 3 running (1h43m); host A→B layer test + C index reader pass. Watch: fixture shows mtime=7762760355 (year 2216?), check in the report. VM off. No APEX (decided).
- 11:14 hourly: erofs-layers done (1d73a1a 90fc954 7e86ccb: A→B reconstruction, Java MTPKIDX2 writer, on-device pack_<sha> splits + layers.index; mtime = big-endian decode bug, fixed). Fast-forwarded into subsystem main 7e86ccb; full subsystem build running (log out/pc-logs/subsystem-layers.log); push after green. Next: stub eager mount + proxy index lookup brief.
- 12:14 hourly: subsystem main a56b6f2 (stub eager mounts + proxy native EROFS fds, pack-vfd.c deleted) pushed, full build green; prebuilt refreshed (vc 703); image v48 building; next disk VM + Tux Racer end-to-end in enforcing mode.
- 13:15 hourly: full mount path worked in VM v48 enforcing (pin by subsystem → erofsd → init → /mnt/matonos/10119/1, app layer at /app). Fixed (uncommitted on subsystem main): pin activity package, layers.index record skip 28 not 32, runtimes now regular stubs w/o launcher (carry layers.index). Tux Racer reinstall in progress (download 16%).
- 14:14 hourly: mount side verified in VM (54 layers, ~150 ms, enforcing); proxy fixes 0a2e3b2 (root ordering) + d4b0ec0 (deployment files → index association) → device retest now. Commits 2b4a3a9 pushed.
- 15:14 hourly: f4ce6e2 (O_NOFOLLOW table link) + ac024d7 (symlinks in pack layers) on subsystem main; device 06989a2 (build-subsystem.sh, prebuilt untracked, erofsd chmod). Tux Racer regenerating in VM (Installing…); install-speed measurement on this run.
- 15:45 MILESTONE: Extreme Tux Racer renders its menu from EROFS layer mounts on v48 (enforcing). Subsystem main 7e58321. Open: erofsd chmod needs image v49, fstat perf follow-up, install-speed (codex running), update + sweep tests.
