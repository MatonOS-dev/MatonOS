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

The native flatpak-portal has been dropped. The session bus and portals move
into the stub app via dbus-java, over the compositor's delegated session
directory; linuxd launches only the initial verified stub session. Until
dbus-java lands the compositor owns the broker and portal, so the earlier
trusted-portal-parent authentication no longer applies.

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
forbids writing exec_type at all. Flatpak code comes from stock deployments
published by linuxd after signed OSTree verification. The launch chain applies
the dedicated `matonos_app_code_exec`/`matonos_runtime_exec` labels to deployed
code; the stage-only helper has no mount or filesystem permissions. The payload
is entered through the `matonos-app-exec` launcher's dyntransition at the
stub's per-app MLS level, never a file entrypoint, and no capability enters the
sandbox. No assertion was disabled and no AOSP project was modified.

The sandbox chain is also asserted binder-free. Stock `domain.te` grants every
domain `system_server:binder call` and `rw_file_perms` on binder_device/
hwbinder_device chr_file; private policy cannot subtract those, so the
neverallows carve out exactly that baseline and forbid every other Binder,
binder-device and service-manager access. The mandatory seccomp filter rejects
all Binder ioctls; that is the real enforcement. An app can never re-enter the
bwrap setup domain or execute bwrap (nested sandboxes are not supported; the
stub owns D-Bus and portals via dbus-java).
