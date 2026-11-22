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
  the site). Made on the host: the server lacks sgdisk (make-iso.sh needs sgdisk, mcopy,
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
- Still open: Updates badge shows "4" on this fresh install (item d) — not looked at yet.
- Earlier plan: release ISO from the last build, VM install-test, stage on the server (not public).
