# mountd design

Status (2026-10-24): device-side extracted-directory prototype implemented.
The subsystem-side syscall fast path and authenticated deployment-record
integration are still pending, so mountd by itself does not deliver native
syscall latency while the proxy still traps every Linux `openat` and
`newfstatat`. A host-only direct-prefix prototype reduced proxy open latency
by 17% and stat latency by 6%, leaving both about 32× slower than native.
User-dispatch overhead dominates the remaining cost. EROFS loop mounting is
also deferred; failures keep the existing proxy path.

## Decision from the spike

Keep the proxy VFS translation as the compatibility fallback, and proceed
with a narrow mountd design. Build a daemon prototype only together with a
proxy fast path that can let safe filesystem syscalls reach the kernel
without the user-dispatch trap. The host-only test switch in `lsa-dns` skips
VFS pathname translation; it does not skip syscall dispatch, and the measured
gain was too small to justify shipping mountd by itself.

The test VM is v46, enforcing. `nsenter -t <loader-pid> -m` successfully bound
an extracted app tree inside the Tux Racer process mount namespace. The
namespace inode differs from the shell's namespace. The VM root has no `/app`
or `/usr`, and is read-only, so target directories must be part of the root
image before the daemon can mount those exact paths. For the spike's lookup
measurements, an isolated test root supplied equivalent `/app` and `/usr`
mountpoints without changing the image.

## Proposed shape

* One privileged native C daemon, `matonos-mountd`, started asynchronously by
  init. Startup and every request are bounded; failure leaves the stub on the
  existing translated VFS path and never blocks boot or app launch.
* The subsystem sends a `mount_stub` command over the system bridge's generic
  `vendor.matonos.channel.IChannel/mountd` target. The bridge authenticates
  the installed subsystem caller, verifies the requested stub package, UID,
  signer and live PID, then passes those fields plus a fresh request nonce.
  Do not add a second app-facing Binder service.
* mountd opens a pidfd first, checks the PID still belongs to the supplied
  UID and installed stub identity, then opens `/proc/<pid>/ns/mnt`. Recheck
  the pidfd and process identity before mount operations to close PID reuse
  races. Reject arbitrary caller paths: derive source trees from the
  verified stub's deployment record and UID-owned work directory.
* The daemon enters only that mount namespace and mounts the fixed mountpoints
  (the targets must already exist in the root image),
  and bind-mounts the selected immutable app tree at `/app`, the authenticated
  runtime tree at `/usr`, and the stub's writable home at `/home/<name>`.
  Mount read-only app/runtime sources; keep only the home writable. Use
  `MS_NOSUID|MS_NODEV` where compatible with Linux application behavior.
* Keep the source open/identity pinned across the operation. Treat namespace
  setup as idempotent: if the expected mount IDs and source identities already
  match, return success; reject unexpected mounts instead of replacing them.
* Track mount namespace inode, stub PID/start time, and source deployment
  identity. When the process exits, its namespace and mounts disappear with
  it. Do not unmount paths in a recycled PID. On update, a new stub process
  gets new mounts; a running process keeps its pinned old deployment.
* Current extracted-tree source roots are `/data/misc_ce/<user>/matonos-linux/`
  and are labeled with dedicated SELinux types. The subsystem must provide a
  deployment ID from its authenticated deployment record; the daemon derives
  every source path from the ID and UID and never accepts paths from callers.
  Confirm that install ownership matches the enforced UID ownership check.
* Retain proxy translation for nested bwrap views, `flatpak-spawn --host`,
  noncanonical paths, anything outside the fixed app/runtime/home view, and
  all launches if mountd is unavailable or rejects the request.

## Content format

The installed Tux Racer app tree is not an extracted directory. Its 49 MiB
payload is carried as hash-named entries in an installed APK split and read
through `objects.index`; the deployment's `files/` directory is an empty
placeholder. The host spike extracted 519 regular files (49,300,206 bytes)
from the index and carrier for the mount test. Runtime payloads use the same
pack-index mechanism. A real mounted view therefore needs content
materialization or an image format understood by the kernel.

Use one immutable EROFS image per app/runtime deployment as the first
candidate:

* Build the image off-device from the already verified pack tree. Record the
  image SHA-256 and deployment identity in the authenticated deployment
  metadata. Verify before mounting, and never accept a hash supplied by the
  stub.
* Mount EROFS read-only through a loop device, then bind its `files/` view at
  `/app` or `/usr`. Reuse a mounted image while another process uses the same
  deployment; count references by image and mount namespace.
* Keep extracted directories as a fallback when EROFS/loop mounting is not
  available. Do not silently expose a partial tree: report mountd unavailable
  for that launch and use proxy translation.

In the Tux Racer content sample, LZ4-HC EROFS was 47,095,808 bytes versus
49,300,206 extracted file bytes, a 4.5% reduction. `mkfs.erofs` took 1.26 s
on the host. VM mount and lookup support passed. Directory transfer to the VM
took 0.30 s and image transfer took 0.07 s over the local QEMU transport;
these are transport/materialization controls, not Android package install
times. Device-side runtime image generation is not proposed.

## Mountpoints and policy integration

The image needs empty `/app` and `/usr` directories. Add them through the
device root-files mechanism selected by the coordinator, and preserve normal
Linux paths inside the stub view. The v46 experiment cannot create them in
place because the root is read-only. Do not patch framework code or alter
zygote behavior.

Run mountd in the privileged MatonOS daemon zone. The exact policy and build
integration are listed in [SHARED-CHANGES.md](SHARED-CHANGES.md); this spike
does not edit shared sepolicy or build files. Grant only the operations
needed for pidfd/proc inspection, namespace entry, loop setup, EROFS mounts,
and fixed bind mounts. Stub processes keep their Android app UID, SELinux
domain and inherited seccomp policy; they never receive mount capability.

## Required next measurements

Before building a daemon, make the proxy bypass syscall dispatch for
filesystem operations rooted under the verified mount prefixes while leaving
all other paths translated. Then repeat in the same VM and same Tux Racer
process environment:

1. Native direct open/stat, existing translated proxy, real mount plus direct
   syscall path.
2. Cold launch through the visible Tux Racer menu, with identical cache state
   and at least five runs per mode.
3. App/runtime data as an extracted tree and as EROFS, measuring actual
   on-device staging, mount setup and open/stat latency.
4. Fallback cases: mountd unavailable, stale PID, changed deployment,
   nested bwrap, and `flatpak-spawn --host`.

The completed spike's exact sample counts and limitations are in
`/mnt/data/aosp/out/pc-logs/agents/mountd-spike.final.md`.
