# Installer v1 plan and ownership

Status: app policy and primitive API are prototyped; the API client is still a
stub and no destructive backend exists. The product build is currently
non-A/B. Do not begin its conversion until the boot layout question in
[AB-LAYOUT.md](AB-LAYOUT.md) is resolved.

## Ownership boundary

`apps/installer` owns the versioned install plan: minimum capacity, GPT
partition table and sizes, LP group/member sizes, source and destination
mapping, user-file selection, exact operation order, and user-facing choices.
The app submits one versioned primitive operation at a time, observes progress,
and decides whether/how to continue. v1 keeps its 16 GiB minimum, 10.5 GiB
super, 5 GiB groups and A→B/data/dormant sequence in the app. Future dual-boot,
custom-size, repair and other workflows are app-only policy additions when
they can be expressed with existing primitives.

The install service is a thin privileged executor, not an installer planner.
Its generic API supports drive/partition enumeration, app-described GPT,
app-described LP metadata, partition format, live or target partition copy,
target clone, bounded file writes to EFI partitions, and a file-level user-data
transfer with app-supplied include/exclude paths. It accepts a single operation
request with `apiVersion`, `targetDiskId`, and a typed `operation`. It exposes
progress/cancel for that operation. It has no v1 layout name, min-size rule,
slot sizing, or workflow state machine. See [AB-LAYOUT.md](AB-LAYOUT.md) for
the v1 request contract.

Service safety is not app policy and cannot be relaxed by a request: only the
live image may invoke operations; the bridge authorizes the installer package
by its pinned signing certificate; every destructive call enumerates devices
again immediately before execution; the target must not be the live medium,
mounted, in use, or read-only; all extents and file paths are bounded to the
selected disk/declared partitions; malformed or unverifiable state fails
closed. The service accepts no caller-supplied `/dev` path. A target becoming
unsafe between operations stops the plan. The live boot marker and bridge
caller authorization are trusted service/bridge state, never request fields.

The native core currently defines the request types and validation boundary,
including per-operation validation and a fresh-snapshot dispatch wrapper.
Hardware execution (GPT/LP/block writes, file copy, channel registration,
progress cancellation) is not implemented yet. The Expo app uses a preview
stub and does not modify storage. No AOSP patches are planned or present.

## Build conversion and integration order

The present build sets `AB_OTA_UPDATER := false`, uses unsuffixed logical
partitions and has an unslotted `super`; its live image packs these existing
images into a compact super. Current images total about 1.8 GiB (system 576
MiB, system_ext 208 MiB, product 545 MiB, vendor 457 MiB, ODM 4.1 MiB), not a
future update budget.

1. Resolve the boot partition contract. The current systemd-boot investigation
   supports one shared XBOOTLDR but has not proved physical `boot_a`/`boot_b`
   partitions or finalized the shared-tree alternative. Test the exact boot
   manager before modifying the product layout.
2. Convert the regular installed build to A/B: suffixed logical partitions,
   slot-aware first-stage fstab, boot-control HAL/VINTF and loader slot
   selection. Avoid AOSP patches; if stock interfaces cannot support this,
   stop and report the blocker under the zero-patches rule.
3. Adapt live packaging to contain only `_a`, retain compact exact-fit super,
   and boot with `androidboot.slot_suffix=_a` and
   `androidboot.matonos.live=1`. After each layout change request a coordinated
   image build and verify fresh live boot.
4. Implement the NDK service primitives and stable `install` channel using
   shared integration requests in `SHARED-CHANGES.md`; keep every destructive
   primitive independently guarded. The app continues to own the policy.
5. Connect the app channel client to submit the generated v1 operations in
   order. Keep the stub for UI preview until integration is available.
6. Exercise the install on a disposable target: app writes GPT and metadata,
   copies live A to target A, clones A to B, transfers allowed user files, then
   records installer dormancy and hands off to reboot.
7. Boot the target by itself; check both slots, loader fallback, no live marker,
   installer package disabled/no launcher entry, service absent, and user files
   available after reboot. Never test with hot-patching.

## V1 ordered operation plan

The app planner emits the following sequence. Each row is an independent
request and service safety is rechecked before every destructive operation.

| Step | App operation and intent | Failure / rollback | Progress |
|---|---|---|---|
| 1. GPT | After confirmation showing model, capacity, 16 GiB minimum and full erase warning, write the app-generated GPT layout. | GPT replacement cannot be rolled back; on failure stop, keep the live source untouched and allow a fresh plan after re-enumeration. Cancellation only between operations, never during an unsafe partial write. | Per-operation byte/sector progress, then fresh partition enumeration. |
| 2. Prepare | Format ESP/XBOOTLDR and userdata as declared by app; create the specified LP metadata/groups. | Stop at the failing operation; never assume partially formatted contents are valid. Rebuild/reinitialize target on retry. | Format/metadata progress and verification. |
| 3. Copy A | Copy the live image's `_a` logical images to target `_a`; write the live boot assets under the A boot tree. | A remains unselected until every item verifies. A failure blocks cloning and data transfer; retry that primitive after fresh checks. | Partition/file name, bytes, verification. |
| 4. Clone B | Clone every A logical partition and its boot files into B and verify equality. | Do not mark B bootable or pending until all members match; A remains first boot choice. | Member, bytes and hash/readback result. |
| 5. User files | Request a decrypted, file-level transfer from live user-visible paths to target userdata with app-supplied excludes. | Never copy raw `/data`, metadata, or FBE keys. Partial restore is reported by category and can be retried without changing slots. | Files/bytes copied, excluded and skipped counts. |
| 6. Dormant handoff | App writes a versioned installed-image dormancy marker consumed by installed first-boot integration; target boot entries set `androidboot.matonos.live=0`. Installed first boot disables the privileged installer package for every user; init leaves its service disabled. | Do not reboot/claim completion if the installed hook cannot guarantee dormancy. Installer stays disabled by default and only live-mode integration enables it. | Marker written/verified; later fresh boot checks package and service state. |

Once all operations verify, loader/boot-control state selects A and keeps B
identical as fallback. Any active-slot publication waits until A and B are
complete. Installation erases the target and cannot restore prior disk data;
the original live medium remains untouched.

## User files and FBE

Live `/data` is RAM-backed and encrypted; raw filesystem copying, fscrypt
policy copying, or copying `/metadata`/vold keys cannot safely migrate keys to
the installed device. V1 asks the service for a file-level export from the
decrypted user view and includes Documents, Download, Pictures, Movies and
Music. Direct pre-first-boot writes to the target media tree are not proven to
receive the installed user's fresh fscrypt policy. The safe alternative is an
encrypted staging bundle on target userdata with a one-time recovery secret
shown to the user, then an installed first-boot importer after fresh keys
exist and the user unlocks; remove the bundle after restore. That importer and
secret UI remain open implementation work. Exclude private application state,
package-manager records, installer APK/data, keys/secrets, ADB keys, system
state, caches and live-only state. Private app data requires each app's
supported backup/restore path; no complete `/data` clone is claimed.

## Installer dormancy and service

The installer remains a normal privileged system app in all images with its
normal `SYSTEM_BRIDGE` permission and exact bridge certificate allowlist. The
app is disabled by default, enabled only in live mode, then disabled for all
users by installed first-boot integration. The service is ODM-bundled,
disabled by default, additionally requires live mode for every operation, and
must never block Android boot. No bridge permission bypass is used.

Shared app registry, prebuilt import, bridge certificate/target allowlist,
bundle registration, SELinux additions to the fixed policy files, live image
marker, service init and installed first-boot dormancy hook are specified in
[SHARED-CHANGES.md](SHARED-CHANGES.md); this low-priority task does not edit
those shared files.
