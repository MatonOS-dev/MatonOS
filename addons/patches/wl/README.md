# Broadcom `wl` compatibility patch provenance

These patches are applied in lexical order by `build/build-release.sh` when
the `broadcom-wl` candidate is enabled. They are compatibility-only; do not
disable kernel IBT or objtool to make the proprietary object pass.

- `0001-linux-7.2-remove-strncpy.patch` — Joan Bruguera Micó's tentative
  Linux 7.2 `strncpy` removal compatibility change for broadcom-sta
  6.30.223.271 (`1ce2a04a54718e8dac4ee2dac6ff11d04aef79d0`). SHA-256:
  `f86c7030a9c465cd26bce54d6ab90d946abe182b9c0a9ee84a286cbaa3258f86`.
- `0002-debian-linux-7.1-wireless-dev-callbacks.patch` — Linux 7.1 cfg80211
  callback signature compatibility change by Joshua Gatley-Dewing, carried
  by Debian's broadcom-sta 6.30.223.271-32 patch set. SHA-256:
  `5d9d9db1ab9b22ca72e72254aaa55762938d9251d9a505cf16a56266c24abc07`.

The patches modify GPL-covered compatibility glue; the prebuilt Broadcom
hybrid object remains proprietary. Its redistribution permission is not
approved, so no driver package may be published unless both object
redistribution and complete patch licensing are cleared. The selected
Nurozen 6.17 source already contains the Android `wlan%d` interface naming
and Makefile/compiler pieces found in BlissOS's `eth-to-wlan` and compiler
patches. Those Bliss changes are therefore recorded as source provenance,
not blindly reapplied.
