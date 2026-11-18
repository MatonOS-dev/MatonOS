# OTA API and storage contract

The OTA service exposes a JSON API and immutable files. It does not upload or
publish builds. A deployment promotes a release by editing its channels
configuration with `ota-server/src/promote.ts`, which refuses a lower
`android_base` than the channel currently serves. Same-base moves in either
direction are allowed. The device and slot writer also enforce downgrade
protection.

## Configuration and publisher

The server requires `OTA_STORAGE_DIR` and `OTA_CONFIG`; `OTA_PORT` defaults to
8080. The storage directory contains immutable update directories:

```text
<storage>/<update_id>/manifest.json
<storage>/<update_id>/manifest.sig
<storage>/<update_id>/manifest.json.sig
<storage>/<update_id>/<base payload files>
<storage>/<update_id>/<addon id>/<addon files>
```

The publisher writes directly to `<out>/storage/<update_id>/`. Set
`OTA_ADDONS_DIR` to an optional directory whose immediate child directories
are add-on IDs; each child tree is copied into that release and included in
its signed `addons` manifest inventory. The signed `files` array remains the
eight base slot-writer payload files. The identical detached signature is
published as `manifest.sig` for the existing slot writer and
`manifest.json.sig` for clients using the manifest filename convention.

The server config has this shape:

```json
{
  "channels": {
    "testing": { "update_id": 1795430400 },
    "main": { "update_id": 1795430400 },
    "internal": { "update_id": 1795430400, "requires_license": true }
  }
}
```

Only `testing` and `main` are initially configured. Channel names are
configuration-driven and may later include versioned names without code
changes. All update and channel selection paths refuse a release with an
Android base below the request's `base_android`. They also require every ID in
the device's `addon_list` to have an entry in the release manifest's `addons`
object. `/api/update` returns `{"available":false,"reason":"missing_addons",
"missing_addons":[...]}` listing every missing ID; `/api/channels` hides a
channel whose current update fails this gate. Channel selection also omits
channels marked `requires_license` because no license validation is implemented yet.
The current slot-writer stage cannot repeat the add-on check: its manifest
model contains only base payload metadata, and the device tree has no source
that enumerates installed add-on IDs. `install/tools/test-install.sh:77-85`
defines the `addons_a`/`addons_b` image partitions, but those partition names
do not identify the add-ons installed in them. The caller must therefore send
the authoritative `addon_list`; adding a device-side inventory is still open.

## Request schema

All endpoints use `POST`, `Content-Type: application/json`, and accept the
same object. `machine_id` and `license_hash` are optional strings accepted but
otherwise unused; neither is logged or stored. When present, each must be a
string.

```json
{
  "update_id": 1795430400,
  "channel": "testing",
  "base_android": 18,
  "machine_id": "opaque-device-id",
  "addon_list": ["games", "editor"]
}
```

Requests larger than 64 KiB are rejected.
Successful and error API responses use `Cache-Control: no-store`.

## Endpoints

`POST /api/channels` returns configured channels this device can switch to.

```json
{"channels":[{"channel":"testing","update_id":1795430401,"version":"18","android_base":18}]}
```

Channels below the installed Android base, license-required channels, and
channels missing any installed add-on are omitted.

`POST /api/update` checks the configured request channel. An update response
contains the base files and every requested add-on. If any requested add-on is
missing, the update is unavailable and the response names those IDs:

```json
{"available":false,"reason":"missing_addons","missing_addons":["editor"]}
```

```json
{
  "available": true,
  "update_id": 1795430401,
  "version": "18",
  "android_base": 18,
  "manifest_url": "/storage/1795430401/manifest.json",
  "signature_url": "/storage/1795430401/manifest.json.sig",
  "files": [{"name":"system.erofs","url":"/storage/1795430401/system.erofs","size":123,"sha256":"..."}],
  "addons": {"games":[{"name":"runtime.tar.zst","url":"/storage/1795430401/games/runtime.tar.zst","size":123,"sha256":"..."}]}
}
```

If the channel points below `base_android`, the response is
`{"available":false,"reason":"android_base_too_old"}`. If it points to the
already installed update, the response is `{"available":false}`.

`POST /api/eol` always returns:

```json
{"eol":false}
```

There is no EOL configuration or date logic.

`POST /api/license` returns `{"status":"none","license":null}` until
license support is implemented.

Errors have `{ "error": "code", "message": "description" }`. API requests
are logged with method, path, status, and error context; `machine_id` and
request bodies are excluded.

## Storage HTTP behavior

Files are served at `/storage/<update_id>/<path>` with
`Cache-Control: public, max-age=31536000, immutable`, `Accept-Ranges: bytes`,
and single byte-range support. Invalid or unsatisfiable ranges return 416.
The `manifest.json` and signature are immutable along with the release files.

The client downloads the signed manifest and detached signature, verifies the
signature with the installed OTA public key, checks sizes and SHA-256 values,
then passes the complete base payload directory to `matonos-slot-writer`.
The writer repeats verification and downgrade checks before writing. It also
requires every currently installed add-on ID to be present as a key in the
signed manifest's `addons` object, and refuses staging with the missing IDs
named in its error.

Updater clients obtain the installed add-on IDs from the add-ons service's
`vendor.matonos.channel.IChannel/addons` `status` command through the system
bridge generic channel. The response's `addons` array contains one object per
installed add-on, with its ID in `id` (alongside `version`, `valid`, and
validation details). Send those `id` values as `addon_list` in OTA API
requests. The slot writer uses the same service state directly: an ID is
installed when its `/mnt/vendor/addons/<id>/active-version` file is present
and nonempty.
