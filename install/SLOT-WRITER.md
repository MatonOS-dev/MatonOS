# Inactive slot writer

`matonos-slot-writer` is a system_ext CLI for the installed-disk file-backed
layout. It stages whichever slot is not running, using `ro.boot.slot_suffix`
to choose the target. The payload directory contains raw, 512-byte-aligned
images named `system_<target>.erofs`, `system_ext_<target>.erofs`,
`product_<target>.erofs`, `vendor_<target>.erofs`, and `odm_<target>.erofs`,
plus `vbmeta_<target>.img` and `EFI/Linux/matonos-<target>.efi`. The writer
checks that the UKI's embedded `androidboot.vbmeta.digest` matches the SHA-256
digest of the target vbmeta header, authentication block, and auxiliary block.
It does not decompress, rebuild, or resign the payload.

The extent format is the existing libfiemap image metadata under
`/metadata/maton/images`; first-stage init opens that metadata and the backing
directory `/maton_system/images`, then calls `ImageManager::MapAllImages()`.
The writer calls the same `ImageManager` API with the standard logical image
names (`system_a`, `system_b`, etc.), so first-stage sees the existing LP
metadata and extent representation.

Commands:

```sh
matonos-slot-writer stage <payload-dir>
matonos-slot-writer finalize
matonos-slot-writer remove-inactive
```

`stage` removes stale target loader entries and UKI before replacing its
images, then copies the running slot's entry to a counted target entry and
changes its UKI path and slot suffix. The target remains unselected. After
validating that the target images, UKI, and vbmeta are present and the UKI
digest matches the staged vbmeta, `finalize` calls
`/system/bin/bootctl set-active-boot-slot <target-index>`. BootControl owns
success marking and renames the counted entry to `<A|B>.conf`.
`remove-inactive` is a separate, explicit operation. It refuses unless the
currently running slot is marked successful, then removes the other slot's
images, vbmeta, loader entries, and UKI.

## Failure and disk risk

Every failed write stops the command and logs context. Since only the inactive
slot's image records, UKI, and counted entry are written before `finalize`,
failure leaves the running slot's image set and selection intact. Replacement
is deliberately non-atomic: a power loss can leave the inactive slot unusable,
but that slot has no loader entry while it is being rewritten. `finalize` checks
the UKI's pinned digest against the staged vbmeta immediately before selection.
After the target is selected, systemd-boot's three-attempt counter is the
recovery path.

The implementation relies on libfiemap's `CreateBackingImage()` and metadata
update for allocation and extent registration. That API does not expose an
atomic replacement operation; the old inactive image is removed before the
new one is created. A failed stage can require retrying the full inactive-slot
stage. No AOSP build, Soong build, or VM boot was run for this follow-up; image
integration and installed-disk validation remain pending.
