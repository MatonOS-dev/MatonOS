# Handover — night 2026-11-21 → morning 2026-11-22

Night approvals: see memory night-2026-11-21-plan (merges, allowlist, orphan cleanup,
hardware API, server builds/VM/codex, OTA fix + A→B→A on testing, ISO staged not public,
store blank-page/refresh fixes). Still not allowed: public ISO, stable OTA, mail/DNS.

## Done
- **Merged + pushed.** Subsystem main = eda26aea (flatpak-pkg-split fast-forwarded).
  software-centre main = 5fbc71d. Device tree branch metadata-enc = 7489ed2 (pushed,
  NOT merged to main until build-48 passes the VM test).
- **Orphan Flatpak cleanup (verified on VM).** The store asks each source for unused
  dependency packages after an uninstall and on every foreground, and removes the ones it
  installed. Subsystem: `inspect --unused` + carriers with no records / no deployment split.
  VM: flatpak.org.gnome.Platform removed automatically, Minecraft + freedesktop kept.
  gnome.Platform.Locale was detected but Android asked for confirmation: it had been
  installed by me via adb (installer=null), not by the store. Removed by hand. Correct.
- **Store blank page (6a):** no blank page; F-Droid rows at ~3 s, Flatpak rows ~6 s warm.
- **Store refresh on close (6b):** closing during a rebuild still finishes the import. Race
  fix: catalogue version read before the catalogue (5fbc71d), smoke-tested on VM.
- **OTA keeps SSD/HDD boot image (eb48049).** OTA sets carry matonos-{a,b}-hdd.efi too; the
  slot writer picks by ro.boot.fstab_suffix (pc_x86_64.ssd/.hdd, anything else fails loud).
  test_ota.py passes (stale assertions from 92d786b fixed).
- **Hardware "Unknown" (7489ed2).** System Bridge target `hardware`, command `summary`:
  Wi-Fi/Bluetooth/audio/GPU presence from vendor.maton.* props; Settings merges it.
  A field stays null (Unknown) until its daemon publishes. Note: no daemon sets
  vendor.maton.bluetooth.* yet, so Bluetooth will keep showing Unknown.

- **build-48 (dt 7489ed2) verified + merged: device tree main = 3303535.** Live boot ~40 s;
  Hardware page: Wi-Fi "Not detected", Audio "Available", Graphics "Hardware", Bluetooth
  "Unknown" (expected). Settings installer → blank 64 GiB SATA HDD (rotation 7200) in
  ~1 min; installed disk boots with fstab_suffix pc_x86_64.hdd, slot _a.
  New: `MATON_QEMU_DISK=hdd` in run-qemu-live.sh (installed disk as rotational SATA).
- Cleanup: deleted 11-19 OTA test images (~25 GB: inst/live.img, target.img, ota/) and
  hdd43.img (build 47); build47-live.img replaced by build48-live.img. /home 98% → 89%.

## In progress
- OTA A→B→A on testing: A = build-48 installed (hdd48.img, vars-48hdd.fd); build-49 on
  the server = B, then build-50 = A. Driven with scratchpad ota-stage.sh (init slot-writer
  request path; Settings has no update entry yet: SystemUpdateSection is not in the nav and
  UpdateService.CHANNEL is hard-coded "main").
- Gotcha: an incremental build with no source change keeps system/build.prop, so
  ro.build.date.utc (= OTA update id) stays the same (build-49 = 48, id 1795199085).
  Removing system/build.prop is not enough (build-50 = same id): the Soong intermediate
  only rebuilds when BUILD_NUMBER changes, and it defaulted to a fixed eng.hanro5.
  Fixed: tools/build.sh sets BUILD_NUMBER=matonos.<UTC date> for image builds.
- **A→B PASSED (03:2x):** build-51 (1795308870) published to testing (server
  /mnt/data/matonos/ota/storage/1795308870, channels.json testing), ota-stage.sh stage →
  finalize → reboot: slot _b, fstab pc_x86_64.hdd, incremental matonos.20261122.005355.
  Note: stage alone does not switch slots; finalize sets the boot entry.
- **B→A PASSED (03:4x):** build-52 (1795310854) published to testing, stage → finalize →
  reboot: slot _a, fstab pc_x86_64.hdd, incremental matonos.20261122.012716.
  **OTA A→B→A on the testing channel is done** (testing = 1795310854; stable untouched).
- **Release ISO staged, NOT public:** matonos-1795310854-x86_64.iso (3.4 GB, build 52)
  + .sha256 in build-server /mnt/data/matonos/staging/iso/ (0700, not served; 404 from
  the site). Made on the host (server lacked sgdisk/genisoimage; since installed, make-iso.sh 0d34081
  adds /usr/sbin to PATH, server ISO build verified). Tools: sgdisk, mcopy,
  mformat, genisoimage). VM test: ISO as SATA DVD + blank 64 GiB HDD → firmware falls
  through to the DVD → live boot → Settings installer "Installation complete" → installed
  disk boots alone (slot _a, pc_x86_64.hdd, 1795310854). Kept VM: iso/hdd52b.img + vars-52b.fd.
  Publishing it is the user's call.
- Host issue: the first ISO boot (port 5557) wedged QEMU in D state (__vm_munmap) and
  after kill -9 it plus its virgl_render_server stay zombies holding port 5557 and the
  old hdd52.img lock. Likely host amdgpu/venus teardown; reboot of the host clears it.
  Test continued on port 5558.
- Old superseded VM images/ISO deleted; /home 88%.
- Build-52 installed VM (port 5558, hdd52b.img), store: searched "klotski" → Install →
  button "Installing…" for ~4 min → package flatpak.org.gnome.Klotski (+ .Locale) →
  button "Open"; Klotski window renders (screenshot out/pc-logs/iso52/klotski52.png).
  So (c) busy status works and the Klotski/glycin crash (a) does not reproduce here.
- Updates badge "4" on a fresh install (item d) is CORRECT, not a bug: the image bundles
  Fennec 156 (F-Droid 160), Thunderbird 24.0 (F-Droid 25.0), microG GmsCore + Companion,
  all older than their repos. Store "Update" on Thunderbird installed 25.0 over
  /product/app/MatonThunderbird in ~10 s (same signer) and the list dropped to 3.
  Done 96003d6: re-pinned Fennec 160.0.0, Thunderbird 25.0, microG 0.3.18 (hash +
  signer verified, signers unchanged). build-53 (1795325641) live VM: versions match,
  store shows no Updates badge and "No updates are available.", no crashes.
  The staged ISO is still build 52 (old pins); rebuild it from 53+ if wanted.
- Earlier plan: release ISO from the last build, VM install-test, stage on the server (not public).
- (f) stub cold start, build-52 installed VM (HDD, venus): Klotski process start → first
  xdg_surface = 2.7 s warm, 3.5 s after guest drop_caches (02:36:57.840 → 02:37:01.328;
  loader started at +0.4 s, the remaining ~3 s is the app itself). The ~28 s from earlier
  builds does not reproduce. Caveat: the host page cache still holds the disk image, so a
  true first boot on real HDD can be slower; re-measure on real hardware.
- (g) MEMORY.md is 24,192 bytes (< 24.4 KB): nothing to trim.
- Cron queue a–g is now fully closed.
- **Release ISO from build 53 staged, NOT public:** build-server
  /mnt/data/matonos/staging/iso/matonos-1795325641-x86_64.iso (+ .sha256), built on the
  server; the build-52 ISO is still next to it. VM: ISO + blank 64 GiB HDD → installer
  "Installation complete" → installed disk boots (slot _a, pc_x86_64.hdd, 1795325641).
  Kept VM: out/pc-logs/b53/hdd53.img + vars53.fd (port 5558); hdd52b.img deleted.
- Watch item: the FIRST boot of the installed build-53 disk loaded MatonOS from the ESP
  but adbd stayed "offline" for ~15 min (disk kept being written); after a VM restart it
  booted in ~1 min. Boot reason then: "reboot,factory_reset". No pstore/log survived.
  My test was messy (two launches fought over slots), so not proven to be an image bug;
  build-52 first boot was fine. Re-check on the next install test, with logcat captured.
- **First-boot stall explained (reproduced on a clean install, 09:34):** adbd comes up
  after 896 s. dmesg: `init: Wait for property 'apexd.status=activated' took 802574ms`.
  apexd decompresses ~30 compressed APEXes (.capex) from /system to
  /data/apex/decompressed, 10–90 s each. The VM image sat on the host's spinning
  /mnt/data disk (rotational=1); build-52's test disk was on NVMe, so it looked fine.
  Real HDD installs (supported layout) will hit this. Recommendation for the user:
  build with `PRODUCT_COMPRESSED_APEX := false` (the 16 GiB system partition has room;
  this also saves /data space). NOT changed yet, product decision.
  Logs: out/pc-logs/b53/firstboot-{logcat,dmesg}.txt. Kept VM: b53/hdd53b.img + vars53b.fd.
- **No-capex fix tested, NOT merged:** branch `wip/no-capex` (95e41f3, pushed) adds
  `PRODUCT_COMPRESSED_APEX := false` to pc_x86_64.mk. Build 54 (1795336420, 0 .capex /
  38 .apex) → ISO (server scratch/b54, not staged) → VM install on the spinning host disk
  → first boot 115 s (build 53: 896 s). Merge = `git merge --ff-only wip/no-capex` on
  metadata-enc, waiting for the user. Kept VM: out/pc-logs/b54/hdd54.img + vars54.fd.
  Server checkout is back on metadata-enc.
- **BUG: A/B fallback never happened; fix on `wip/fat-dates` (7763d4b, pushed, NOT merged).**
  The installer's FatFilesystem.cpp wrote every ESP entry with FAT date 0 (`1980-00-00`).
  OVMF refuses SetInfo on that, so systemd-boot could not rename `B+3-0.conf` to count down
  tries ("Failed to rename ... Invalid parameter"); a slot that cannot boot looped forever
  (1008 reboots seen). Fix: stamp the current time on new files and dirs. Host test: valid
  dates, fsck.fat clean. Firmware proof: entries re-dated with mtools → B counted down to
  `B+0-3` over 3 failed boots → slot A booted in 72 s. Existing test disks keep zero dates
  (no migrations). Note: the writer's own RenameFile accepts 8.3 names only.
- **OPEN: build 52 cannot boot after build 54** (OTA 54→52 on the b54 VM, slot B).
  It reboots about 8 s in with reason `netbpfload-missing`. The two builds have the same
  APEX versions (370399999) and differ only in compressed vs uncompressed APEXes. A→B→A
  between two compressed builds (51/52) worked. The kernel console shows no cause, so
  logs from slot B are still needed. This affects rolling back from a no-capex build to
  a compressed one, so look at it before merging `wip/no-capex`. Build 54 is uploaded to
  OTA storage/1795336420 but on no channel; the testing channel is still 1795310854.
- **SOLVED: why build 52 can't boot after build 54.** Build 54 has no .capex and an
  empty /data/apex/active, so at boot-complete upstream apexd touches
  `/metadata/apex/config/mount_before_data` (one-way migration to a single mount
  namespace; on the VM, `ro.init.mnt_ns.count=1`). /metadata is shared by both slots,
  so build 52's init also picks a single namespace. apexd-bootstrap can't mount .capex
  ("Cannot directly mount compressed APEX …tethering.capex"), normal-mode apexd skips
  activation ("19 packages restored", OnStart 0 ms), bpfloader falls back to
  /system/bin/false, and the boot ends with `reboot,netbpfload-missing`. Proof: verbose
  slot-B serial log at out/pc-logs/b54/slotB-verbose-serial.log (that boot used an
  options line with loglevel=7 on the B entry).
  Effect: after any no-capex build has booted, rolling back to a compressed build
  (≤ 53) fails. Those builds never shipped and every later build is no-capex, so this
  is dev-only once `wip/no-capex` is merged. Recommendation: merge `wip/no-capex` +
  `wip/fat-dates` together; no workaround code. For dev rollbacks to ≤ 53, delete
  the marker first.
  The test disk's B entry still has the verbose options line (disk only, harmless).
- **Merged (user OK, 15:2x):** wip/no-capex + wip/fat-dates → fc374a1. Build 55 (1795354159) from it:
  ISO install on blank 64 GiB HDD VM passed; ESP entries carry real dates; first disk boot 130 s
  (was 896 s with capex). ISO staged (not public) at build-server staging/iso/matonos-1795354159-x86_64.iso
  (+ .sha256); build-52/53 staged ISOs deleted. VM disk: out/pc-logs/b55/hdd55.img (port 5558).
- **User 2026-11-22:** public ISO waits for the custom desktop caption (Steam looks wrong without it).
  Codex caption-design (luna-high, read-only) running → out/pc-logs/agents/caption-design.report.md.

## 2026-11-22 17:55 — MatonSystemUI skeleton runs (wip/systemui-variant 4ffd3433)
- Variant SystemUI (own GlobalRoot + MatonWMComponent {WMShellModule, MatonCaptionModule}) is stable on the build-55 VM (5558): same PID 90 s, no new crashes; status bar, captions, taskbar as stock.
- Fixes needed: keep rule for MatonSystemUIAppComponentFactory; `-include` of stock proguard_common.flags (as Car does; included_proguard_flags_files alone doesn't apply it).
- Bind-mount test recipe: push APK, `chcon u:object_r:system_file:s0`, bind over /system_ext/priv-app/SystemUI/SystemUI.apk, delete /data/system/package_cache/*/SystemUI-*, `stop; start`, then pm grant the runtime perms (image builds get default grants on first boot).
- Next: framework caption-factory hook (~50–80 lines, waits on user OK), RRO left buttons + centred title, Steam test.
- 18:55 wip/systemui-variant 854fac68: caption layout override in the app (close/maximize/minimize left, centred title chip, 156dp right spacer). VM 5558: stable, close/maximize(+restore icon)/minimize all work. Screens: out/pc-logs/sysui/maton-sysui3.png, max.png. Not yet checked: maximize long-press menu anchor, TalkBack order.

## 2026-11-23 night (ISO gate = empty queue, user)
- Merged to metadata-enc (4cb1d3d2): own release keys (tools/release-keys.sh, MATON_RELEASE_KEY_DIR; build-server = ~/.matonos-build/release-keys, generated by the next image build → then back up to host) and the software-Vulkan warning notification (systembridge, vendor.maton.graphics.vulkan_reason).
- Build-58 (wip/systemui-variant 4786622e + 0004) installed on VM 5558 via OTA storage 1795384377 (no channel), boots slot B, no crashes.
  **BUG: caption provider never used.** AOSP 17 has ENABLE_WINDOW_DECORATION_REFACTOR on (read-only), so Shell uses DefaultWindowDecoration → AppHeaderController; 0004 only hooks legacy DesktopModeWindowDecoration. Codex caption-refactor-hook (luna-high) extends the hook in ~/matonos/build/fwb-caption → regenerate 0004 → build-59 → VM retest.
- Codex running: bt-audio (wip/bt-audio, luna-high), rotation (investigation only).
- 02:30 0004 regenerated (67007e1b): provider hooked into the refactored DefaultWindowDecoration/AppHeaderController path (not compiled by the agent; build-59 is the compile check). build-59 started 02:23 (caption + release keys + GPU warning + BT audio). Test it by FRESH INSTALL/live boot: platform key changed, so OTA onto 5558 will hit shared-UID signature mismatches.
- Rotation: IIO accelerometer AIDL HAL merged to metadata-enc (e3489887), not yet built. No accelerometer feature XML (the list is hardware-dependent). QEMU check = empty sensor list + boot OK.
- `.flatpak` bundles: design in out/pc-logs/agents/flatpak-bundle.report.md (3–5 days; .flatpakref +1–2). Subsystem side running (flatpak-bundle-impl, wip/flatpak-bundle); the store UI follows.
- 04:30 release-key build fixes: in-tree symlink `maton-release-keys` (Soong rejects out-of-tree paths), and `testkey` is provisioned too (sepolicy keys.conf). Host key backup at ~/.config/matonos/release-keys; re-sync after testkey was added. Caption: build-61 javac failure (@JvmOverloads) fixed with a setter (c259445c). build-62 hit a metalava JVM SIGSEGV (transient?); build-63 is running.
- `.flatpak` bundles: subsystem helper compiled and host-tested (unsigned refused/allowed, signed detected) and merged to subsystem main; contract REVISION 2 on contract main; F-Droid source rejects local files (829022c). Store UI is running (flatpak-bundle-ui, ~/matonos/build/sc-bundle). The dt store-sources/fdroid pin is not bumped yet.
- 05:10 build-63 OK (c259445c, 42 min). Live image booted headless on port 5560 (~/matonos/vm/live63, LD_LIBRARY_PATH=~/matonos/qemu-venus/lib). VM 5558 stopped (test-keys build; disk hdd55.img kept). Checked: dev-keys tag, vulkan=radeon, crash buffer empty, IModule/bluetooth + IBluetoothAudioProviderFactory registered (no BT controller), audioserver running. Caption provider BINDS on the refactored path; left-side buttons drawn. OPEN: maximize enters stock immersive (full bounds) but the status bar and caption stay visible over the content, because stock assumes the app hid its bars. Agent caption-immersive is running in fwb-caption. Not yet tested: left/right toggle, fallback, GPU warning (-g none), F5 Steam.
- metadata-enc and wip/systemui-variant are both at 796100cc (caption + release keys + rotation + GPU warning + BT audio).
- 05:50 caption-immersive merged into 0004 (bbebf79b): uses the stock setSystemBarVisibilityOverride (the SplitStatusBarHider precedent), provider only, plus a 2.5 s top-edge hover reveal. Rotation preflight fix 91a47621 (VINTF + rc moved from PRODUCT_COPY_FILES into Android.bp). build-64 running (dt 91a47621, sub b0e3a4ab). Bundle UI merged to software-centre main (5fd4b99); store picks the source via new contract CAP_LOCAL_FILES (contract 8c237d9, subsystem e15a73d4). Pins bumped in aa1b5e4d, so they arrive in build-65. Agent components-page (tray host permission + replaceable tray + Settings components page) is running in ~/matonos/build/dt-components.
- 06:40 build-64 failed (sensors licence module), build-65 failed (status_t not qualified); fixes f9a00b2d and 3728c971, with all three sensor .cpp files syntax-checked locally using the server's clang command. build-66 running (dt 3728c971). The components-page agent leaked drafts into the MAIN tree; they are reverted and archived in out/pc-logs/agents/components-page/leak-main-tree, and the main md5 is restored. Its Launcher3 patch had been diffed against the host's already-patched Launcher3; I regenerated it from pristine c612e6ec (9 files, apply-check OK). Components page merged (e9df6c5f; tsc + eslint OK). Subsystem 8766962: TrayHostService requires BIND_TRAY_HOST. Agent app-settings-slot running in ~/matonos/build/dt-appsettings.
- 07:30 build-66 failed (sensors link: libcutils missing; fix 7b334a1a). build-67 failed in apply-patches: both the server's and the host's Launcher3 had the old tray patch COMMITTED (detached 315e620c21), so the new full patch applied neither forward nor reverse. Server Launcher3 reset to pristine c612e6ec (backup branch matonos-tray-backup-1123). build-68 running. App settings slot merged (7753b915; tsc + eslint OK). Agent dbus-settings-page running in ~/matonos/build/sub-dbus.
- 08:50 build-68 built but system_server crash-loops: "Signature|privileged permissions not in privileged permission allowlist: org.matonos.settings WRITE_SECURE_SETTINGS". Two allowlist copies (rn-apps/settings/ and buildinfra/privapp-permissions/); only the rn-apps one was updated. Synced in b529b434 → build-69 running. TODO: single source for the allowlist. Debug method: loader.conf default → matonos-debug.conf via mcopy, -chardev socket serial, console shell → logcat -b crash (~/matonos/vm/sercmd.py). D-Bus settings page on subsystem main ca367f8f (stub read check = initiating installer). Agent linux-downloads running (~/matonos/build/sub-downloads).
- 09:25 USER DECISION NEEDED — Linux Downloads (agent report out/pc-logs/agents/linux-downloads.report.md, branch wip/linux-downloads = docs only): on API 37 there is no ordinary Android prompt that grants POSIX path access to Download; only MANAGE_EXTERNAL_STORAGE (all-files, Settings page) would back the symlink. Unchecked alternative: scoped storage lets an app create its OWN files in Download/ by direct path without any permission — if the Linux process can see /storage (stub mount ns + bwrap), the symlink may work with no prompt (reading other apps' files would still be denied). Parked until the user picks. build-70: metalava JVM SIGSEGV (tool crash) → build-71 retry running. build-69: Settings Java compile error fixed f34caa55.

## 10:15
- build-71 VM: caption restore works; left/right button toggle in the embedded MatonOS Captions page applies live; App settings lists Captions; System components page lists caption + tray providers.
- Real subsystem APK failed to install over the bootstrap: INSTALL_FAILED_DUPLICATE_PERMISSION (subsystem redeclared BIND_EMBEDDED_SETTINGS, owned by MatonSystemUI). Fixed in subsystem 6c45db95, so the "Linux D-Bus services" entry was missing on build-71.
- Merged wip/flatpak-safe-insets (subsystem 330a36ef): stub surface keeps gesture/cutout (+ visible bars) insets. VM check: Steam BPM maximized.
- build-72/73 superseded; build-74 (dt b2a82552, sub 330a36ef) running.
- Codex audio-boost (luna high) in ~/matonos/build/dt-audioboost: DynamicsProcessing on session 0, 100–200 % slider.

## 11:15
- build-74 (dt b2a82552, sub 330a36ef) VM PASS: real subsystem APK installs over the bootstrap; maximize hides status bar + caption; top-edge hover (MatonOS Pointer, relative evdev event5 via sendevent) reveals both and auto-hides; restore via revealed caption; App settings lists Linux D-Bus services + MatonOS Captions; D-Bus page renders ("No D-Bus providers" is correct, none on the image). Not tested: provider-absent fallback; Steam insets (needs Steam login).
- wip/systemui-variant fast-forwarded into metadata-enc (64644169), pushed. fwb-caption worktree removed (work lives in patch 0004).
- Headless VM tip: QEMU usb-tablet shows as ROTARY_ENCODER in Android, HMP mouse_move is relative → use sendevent on "MatonOS Pointer".
- audio-boost round 1 (04028a08) NOT merged: Settings background service dies when the app idles. Round 2 (audio-boost2) moves the effect owner into MatonSystemUI.
- vold-fs (luna high) started in ~/matonos/build/vold-fs (system/vold branch maton/linux-fs); patches will go to patches/system/vold/.
- 12:40 Host crashed ~11:52 (desktop froze, reboot). Build-75 on the server survived and is still running; codex agents had finished before the crash.
  - image-trim: nothing to merge. EROFS already uses lz4hc,9 by default; Android's libmodprobe cannot load .ko.zst/.ko.xz (it would need a loader patch); Mesa was already stripped. Branch dropped.
  - vold-fs: patch reviewed. I fixed the idmap (child only unshares the user namespace; vold writes the maps and does open_tree/mount_setattr/move_mount; the last map range no longer overflows u32). Added patches/system/vold, storage/matonos-linux-fs.rc (modprobe exfat xfs ntfs3 hfsplus udf isofs) and the vold_idmap.te user_namespace rule (69a3a749). Needs build-76 and a VM test with attached ext4/ntfs/iso disks. Note: only UID/GID 1000 maps to media_rw, so Android writes need group-writable files (umask 002 distros).
  - privapp allowlist now has one source, buildinfra/privapp-permissions (ca5309b7).
  - .flatpak bundles were already done (software-centre 5fd4b99 pinned).
  - efi-logo agent started (~/matonos/build/dt-efilogo, luna high).
- 12:30 build-75 OK and booted on VM 5560 (~/matonos/vm/live75). Volume boost code runs, but there was NO audio effect at all: the BayLibre APEX ships only *sw_generic placeholders (copy input to output) and the AOSP effects config names libraries that aren't installed. Fix with no patch: real AOSP AIDL effect libraries go to /vendor/lib64/soundfx (the effects service's fallback path) via PRODUCT_PACKAGES, config in audio/effects/, and a preflight exemption for those vendor-only libs (86d958da). The vold patch is now in APPROVED_PATCHES (user approval 11-16). build-76 is running (drives + effects + allowlist).
  - User 12:25 asked: keep the current HAL or put a desktop audio stack under a HAL interface? My recommendation: keep the current HAL for the ISO; reconsider PipeWire only if real-PC hardware coverage (jack/HDMI switching, codecs) proves weak.
  - efi-logo part 1 merged (8e9ca8be): normal boots keep kernel output on serial only. Part 2 (BGRT composer) agent efi-logo2 is running.
- 12:47 AUDIO DECISION (user): full PipeWire under our own AIDL HAL in the device tree, and it GATES the ISO. Report out/pc-logs/agents/audio-coverage.report.md (the agent recommended a middle route; the user chose full PipeWire). HAL base: subclass Module from AOSP libaudioserviceexampleimpl (public, meant for vendor extension), no AOSP patch. Mixing stays with AudioFlinger (one stream). Each PipeWire device is shown to Android as a device port; with no device, a "No audio device" stand-in. P1 agent pw-build (vendor stack recipe) is running in ~/matonos/build/dt-pipewire. The vold-fs worktree was removed (patch already exported).
- 13:35 build-76 failed: metalava's JVM crashed with a SIGSEGV (transient, not our code). Retried as build-77 (dt 9b5ac065, includes efi-logo part 1). P1 pw-build done (b9501f0b on wip/pipewire-build: 21 MiB PipeWire+WirePlumber+alsa-lib/UCM stack, all NEEDED libraries resolve, no GPLv3). It is NOT merged yet: nothing ran build-pipewire.sh in build.sh. P2 pw-hal is running (own HAL on libaudioserviceexampleimpl, behind a product flag that is off by default). efi-logo2 review found that /oem is on the read-only root, blobs were committed, and the name was odd; fix round efi-logo3 is running.
- Boost spec (user): a per-device multiplier from 0.5 to 1.5 in MatonOS Settings; PipeWire volume = Android volume x multiplier; no limiter. Goes in P3.

## 14:22 hourly
- Merged wip/efi-logo (bfd08b7a) + vold Linux-FS labels (102493e5) into wip/systemui-variant, pushed. dt-efilogo worktree removed.
- pw-run (P4: daemons, SELinux, HAL registration, AOSP effect service, default on) running PID 277916.
- BLOCKER: build server lacks python3-pil (bootlogo generator) and meson (PipeWire build); sudo helper gone — user asked to apt install. Next server build waits on it.

## 15:22 hourly
- Merged docs-caption-tray + pipewire-build (PipeWire HAL default ON, BayLibre APEX out, AOSP effect APEX in) → e86fecfc, pushed.
- pw-p3 (device mirroring/hotplug/per-device boost + Settings page) running PID 380923, worktree dt-pwp3.
- STILL BLOCKED: build server needs python3-pil + meson (user to apt install). Next build = first PipeWire image; VM checks listed in pw-run.report.md.

## 16:23 hourly
- build-78..81 failed early (preflight vs generated files; env+shell fn; stale vold patch on server; Soong ../ paths) — all fixed, pushed. build-82 running (first PipeWire image).
- Merged wip/pipewire-p3 (e0e0bef7; fixed JSX .map rule). Goes into the build after 82.
- Started byo-gapps (PID 516610, dt-gapps) and hotspot scope (PID 516619, dt-hotspot, report-only).
- Note: apply-patches.sh cannot replace a changed patch already applied (needs manual reset of that project).

## 17:23 hourly
- build-82 sepolicy neverallow (vold context= relabelto vfat) → reverted context mounts; build-83 HAL `try` with -fno-exceptions → strtof. build-84 running.
- Merged wip/byo-gapps (5aaebae9; fixed device ID → GSF gservices android_id). Open: no GSF stand-in → gsf-standin round PID see log.
- hotspot scope done (no AOSP patch needed); hotspot1 round (stock hostapd/P2P wiring + hwsim VM plan) running.
- Worktrees 4/4: main, dt-fdroid, dt-hotspot, dt-gsf.

## 18:22 hourly
- build-84 failed: file_contexts clash /oem(/.*)? vs AOSP oemfs → bootlogo labels only /oem/media (842c2ad9). build-85 running.
- Merged gsf-standin (872412f0: pinned microG GsfProxy v8, GappsInstaller reads real versions) and hotspot (b46312a2: stock hostapd + wpa_cli + wifi.direct; wifi.present = physical only). Hotspot VM plan in out/pc-logs/agents/hotspot1.report.md (hwsim radios=2; DHCP needs a test helper). Limit: stock framework can still start AP on hwsim (zero-patch rule).
- Started cleanup1 (privapp allowlist single source + stale TITLE-BARS/NOTES tray docs, dt-cleanup) and libcamera-scope (report only).
