# Inactive slot writer

`matonos-slot-writer` is a system_ext CLI for the installed-disk file-backed
layout. It only stages B while `_a` is running. Its payload directory contains
raw, 512-byte-aligned images named `system_b.erofs`, `system_ext_b.erofs`,
`product_b.erofs`, `vendor_b.erofs`, and `odm_b.erofs`, plus `vbmeta_b.img` and
`EFI/Linux/matonos-b.efi`. The B UKI must already pin the exact `vbmeta_b.img`
digest and size. The writer does not decompress, rebuild, resign, or validate
that cryptographic relationship.

The extent format is the existing libfiemap image metadata under
`/metadata/maton/images`; first-stage init opens that metadata and the backing
directory `/maton_system/images`, then calls `ImageManager::MapAllImages()`.
The writer calls the same `ImageManager` API with the standard logical image
names (`system_b`, etc.), so first-stage sees the existing LP metadata and
extent representation. It unmaps and replaces existing B image records as
needed, writes and flushes every mapped image, then writes the B UKI and a
counted `B+3-0.conf` copied from `A.conf` with the B UKI and `_b` suffix.

Commands:

```sh
matonos-slot-writer stage <payload-dir>
matonos-slot-writer finalize
matonos-slot-writer remove-old-a
```

`stage` never changes loader default. After verifying the staged payload,
`finalize` calls `/system/bin/bootctl set-active-boot-slot 1`, which reaches
the existing BootControl HAL and selects B. BootControl owns success marking
and renames the counted entry to `B.conf`. `remove-old-a` is a separate,
explicit operation and refuses unless the running suffix is `_b` and
BootControl reports B successful.

## Failure and disk risk

Every failed write stops the command and logs context. Since only B image
records, B's UKI and B's counted entry are written before `finalize`, failure
leaves A's image set and default selection intact. `finalize` is intentionally
last; do not run it unless the B UKI matches the staged vbmeta and B's system
images. After B is selected, systemd-boot's three-attempt counter is the
recovery path. A power loss while replacing B can leave B unusable, but A
remains the selected default until `finalize` succeeds.

The implementation currently relies on libfiemap's `CreateBackingImage()`
and metadata update for allocation and extent registration. That API does not
expose an atomic rename operation for replacing an existing image; the old B
image is removed before the new one is created. This is safe for A's boot path
but means a failed stage can require retrying the full B stage. No AOSP build,
Soong build, or VM boot was run for this task; the Android module integration
still needs a coordinated image build and installed-disk validation.
