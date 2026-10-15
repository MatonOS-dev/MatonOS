# Fixed-identity extra-data extraction: deferred

Investigation on branch `extra-data-installer`, 2026-10-15. No runtime change
is shipped. Chrome remains an example of the general extra-data failure;
there is no per-app exception.

## Confirmed call path

`install/linuxd/FlatpakPublish.c:deploy_ref()` runs the wrapper's offline
`install --no-pull` as system. The wrapper clears its environment, including
any caller-supplied `FLATPAK_BWRAP`. Therefore setting that variable only in
linuxd would have no effect: the wrapper must explicitly select the helper
for authenticated offline deployment, after its existing root-path checks.
No new installation-path exception is necessary for this selection.

The checked-out Flatpak fork's `common/flatpak-dir.c:apply_extra_data()`:

1. Opens the runtime and app directory descriptors and binds them read-only
   at `/usr` and `/app`.
2. Binds the writable extra directory descriptor at `/app/extra`.
3. Sets that directory to 0700 immediately before spawning bubblewrap.
4. Runs `/app/bin/apply_extra` in its restricted sandbox, with no `/proc`.
5. Canonicalizes permissions before inspecting the child's exit status.

The directory does not exist early enough for a simple pre-deploy linuxd
chown. A helper can operate on the supplied FD, including reopening an
O_PATH descriptor. It would need to hand over existing downloaded files,
not only the containing directory. The helper must reject non-system UID
and non-linuxd domain callers before handling any FD or dropping identity.

`flatpak_canonicalize_permissions()` receives uid/gid -1 when the caller is
system, so it does **not** restore ownership. A supervisor must reclaim the
entire extra tree before returning to Flatpak, including failure paths.
Flatpak canonicalization is not an ownership rollback mechanism. The
supervisor must handle mode-000 directories, symlinks without following them,
malicious file types and extractor descendants before walking this tree.
The existing later publication/sanitization step is too late for this.

## Enforcing-policy blocker

The actual platform checkout's `system/sepolicy/private/domain.te`, around
line 2040, has a neverallow covering `no_x_file_perms` on `data_file_type`
for every domain except appdomain and the debug overlay remounter, with
exceptions for a short list of Android-owned code types. Our
`matonos_flatpak_deployment_file` and `matonos_linux_code_file` are
`data_file_type`, not those exceptions. `no_x_file_perms` includes execution
and executable mappings. A new native non-app extractor domain therefore
cannot execute the runtime shell or extraction tools from those deployments.

The historical data-execution exception in AGENTS.md is not present in this
platform checkout. Adding/reinstating a platform exception for a new domain
would change AOSP policy and needs authorization under the zero-patches rule.
No such exception was added. Assigning appdomain just to evade the neverallow
would import app privileges without establishing an app identity or a
reviewed security boundary; that is not an acceptable shortcut.

The existing store helper transitions to `matonos_flatpak_installer`, whose
neverallows explicitly prohibit writes to deployment, store and code labels.
Using that same domain for extraction breaks the staging/publication zone
boundary. Merely dropping UID leaves SELinux permissions unchanged. Running
vendor code in `matonos_linuxd` is likewise unacceptable even at UID 2902.

## Required design before implementation

If a future authorized design retains native bubblewrap, it needs these
scoped policy surfaces in the existing fixed policy files:

- A supervisor entry point callable only by linuxd, retaining ownership and
  relabel authority, with no vendor instructions in that process.
- A separate payload entry point/domain after setgroups(0), setresgid(2902)
  and setresuid(2902). No Binder, network, staging/store writes, Android app
  data or deployment/code writes. Enforce those exclusions with neverallows.
- A temporary extra-data label for this tree alone. Payload may mutate this
  label; runtime and app labels remain read-only. Supervisor alone relabels
  and reclaims the tree on success and failure. Restore original labels
  before normal deployment sanitization/sealing.
- Inherited-FD and capture-pipe use, only from the supervisor/linuxd; read and
  executable access to the pinned runtime/app; user-namespace setup and
  mounts on private tmpfs targets only. Derive precise setup permissions
  from the actual bubblewrap implementation and validate with selinux_policy.
- An explicitly authorized solution to the data-execution neverallow above.

A patch-free alternative would need an installer-identity worker using the
v3 in-process loader rather than kernel exec of deployment files. This is a
separate design: do not reuse an ordinary app UID, grant system identity to
vendor code, or claim the existing stub path already provides this worker.

No helper/APEX/policy modules need rebuilding for this documentation-only
result. No AOSP builds or VM mutations were performed.
