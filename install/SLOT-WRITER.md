# Inactive slot writer

`matonos-slot-writer` is a system_ext CLI for the installed-disk file-backed
layout. It stages whichever slot is not running, using `ro.boot.slot_suffix`
to choose the target. `stage <dir>` consumes a signed, slot-neutral OTA set:
five raw EROFS images (`system.erofs`, `system_ext.erofs`, `product.erofs`,
`vendor.erofs`, `odm.erofs`), one `vbmeta.img`, and both installed UKIs
(`matonos-a.efi`, `matonos-b.efi`), together with `manifest.json` and its raw
Ed25519 detached signature `manifest.sig`.

Before any slot state changes, the writer verifies the signature using
`/system_ext/etc/security/matonos/ota-ed25519.pub`, validates the exact file
inventory, sizes and SHA-256 hashes, and refuses an `android_base` below the
running major release parsed from `ro.build.version.release`. Build ids are
not used for downgrade protection; same-base builds are accepted in either
timestamp order. It selects the target UKI itself. The UKI's
embedded `androidboot.vbmeta.digest` must match the SHA-256 digest of the
target vbmeta header, authentication block, and auxiliary block.

The extent format is the existing libfiemap image metadata under
`/metadata/maton/images`; first-stage init opens that metadata and the backing
directory `/maton_system/images`, then calls `ImageManager::MapAllImages()`.
The writer calls the same `ImageManager` API with the standard logical image
names (`system_a`, `system_b`, etc.), so first-stage sees the existing LP
metadata and extent representation.

The installer does not execute this system_ext binary from its vendor
domain. Init starts it as a disabled oneshot after the installer writes a
request file and sets `vendor.maton.install.slot_writer.request`; init writes
the matching done id after exit. Request and result files plus the staged
payload directory use the existing shared `matonos_install_mount_file` type
under `/maton_system/ota-staging`.

The init service runs the request mode:

```sh
matonos-slot-writer --request <request-file> <result-file>
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

See [OTA-CONTRACT.md](OTA-CONTRACT.md) for publisher format, verification and
update-centre sequence.

The writer does not consume or validate channel pointers. The signed build
manifest carries the authenticated Android base used for downgrade protection;
channel promotion is server-side metadata.

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
stage. Host verification covers signing, tamper detection, and downgrade
rejection. No AOSP build or installed-image boot was run for this change.
