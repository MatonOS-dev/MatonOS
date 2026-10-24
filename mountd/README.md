# matonos-mountd

`matonos-mountd` offers the Linux subsystem an optional fast filesystem view.
If the daemon is absent or a request fails, the subsystem must keep its current
proxy VFS path. Mount failures return promptly and do not gate boot or launch.
This first version supports extracted app/runtime deployment directories only;
EROFS loop mounting is not implemented.

The stable channel target is `vendor.matonos.channel.IChannel/mountd`. Only the
system bridge can reach the channel. It checks the installed subsystem caller,
verifies the requested stub package, UID, signing certificate and live PID,
then adds the verified signer digest and a fresh nonce. mountd rechecks the
process UID, command name and start time through a pidfd before and after
entering its mount namespace. SELinux grants access only to explicitly labeled
MatonOS deployment and home trees, never generic app or system data. Source
paths are never accepted in JSON: they are derived under the fixed per-user
`/data/misc_ce/<user>/matonos-linux/` roots and opened without following
symlinks. Each request has a five-second deadline; a timeout returns to proxy
mode without waiting on the terminated mount worker.

## Channel wire format

The subsystem calls the bridge's generic channel method with `mount_stub` and
exactly these four fields (no extras):

```json
{"pid":1234,"uid":10123,"package":"org.matonos.linux.firefox","deployment":"app-0123abcd"}
```

`pid` and `uid` are positive JSON integers. `package` is the installed
Android stub package/process name. `deployment` is an opaque ID using only
ASCII letters, digits, `_` and `-`, beginning with a letter or digit. The
bridge verifies the package, UID, signer and live process. Its call to the
daemon carries exactly these six fields:

```json
{"pid":1234,"uid":10123,"package":"org.matonos.linux.firefox","deployment":"app-0123abcd","signer":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","requestNonce":"01234567-89ab-cdef-0123-456789abcdef"}
```

`signer` is the lowercase hexadecimal SHA-256 signing certificate digest
verified by the bridge. `requestNonce` is a bridge-generated UUID. The daemon
requires both fields and echoes the nonce in a mount response. The fixed source
roots are:

* `/data/misc_ce/<uid/100000>/matonos-linux/deployments/<deployment>/app` → `/app`
* `/data/misc_ce/<uid/100000>/matonos-linux/deployments/<deployment>/usr` → `/usr`
* `/data/misc_ce/<uid/100000>/matonos-linux/home/<uid>` → `/home/<uid>`

Success:

```json
{"ok":true,"fallback":false,"mode":"mounted","mounts":["/app","/usr","/home/10123"],"requestNonce":"01234567-89ab-cdef-0123-456789abcdef"}
```

Any validation, identity, source, namespace, timeout or mount failure returns
`{"ok":false,"fallback":true,"mode":"proxy","error":"..."}`. The
subsystem must treat a missing channel, Binder exception, malformed response,
or `fallback:true` as a request to use the existing proxy path. `get_status`
with `{}` returns `{"ok":true,"available":true,"mode":"extracted_bind","erofs":false}`.

Targets `/app` and `/usr` must already exist in the stub's root image. Their
image locations are provided by `BOARD_ROOT_EXTRA_FOLDERS += app usr`. The
home target must exist in the stub mount namespace before calling mountd.
App/runtime bind mounts are read-only with `nosuid,nodev`; home remains
writable. An existing mount is accepted only when its target inode matches
the opened source and its read-only state matches the requested mode. A
different mount is left untouched and causes proxy fallback.

## Validation and remaining work

`tests/mountd_core_test.c` covers request, signer and nonce validation and multi-user path
derivation. Build the Soong module with
`MATON_BUILD_COORDINATOR=1 bash tools/build.sh -m matonos-mountd`, and validate
policy with `.../tools/build.sh -m selinux_policy`. No VM was used for this
implementation. The subsystem still needs to add the optional channel call
and retain proxy fallback. Its deployment metadata must ensure the selected
deployment belongs to the verified stub; mountd limits lookup to that ID and
requires UID-owned source directories. Confirm the storage layout with the
subsystem before enabling calls.

On the Ryzen 5800X/RX 6600 build PC, Surface Pro 3, and HP ProDesk 600 G1,
verify absent daemon startup does not affect boot; then launch a Linux app with
mountd disabled and enabled, confirm `/proc/<pid>/mountinfo` in the stub shows
only the expected `/app`, `/usr` and home bind mounts, and compare direct file
open/stat behavior. Repeat with a stale PID, bad UID/package, invalid
deployment ID, unavailable source, duplicate request, and missing `/app` or
`/usr` mountpoint. Every failure must leave the app using the proxy path.
