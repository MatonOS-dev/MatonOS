# Full image OTA contract

The update centre downloads an immutable full image set over HTTPS, verifies
`manifest.json` against `latest.json`, and calls the slot writer. Payloads are
slot-neutral: one `vbmeta.img` has suffix-free descriptors for `system`,
`system_ext`, `product`, `vendor`, and `odm`. The writer selects the inactive
slot's `matonos-a.efi` or `matonos-b.efi` and names the installed image records
for that slot.

## URL layout

Given the configured OTA root URL `https://updates.example/`:

```text
latest.json
<build-id>/manifest.json
<build-id>/manifest.sig
<build-id>/system.erofs
<build-id>/system_ext.erofs
<build-id>/product.erofs
<build-id>/vendor.erofs
<build-id>/odm.erofs
<build-id>/vbmeta.img
<build-id>/matonos-a.efi
<build-id>/matonos-b.efi
```

`latest.json` is refreshed by the publisher and has a short cache policy via
`latest.json.hwfm`. Numbered release directories are immutable and remain on
the server until an operator removes them. `latest.json` has this schema:

```json
{"build_id":420,"version":"1.2","path":"stable/420/","manifest_sha256":"<64 lowercase hex chars>"}
```

The `path` is relative to the configured OTA root URL. The UI checks the
downloaded manifest's SHA-256 against `manifest_sha256`, then verifies the
detached signature using the public key installed at
`/system_ext/etc/security/matonos/ota-ed25519.pub` before trusting its file
list. The signature is the raw 64-byte Ed25519 signature over the exact bytes
of `manifest.json`.

## Manifest schema

Version 1 has this shape; every file entry contains the exact byte size and
lowercase SHA-256 of the corresponding downloaded file:

```json
{
  "format_version": 1,
  "channel": "stable",
  "build": {"id": 420, "version": "1.2"},
  "files": [
    {"name":"system.erofs","size":123,"sha256":"<64 lowercase hex chars>"},
    {"name":"system_ext.erofs","size":123,"sha256":"..."},
    {"name":"product.erofs","size":123,"sha256":"..."},
    {"name":"vendor.erofs","size":123,"sha256":"..."},
    {"name":"odm.erofs","size":123,"sha256":"..."},
    {"name":"vbmeta.img","size":123,"sha256":"..."},
    {"name":"matonos-a.efi","size":123,"sha256":"..."},
    {"name":"matonos-b.efi","size":123,"sha256":"..."}
  ]
}
```

The manifest must list those eight names exactly once. The writer independently
checks the signature, each size and hash, and requires `build.id` to be greater
than the running `ro.matonos.build_id` property. `BUILD_NUMBER` must therefore
be a positive monotonic integer for release builds; `ro.build.version.release`
provides the human version string.

## Update sequence

1. Fetch `latest.json`; skip if its build id is not newer than the installed
   `ro.matonos.build_id`.
2. Download the manifest, signature and all eight named files into a private
   temporary directory. Verify the latest manifest hash and Ed25519 signature,
   then sizes and SHA-256 hashes. Keep the directory layout flat.
3. Call `matonos-slot-writer stage <download-dir>`. The writer repeats all
   cryptographic and integrity checks before touching the inactive slot. Keep
   the download until staging returns success.
4. Call `matonos-slot-writer finalize`. This asks BootControl to select the
   target slot; the bootloader's normal counted entry provides rollback if the
   new system does not mark itself successful.
5. Reboot. After the new slot has booted and BootControl marks it successful,
   call `matonos-slot-writer remove-inactive` to reclaim the old slot. This is
   an explicit cleanup; do not run it before success is reported.

## Exit codes

All commands return `0` on success and `1` for an operational failure. The
writer prints the failure and its context to stderr. `stage` returns `1` for
an invalid signature, missing/mismatched file, non-newer build, disk/ESP
failure, or image write failure. `finalize` returns `1` if the target is
incomplete, its UKI does not pin the staged vbmeta, or BootControl refuses the
selection. `remove-inactive` returns `1` until the running slot is marked
successful, or if cleanup fails. Invalid command syntax returns `2` and prints
usage. A failure is not permission to reboot into a partially staged slot;
`stage` leaves the inactive counted entry removed until it completes its
writes and publishes the replacement entry.

## Publisher

`tools/ota-publish.sh <images-dir> <channel> <out-dir>` writes locally only.
The image directory is the AOSP product output after `tools/make-live.sh` has
run: it supplies `system.img` through `odm.img`, `system/build.prop`,
`vbmeta.img`, and `matonos-installed-{a,b}.efi`. Sparse system images are
converted to raw EROFS files with `simg2img`; no image data is modified. Set
`MATON_OTA_KEY` to the Ed25519 private-key path. The publisher writes
`<out-dir>/<channel>/<build-id>/` and its `latest.json` index, retains older
build directories, and prints a manual `rsync -aS` command. It never uploads.
The checked-in public key is a bootstrap key generated for this implementation;
no `MATON_OTA_KEY` was provisioned in this worktree, so its matching private
key is not retained. Replace the public key with the release public key before
building an image intended to accept published OTAs. The publisher rejects a
signing key that does not match the installed public key.
