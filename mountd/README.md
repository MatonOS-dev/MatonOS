# matonos-mountd

`matonos-mountd` offers the Linux subsystem an optional fast filesystem view.
If the daemon is absent or a request fails, the subsystem must keep its current
proxy VFS path. Mount failures return promptly and do not gate boot or launch.
This first version supports extracted app/runtime deployment directories only;
EROFS loop mounting is not implemented.

The stable channel target is `vendor.matonos.channel.IChannel/mountd`. The
System Bridge authorizes the calling installed subsystem package for that
target. mountd separately checks that the supplied PID is alive, has the
supplied app UID, runs under the supplied package process name, and that the
package/UID pair exists in PackageManager's `packages.list`. PIDFD and process
start-time checks pin the process across the namespace operation. Source paths
are never accepted in JSON: they are derived under the fixed per-user
`/data/misc_ce/<user>/matonos-linux/` roots and opened without following
symlinks. A request worker gets five seconds; a timed-out operation is killed.

## Channel wire format

Call `IChannel.call("mount_stub", jsonArgs)`. `jsonArgs` must be exactly an
object with these four fields (no extras):

```json
{"pid":1234,"uid":10123,"package":"org.matonos.linux.firefox","deployment":"app-0123abcd"}
```

`pid` and `uid` are positive JSON integers. `package` is the installed stub's
package/process name. `deployment` is an opaque ID using only ASCII letters,
digits, `_` and `-`, beginning with a letter or digit. Its fixed source roots
are:

* `/data/misc_ce/<uid/100000>/matonos-linux/deployments/<deployment>/app` → `/app`
* `/data/misc_ce/<uid/100000>/matonos-linux/deployments/<deployment>/usr` → `/usr`
* `/data/misc_ce/<uid/100000>/matonos-linux/home/<uid>` → `/home/<uid>`

Success:

```json
{"ok":true,"fallback":false,"mode":"mounted","mounts":["/app","/usr","/home/10123"]}
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

`tests/mountd_core_test.c` covers request validation and multi-user path
derivation. Build the Soong module with
`MATON_BUILD_COORDINATOR=1 bash tools/build.sh -m matonos-mountd`, and validate
policy with `.../tools/build.sh -m selinux_policy`. No VM was used for this
implementation. The subsystem still needs to add the optional channel call
and retain proxy fallback; there is no bridge method that introspects arbitrary
live process identity, so mountd itself verifies PID, UID, package process name
and installed UID mapping. The deployment storage layout above must match the
subsystem's verified extracted-deployment layout before enabling calls.

On the Ryzen 5800X/RX 6600 build PC, Surface Pro 3, and HP ProDesk 600 G1,
verify absent daemon startup does not affect boot; then launch a Linux app with
mountd disabled and enabled, confirm `/proc/<pid>/mountinfo` in the stub shows
only the expected `/app`, `/usr` and home bind mounts, and compare direct file
open/stat behavior. Repeat with a stale PID, bad UID/package, invalid
deployment ID, unavailable source, duplicate request, and missing `/app` or
`/usr` mountpoint. Every failure must leave the app using the proxy path.
