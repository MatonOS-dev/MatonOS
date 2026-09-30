# Shared changes

- DONE — `tools/make-live.sh`: add an explicit `-S` Secure Boot image option
  that switches the live ESP to shim -> signed systemd-boot -> signed UKI
  while preserving the existing default image path.
- DONE — `tools/make-payload.sh`: add an explicit `-S` option and include
  signed EFI assets/UKI plus the public MOK certificate for Secure Boot
  installs.
- DONE — `tools/installer.sh`: when its payload says `SECURE_BOOT=1`, write
  the shim chain and UKI to the installed ESP and register the fallback shim
  path.
- DONE — `tools/build-kernel.sh`: add explicit `-S` opt-in to use the shared
  MatonOS signing key and enforce signed modules; no default kernel config
  change.

The signing helper and docs are area-owned under `secureboot/`. No AOSP
patches, new SELinux files, shared policy files, kernel fragments, or default
boot behavior were changed.
- DONE — `tools/run-qemu-live.sh`: add `-S` to select OVMF Secure Boot firmware with
  Microsoft keys and `-V` to persist a writable firmware-vars file across the
  MokManager enrollment/reboot test.
- DONE — `tools/make-live.sh`: always prepend the pinned AMD/Intel microcode early-cpio
  to normal initrd entries; Secure Boot UKIs embed the same archive first.
- DONE — `tools/make-payload.sh`: always build the pinned early-cpio, include it first
  in Secure Boot UKIs, and package it with source notices for normal installs.
- DONE — `tools/installer.sh`: install the microcode cpio as the first initrd for
  normal installs and preserve microcode source notices on the ESP.
- DONE — `tools/build-kernel.sh`: always stage the pinned microcode cpio and notices,
  independent of `-S` and firmware staging options.
- DONE — `tools/make-live.sh`, `tools/make-payload.sh`, and `tools/installer.sh`:
  carry the Fedora shim-x64 BSD-3-Clause notice alongside the vendor-signed
  shim/MokManager files placed on Secure Boot ESPs.
