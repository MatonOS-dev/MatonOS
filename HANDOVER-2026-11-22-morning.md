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

## In progress
- build-48 on the server (dt 7489ed2, sub eda26aea): bridge hardware channel + OTA slot
  writer. Next: VM test, merge metadata-enc → main, OTA A→B→A on testing, release ISO
  staged on the server (not public).
