# Handover — morning 2026-11-16 (store night)

Living doc; updated through the night.

## Done
- Image 7.3p built (248 min), x86-64-v2 ISA audit PASS.
- Device tree 2de9f50: `store-sources/fdroid` is a submodule
  (MatonOS-dev/store-source-fdroid); store `rn-apps/flathub` → bb32bc9,
  contract → c21e8d9 (REVISION 1, `running()` keep-alive).
- Store main bb32bc9 (fast-forward of contract-v2): ownership record, IStore
  update service, cancel, per-app parallel operations, keep-alive polling,
  "<label> stopped" / "source unavailable".
- F-Droid source: catalogue caching, conditional entry.jar, parallel repos,
  running(); two VM-found fixes pushed:
  - entry.jar verified with JarFile (F-Droid puts entry.json before the
    manifest; JarInputStream reported every real repo as unsigned).
  - HTTP validators stored only after a verified index exists (a failed load
    left an ETag → permanent "304 without cached index").

## VM store smoke test (disk VM, headless, port 5556)
- Store launches, Flathub browse + search work.
- Sources page renders; F-Droid listed as Enabled.
- F-Droid search works (f-droid.org index streamed, a93c1b9). Results were
  appended after every Flathub hit and labelled with the plugin package name;
  store now interleaves sources round-robin and shows the label.
- Open: F-Droid ranking — "newpipe" ranks BendyStraw above NewPipe (exact
  name match should come first; plugin-side).
- Verified on VM: F-Droid install (BendyStraw, LocalTube, Organic Maps),
  uninstall, Flathub + F-Droid installs running in parallel with separate
  progress banners.
- Fixed: Cancel was shown for the built-in Flathub source, which has no
  cancel (dead button). Now shown only when the source implements cancel;
  the Flatpak plugin (round 4) brings real cancel.
- Not verified on VM: F-Droid cancel — downloads finish in < 10 s on the VM
  link (tc only shapes egress). Host tests cover it.
- Follow-ups (small, store):
  - Banners above the list shift every row when an operation starts/ends;
    taps land on the wrong app. Put progress in the row or a fixed-height area.
  - Package removed outside the store (pm uninstall / Settings) keeps showing
  "Uninstall" until the store restarts; listen for package changes.
  - Search submitted while the first collection is loading stays on
    "Loading apps…" until resubmitted.

## Flatpak source
- Round 4 (sol) done: contract submodule, typed paging, remotes as repos,
  cancel + running(), launch repair via IStore.update, ISA baseline fix
  (BoringSSL scalar paths, xz CLMUL off).
- Review fix abb9621: stub resolves the store service among system apps only.
- Merged into subsystem main 8d415f3 (prebuilt conflict → branch build,
  superset of main's sources, matches SOURCE).
- VM fix d2ad0c1: AppStream reader rewritten DOM → SAX. On Android the DOM
  path failed twice: KXml rejects the JDK hardening flags, then Flathub's
  catalogue as a DOM ran the subsystem out of heap (crash loop on search).
  DOCTYPE/external entities still refused (LexicalHandler/EntityResolver).
- VM verified (v1171): plugin search, uninstall, install + Cancel
  ("cancelled", no package left), full install (~280 s), app launches
  (MediathekView). Pushed subsystem main d2ad0c1.
- Not VM-tested: launch repair after a runtime is removed (host test covers).
- Open: every Flathub app is listed twice (built-in store Flathub source +
  "Flatpak" plugin); both rows share the package → phase 2 retires the
  built-in source. After a plugin install the rows kept "Install" until the
  store regained focus (luna's refresh-on-focus covers part of this).
- Device tree: AIDL copy-drift check removed from tools/build-subsystem.sh
  (subsystem now uses the contract submodule).
- Disk: /home hit 97% (ostree test failed on min-free-space); deleted nine
  finished build dirs → 59 GB free.

## Store follow-ups (done, pushed, VM-checked)
- Store 2133b50: progress + Cancel live in the app's row (no list shift),
  latest search/collection request wins, refresh on foreground (luna's
  e7bc4fb only added the import; listener added by me).
- F-Droid 9655a0c: ranking exact name > prefix > word > substring > id >
  summary > description; "newpipe" now lists NewPipe first on the VM.
- Device tree 43e4428 bumps both submodules.

## Phase 2 (done, pushed, VM-checked)
- Subsystem main 8ab85a8: ILinuxAppStore, LinuxAppStoreService (933 lines),
  stub Install/Update/ConfirmInstall activities, DebugExtraInstallActivity
  and DELETE_PACKAGES removed (-1281 lines). INSTALL_PACKAGES stays: the
  extra-data temporary base stub still uses PackageInstaller.
- VM v1174: search across Flathub/Flatpak/F-Droid; plugin install of
  Curo Calculator completed (~300 s).
- Worktrees phase2 + lsa-flatpak-source removed (merged).

## Installer (device tree 8fae004, not yet image-tested)
- Ventoy drives refused (VTOYEFI GPT name or FAT label).
- Drive JSON reports rotational; confirm step offers F2FS / ext4 with the
  per-drive default (flash → F2FS, rotational → ext4); make_f2fs format op;
  fstab f2fs userdata entry after ext4.

## Decisions taken
- Store collections: plugin `collection(kind)`, built-in FlathubSource deleted.
- Separate /data/media partition: dropped.
- Kernel: ship 7.2.7 until the next LTS (7.3.4 denies all property sets).

## Parked (needs you)
- Duplicate Flathub rows: the store's built-in FlathubSource also feeds
  Browse (Popular/New via Flathub web API) and ODRS ratings; the Flatpak
  plugin only does search/install from AppStream. Retiring the built-in
  source drops Browse collections unless the plugin grows a collections
  call (contract change). Your pick: (a) contract `collection(kind)` on
  plugins, built-in source deleted — my recommendation; (b) keep built-in
  for Browse only, search/install via plugin.
- Separate user-files location (/data/media on its own partition): not
  started. FBE keys are only installed by vold on userdata; a second
  partition mounted at /data/media would hold user files unencrypted
  (CDD/certifiability break). Options: (a) drop the feature — my
  recommendation; (b) metadata-encryption-only media partition (dm-default-key
  via fstab keydirectory) — weaker than per-user FBE, needs vold/fstab work.

## Image build 7.3q — BUILT, LIVE BOOT HANGS (blocker, parked)
- Built 02:43 (34 min, ISA audit PASS) after two fixes: Zink stage env
  must be exported for build.sh too; removing the phase2 worktree left
  main-out/deps/pipewire/build + one cxx dir pointing at it (deleted).
- Live image (copied to ~/matonos/vm/claude/inst/) never reaches adb
  (device offline, no display surface). Debug + debug-permissive entries:
  boot stops after `init: processing action (wait_for_coldboot_done)` at
  ~4–7 s, nothing after for 5 min. Same with system QEMU 8.2 virgl,
  -g std, and qemu-venus -g native; same with no extra disks.
  Not SELinux (permissive hangs too). pc-gpu-detect also gets denials
  setting vendor.maton.graphics.* (exits 1) — separate issue, see serial
  logs in ~/matonos/vm/claude/inst/serial*.log.
- No device-tree change since 7.3p touches ueventd/modules; uncommitted
  input/native/matonos-inputd.c + inflinger patch (not mine) were in the
  build. Unknown whether 7.3p's live image booted under this harness —
  first check: rebuild at 7.3p's commit, or test the installed disk VM.
- Lead: ueventd logs "Loading module" for lpc_ich.ko, virtio-gpu.ko,
  failover/net_failover/virtio_net, cdrom/sr_mod, i2c-*, intel_rapl_* but
  never "Loaded" for them (earlier modules do get "Loaded") → coldboot
  probably blocked in a module load. Serial root shell never starts
  (stall precedes it); SysRq via monitor printed nothing.
- Harness is fine: installed disk VM boots in 30 s — but it runs kernel
  7.2.7; the live image has prebuilt kernel 7.3.4 (bzImage 11-15 16:41,
  same as 7.3p). Prime suspect: the 7.3 kernel bump + its modules, not
  tonight's userspace changes. Unverified whether 7.3p ever booted.
- ROOT CAUSE NARROWED (morning debug): the hang is init waiting forever in
  wait_for_coldboot_done because EVERY property set is denied
  (`avc: denied { set } ... tclass=property_service permissive=0`, also
  init.svc.*, ro.cold_boot_done from ueventd), even with
  androidboot.selinux=permissive. Same live image with the old 7.2.7
  bzImage + old initrd (ukify UKI on the debug entry) boots past it: zero
  property denials, surfaceflinger + zygote start. → the 7.3.4 kernel is
  the cause (no local SELinux patches; SELinux Kconfig unchanged). Next:
  diff security/selinux 7.2.7→7.3.4 (class/perm mapping, status page,
  selinuxfs access) or bisect; meanwhile 7.2.7 is a working fallback.
- Debug UKI on ~/matonos/vm/claude/inst/live.img is currently the 7.2.7
  one (dbg72.efi); original saved as dbg.efi there.
- RESOLVED: kernel back to 7.2.7 (b397339, your call: stay until the next
  LTS). Image 7.3r (63 min, ISA PASS) live boot → boot_completed in 15 s,
  zero property denials.
- quick-image.sh refused (graph env differs) → full `build.sh -K -M`.
- Store 3b49490: search field single-line; VM-checked on disk VM (Enter keeps one line, results show).

## Image trim (measured)
- 6243 modules, 552 MB, already stripped (INSTALL_MOD_STRIP); module
  compression off — skipped (Android modprobe .ko.zst support unverified).
- EROFS partitions uncompressed → next: BOARD_EROFS_COMPRESSOR lz4hc after
  7.3q finishes, then size + boot comparison.

## Docs drift scan (luna, report-only; full report out/pc-logs/agents/docs-drift.last.md)
- Fixed: install/README.md formatting summary now lists f2fs.
- Open (mostly linuxd/v2 text still reading as current): docs/LINUX-INSTALLER.md
  (mark superseded), AGENTS.md v2 linuxd sections, FLATPAK-EXTRA-DATA.md,
  PER-APP-RUNTIME.md, OWN-DBUS.md:20, rn-apps/flathub/SHARED-CHANGES.md
  (DELETE_PACKAGES checklist), SOFTWARE-CENTRE.md (pre-plugin model),
  FLATPAK-STUB-HANDOVER.md (InstallActivity flow), subsystem README
  "confirm priv_app" check. AGENTS.md edits left for you (shared agent rules).

## Audit sweep (luna, report-only; out/pc-logs/agents/sweep-night1115.last.md)
- "F2FS loses to earlier ext4 fstab entry": false — fs_mgr formats only when
  partition_wiped() (blank superblock), else tries the next entry.
- Fixed: bootstrap manifest still requested DELETE_PACKAGES (subsystem main, built OK).
- Downgraded to low, not fixed: F-Droid plugin parses its cached index on
  a 304 without re-hashing. The cache is only ever written hash-verified via
  temp+rename in app-private storage, so only disk corruption hits this.

## Store collections (merged, pushed, VM-checked on 7.3r live)
- contract 22355f4 (`collection(kind, offset, limit)`), store 3e36e7d, F-Droid
  d0d2200, subsystem 6919826: Browse Popular/New come from plugins; built-in
  FlathubSource/FlathubApi/Odrs deleted (ratings now from the Flatpak plugin).
- Review fix 3e36e7d: one page (50) per source — F-Droid "new" is the whole
  index and would have buried Flathub.
- VM: Popular shows Flathub with ratings; New interleaves Flatpak + F-Droid.
  First load ~1–2 min on a fresh system (index downloads). IronFox repo
  returned HTTP 502 (upstream; logged loudly).
- Seen on 7.3r: com.android.bluetooth native crash at boot (not looked at yet).

## Installer VM test (7.3r live, port 5557)
- Fixed 80fcb12: drive list was empty — RN FlatList renders nothing inside
  the Compose host tree; rows now rendered directly (drive + progress lists).
- Verified: NVMe listed as safe; boot drive and Ventoy stick refused with
  reasons; F2FS preselected for NVMe, ext4 offered.
- Enforcing install fails at "Write the target partition table": helper
  killed after exec (`matonos_driver_helper` denied fd use on
  `matonos_driver`). Policy unchanged since import, so this path only ever
  ran permissive. Collecting the full helper denial set in a permissive run.
- Bluetooth crash at boot = BT HCI AIDL HAL reports init failure (VM has no
  controller); abort is in com.android.bluetooth's assert.

## Installer: system repair (decided by user 2026-11-16, not started)
- Offered only when an existing MatonOS install is detected on the drive.
- Rewrites super (live system into slot A, B left empty for the next OTA),
  /boot (XBOOTLDR) and the ESP boot images/entries (kernel must match the
  modules in super) and the add-on partitions (their per-kernel folders
  must match the new kernel). Keeps userdata and metadata, but runs a
  filesystem check on both (e2fsck or fsck.f2fs for userdata by its
  filesystem; e2fsck for metadata) and reports problems before writing.
- Downgrade check: repair is refused when the live build is older than the
  installed build (say so in the UI).
- Installer has no slot choice; OTA stays the only update path.
- Confirm step now lists each new partition with size + purpose (uncommitted
  until the current install test passes).
- Risk rating shown before repair (user 2026-11-16); the highest finding wins:
  - very high: /data missing key files / partly broken; metadata not as
    expected and files had to be reconstructed
  - high: severe fsck errors on /data or metadata; SMART drive alerts
  - medium: super an unexpected size or empty
  - low: EFI boot entries missing
  - nothing found: the action is labelled "Upgrade" instead of "Repair"
    (same operation; still refused on downgrade).
  - Approved: highest finding sets the rating, every finding listed; SMART =
    NVMe health log first (kernel, no tool), SATA reader added later.
