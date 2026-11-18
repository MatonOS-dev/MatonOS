# Full image OTA contract

The update centre downloads an immutable full image set over HTTPS, verifies
the signed `manifest.json`, and calls the slot writer. Payloads are slot-neutral:
one `vbmeta.img` has suffix-free descriptors for `system`, `system_ext`,
`product`, `vendor`, and `odm`. The writer selects the inactive slot's
`matonos-a.efi` or `matonos-b.efi` and names installed image records for it.

## URL layout

Given the configured OTA root URL `https://updates.example/`:

```text
updates/testing-manifest.json
updates/testing-manifest.json.hwfm
updates/main-manifest.json
updates/v16_10-manifest.json
updates/<update_id>/manifest.json
updates/<update_id>/manifest.sig
updates/<update_id>/system.erofs
updates/<update_id>/system_ext.erofs
updates/<update_id>/product.erofs
updates/<update_id>/vendor.erofs
updates/<update_id>/odm.erofs
updates/<update_id>/vbmeta.img
updates/<update_id>/matonos-a.efi
updates/<update_id>/matonos-b.efi
```

Each `<update_id>` is the build's `ro.build.date.utc` value. A build directory
is immutable and can be referenced by any number of channel manifests. Channel
names contain only lowercase ASCII letters, digits, and underscores. Channel
manifests are plain JSON pointers, not signed, and have this schema:

```json
{"format":1,"update_id":1795430400,"version":"1.2"}
```

They are hints for discovery only. The device does not read a channel property,
validate a channel name, or trust the pointer for integrity or downgrade
protection. The downloaded build manifest's Ed25519 signature authenticates
its build id and file inventory; stage continues to require that id to be
greater than running `ro.build.date.utc`.

## Signed build manifest

Version 1 has this shape; every file entry contains exact size and lowercase
SHA-256 for its downloaded file:

```json
{
  "format_version": 1,
  "build": {"id": 1795430400, "version": "1.2"},
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

The manifest must list those eight names exactly once. The UI verifies the
detached signature using the public key installed at
`/system_ext/etc/security/matonos/ota-ed25519.pub` before trusting its file
list. The signature is the raw 64-byte Ed25519 signature over the exact bytes
of `manifest.json`. The slot writer independently repeats signature, file,
size, hash, and newer-build checks before writing.

## Update sequence

1. Fetch `<channel>-manifest.json` (with the short cache sidecar policy); skip
   if its update id is not newer than installed `ro.build.date.utc`.
2. Download `updates/<update_id>/manifest.json`, its signature, and the eight
   named files into a private temporary directory. Verify the signature,
   sizes, and SHA-256 hashes. Keep the directory flat.
3. Call `matonos-slot-writer stage <download-dir>`. The writer repeats the
   cryptographic and integrity checks before touching the inactive slot. Keep
   the download until staging succeeds.
4. Call `matonos-slot-writer finalize`. BootControl selects the target slot;
   the bootloader's counted entry provides rollback if the system does not
   mark itself successful.
5. Reboot. Once the new slot boots and BootControl marks it successful, call
   `matonos-slot-writer remove-inactive` to reclaim the old slot.

## Exit codes

Writer commands return `0` on success and `1` for an operational failure.
`stage` returns `1` for invalid signature, missing/mismatched file,
non-newer build, disk/ESP failure, or image write failure. `finalize` returns
`1` if the target is incomplete, its UKI does not pin the staged vbmeta, or
BootControl refuses selection. `remove-inactive` returns `1` until the running
slot is successful or if cleanup fails. Invalid command syntax returns `2`.
A failed stage leaves the inactive counted entry removed until all writes
complete; it is not permission to reboot into a partially staged slot.

## Publisher and promotion

`tools/ota-publish.sh <images-dir> <out-dir>` writes one build to
`<out-dir>/updates/<update_id>/`. The image directory is the AOSP product
output after `tools/make-live.sh`; it supplies the five partition images,
`system/build.prop`, `vbmeta.img`, and `matonos-installed-{a,b}.efi`. Sparse
partition images are converted to raw EROFS with `simg2img`; image data is not
modified. Set `MATON_OTA_KEY` to the Ed25519 private-key path. The publisher
does not promote a build or upload files.

`tools/ota-promote.sh <out-dir> <channel> <update_id>` atomically writes the
plain channel pointer and its `.hwfm` sidecar. It refuses to move an existing
channel to a lower build id, so operators can promote the same immutable build
from `testing` to `main` without rebuilding. Upload the resulting `updates/`
tree using the printed/manual deployment process.

The checked-in public key is a bootstrap key; replace it with the release
public key before building an image intended to accept published OTAs. The
publisher rejects a signing key that does not match the installed public key.
