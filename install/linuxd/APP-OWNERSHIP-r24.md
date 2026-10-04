# r24 app ownership prototype — enforcing release blocked

This change is reviewable source, **not an enforcing-ready r24 launch path**.
The independent native/APK/policy checks are documented in
`out/pc-logs/agents/codex-app-owns-result.md` in the shared checkout.

## Boundary and lifetime

The signed Android stub supplies a process-lifetime pipe reader. Its writer
is held only in a static field in that stub process, across activity/window
recreation. The compositor captures Binder UID/PID before clearing identity.
The bridge repeats `ownsStub(uid, ref)` and sends UID/PID plus the pipe to
linuxd. Legacy launches without a verified stub are rejected.

The exec'd wrapper pins the existing Android cgroup directory and checks the
stub's proc UID, SELinux range, membership and live pipe. It never creates a
substitute cgroup. This AOSP checkout uses
`/sys/fs/cgroup/apps/uid_<uid>/pid_<pid>`, not a direct `uid_<uid>` child.
A child enters that group before executing Flatpak, drops supplementary
system groups, switches all UID/GID slots to the stub UID/GID, clears all
capabilities, changes to `matonos_flatpak_run` with the MLS range computed from
the verified UID (never from a caller-supplied string), and installs NNP plus
a Binder-ioctl filter. Children inherit both cgroup and filter. Only inet and a
granted controller gid remain; device=all is denied.

The wrapper supervisor stays outside that cgroup. It kills the pinned group
through `cgroup.kill` when the stub pipe reaches EOF, and waits for the entire
payload group (excluding the stub) to empty before reporting Linux exit.
Every stub window polls launch status and finishes on exit. This requires
cgroup v2 and the kernel's `cgroup.kill`; no legacy-layout fallback is claimed.

The native flatpak-portal stays in linuxd's domain and outside the payload
cgroup. Its UID is the stub UID so stock Flatpak can inspect its peer's
`/proc/<pid>/root/.flatpak-info` without forbidden host CAP_SYS_PTRACE. It
retains trusted launcher capabilities. Nested wrappers read identity from
this exact portal parent's immutable environment, rather than accepting
ownership supplied in a Spawn request, and enter the same cgroup/credentials.
MAC must deny sandbox ptrace of that trusted portal; permissive boot is not
an isolation test. The portal's environment is readable to its trusted
wrapper (dumpable), protected from payload manipulation by that MAC boundary.

Homes are `/data/matonos/linux/apps/<uid>`, mode 0700, owned by that UID/GID,
with a private data type and the stub's MLS categories. A system-owned
`<uid>.owner` record binds the UID to the Flatpak ID; recycled UIDs cannot
adopt another app's retained home. No automatic shared-HOME migration or
uninstall reclamation is implemented. UID collisions fail closed and need
an offline purge/migration. Current implementation supports user-0 normal
app UIDs (10000–19999); other users fail closed. Init preserves dynamic home
ranges instead of recursively resetting them to s0.

Session transport directories are owned by the compositor, group=stub UID,
mode 0750; the broker may bind its bus while other apps cannot traverse them.
Display sockets are system:stub-UID, mode 0660. The supervisor restores its
ownership for cleanup. Broker native-directory validation checks the
compositor owner and exact session app group. Broker peers are the verified
app UID or the trusted native side, with the existing portal PID gate.

Activity onStop/onStart sends the window's stopped state to the compositor.
Wayland gets xdg_toplevel suspended and no frame callbacks or presentation
retries while stopped. Resume schedules a frame. Android's cached freezer
writes cgroup.freeze for the stub group, so all joined descendants freeze.
Window X remains the existing graceful close. Recents uses stock removed-task
process killing, which can be deferred by Android while a process remains
foreground; this change does not replace Android's task/multiwindow semantics.

## Stock SELinux constraint — resolved

`domain.te` forbids non-appdomain execution of data_file_type files, and also
forbids writing exec_type at all. That constraint is resolved by
`CODE-STORAGE-r24.md`: the installer builds read-only images whose superblock
is `matonos_code_fs` (`contextmount_type`, `fusefs_type`) and whose inodes are
labelled `matonos_app_code_exec`/`matonos_runtime_exec` via `context=` at
mount time. The inodes are an exec_type (so the /data execution neverallow
does not apply) and the images are read-only (so the exec_type write
neverallow cannot be bypassed). Per-app code and volume are mounted only by the
privileged `matonos_mount_helper`, outside the sandbox, which `setns()`es into
the sandbox mount namespace; the payload is entered through the
`matonos-app-exec` launcher's dyntransition at the stub's per-app MLS level,
never a file entrypoint, and no capability enters the sandbox. No assertion was
disabled and no AOSP project was modified; the policy compiles with
`sepolicy_neverallows` and the merged `secilc` both passing.

The sandbox chain is also asserted binder-free. Stock `domain.te` grants every
domain `system_server:binder call` and `rw_file_perms` on binder_device/
hwbinder_device chr_file; private policy cannot subtract those, so the
neverallows carve out exactly that baseline and forbid every other Binder,
binder-device and service-manager access. The mandatory seccomp filter rejects
all Binder ioctls; that is the real enforcement. An app can never re-enter the
bwrap setup domain or execute bwrap (nested sandboxes go through
flatpak-portal under linuxd).
