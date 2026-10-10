# Per-app SysV IPC supervisor

Apache-2.0 C library for the Android stub process. This does not enable kernel
SysV IPC, modify AOSP, or implement the session filter/stub integration (part 2).
Each `maton_sysvipc_new()` creates an independent IPC namespace in memory.
Objects and queues grow dynamically up to Linux's default namespace limits:
32000 semaphore sets, 32000 semaphores/set, 500 operations/call, values and undo
adjustments bounded by 32767; 4096 shared-memory segments; 32000 message queues,
8192-byte messages and initially 16384 bytes/queue. These are kernel defaults,
not additional application-specific caps. Queue bytes can be changed with
`IPC_SET`; increases above MSGMNB require CAP_SYS_RESOURCE. IDs use a reusable
15-bit index and a sequence; `*_STAT` accepts an index and returns the full ID.

## Lifetime and transport

Create the supervisor, hand one serving thread a seccomp user-notification
listener, call `maton_sysvipc_serve(s, fd)`. The fd is borrowed. The loop exits
on tracee/listener HUP or `maton_sysvipc_stop(s)`. Stop only writes to an eventfd
and is safe from another thread. Join before closing the listener and calling
`maton_sysvipc_free(s)`. One serving thread per supervisor; other operations
are confined to that thread. Stop is permanent for that supervisor.

The loop snapshots caller data with `process_vm_readv` and writes results with
`process_vm_writev`. It checks notification validity before and after each
copy, before dispatch, and before committing semaphore operations. It never
continues a trapped syscall in the kernel. Credentials come from `/proc/TID/status`:
effective UID/GID, supplementary groups, TGID and effective capabilities. Read,
alter and owner checks follow SysV mode bits and the relevant capability bits.
The supervisor needs permission to access tracees' memory and proc files.
Android SELinux/Yama permissions are part 2's deployment responsibility.

Waiters retain copied semaphore operations/message payloads rather than caller
pointers. A FIFO list is reevaluated after changes and pidfd exit events; completed
waiters can in turn enable others. Timed operations use CLOCK_MONOTONIC.
Invalid notification IDs are dropped without reply, including signalled/killed
waiters. Poll wakes at most every 50 ms while waiters exist to discard invalid IDs;
a nearer semaphore deadline shortens that interval. SEM_UNDO adjustments belong
to a TGID and set, apply on pidfd-observed process exit, clamp to 0..32767, and
are cleared by SETVAL/SETALL or discarded by IPC_RMID.

## Shared memory helper contract

Build `shm-helper.c` as a glibc LD_PRELOAD DSO **before** starting the filtered
workload. It interposes only public `shmat` and `shmdt`. Initial trapped shmat
returns a memfd injected with `SECCOMP_IOCTL_NOTIF_ADDFD | SECCOMP_ADDFD_FLAG_SEND`.
The segment's file size is page rounded; IPC_STAT retains the requested size.
Growing/shrinking the memfd is sealed. SHM_RDONLY injections are actual read-only
file descriptions. The helper fstats, maps with the requested protection,
SHM_RND/address/SHM_REMAP behavior, commits the address, then closes the fd.
Failure cancels the reservation. Commit/inherit verifies the reported mapping's
memfd device/inode through `/proc/TGID/maps`.

All helper metadata uses the direct shmat syscall, with six arguments:

| Call | arg0 | arg1 | arg2 | arg3 | arg4 | arg5 |
|---|---|---|---|---|---|---|
| Initial attach | shmid | requested address | flags | 0 | 0 | 0 |
| Commit mmap | shmid | actual address | flags | MATON_SHM_COMMIT | injected fd | 0 |
| Cancel failed mmap | shmid | 0 | 0 | MATON_SHM_CANCEL | injected fd | 0 |
| Inherit at libc fork | shmid | inherited address | 0 | MATON_SHM_INHERIT | 0 | 0 |
| Constructor/exec reset | -1 | 0 | 0 | MATON_SHM_RESET | 0 | 0 |

The constants are in `sysvipc.h`. Reservations count as attachments until commit
or cancel, keeping IPC_RMID from destroying a segment during mmap. Committed
attachments are released by shmdt or process exit. libc fork hooks register
inherited mappings; the helper constructor releases old attachments after exec.
SHM_REMAP removes fully replaced attachments. Removed but attached segments can
still be inspected/attached by ID and are destroyed at last detach. On i386 the
helper uses direct syscall 397/398 even when glibc normally uses ipc(117); public
symbol interposition bypasses glibc's ipc wrapper.

## ABI and filter requirements

Only x86_64 and i386 are supported. Unsupported architectures/numbers/commands
return EINVAL. Part 2 must reject x32 (`nr & 0x40000000`) in its filter; do not
allow x32 to fall through to a real SysV kernel syscall. An arch check is required
because syscall numbers overlap between architectures.

| Operation | x86_64 | i386 direct | i386 ipc(117) low-16-bit call |
|---|---:|---:|---:|
| semget | 64 | 393 | 2 |
| semop | 65 | — | 1 |
| semtimedop | 220 | 420 (time64) | 4 (time32) |
| semctl | 66 | 394 | 3 |
| shmget | 29 | 395 | 23 |
| shmat | 30 | 397 | 21 |
| shmctl | 31 | 396 | 24 |
| shmdt | 67 | 398 | 22 |
| msgget | 68 | 399 | 13 |
| msgsnd | 69 | 400 | 11 |
| msgrcv | 70 | 401 | 12 |
| msgctl | 71 | 402 | 14 |

Trap exactly native `{29,30,31,64,65,66,67,68,69,70,71,220}` and compat
`{117,393,394,395,396,397,398,399,400,401,402,420}`. Do not invent direct i386
semop or time32 semtimedop numbers; they use ipc. ipc SEMCTL dereferences the
32-bit union pointer; version-0 MSGRCV reads the pointer/type kludge; nonzero
versions use the fifth argument for type. The ipc high version word is decoded
separately from the command's IPC_64 bit.

Wire structures are marshalled by fixed little-endian x86 offsets, independent
of the supervisor's libc: native ipc64 layouts, direct i386 ipc64 layouts,
legacy i386 ipc layouts selected by ipc without IPC_64, and time32/time64
semtimedop. Modern i386 times include their high words. sembuf has the same
six-byte layout on both ABIs; message type is an ABI-sized signed long.

## Tests and builds

```sh
linux/sysvipc/build.sh
# Only for a development sandbox blocking process_vm_*:
linux/sysvipc/build.sh --test-memory-adapter
```

Outputs stay in `linux/sysvipc/out/`. NDK r30 comes from `$ANDROID_NDK` or
`~/Documents/android-ndk-r30`; the script compiles and links the bionic supervisor
for x86_64-linux-android36 with `-Wall -Wextra -Werror`. Host tests need normal
user permissions, not root. The harness forks a filtered child, transfers the
NEW_LISTENER fd over SCM_RIGHTS, execs the workloads, and serves two independent
sessions concurrently. Both sessions use the same IPC_EXCL keys.

The ordinary build runs the real process_vm transport. If an outer seccomp
sandbox denies even a self-targeted copy, it explicitly skips integration tests.
The optional adapter uses `/proc/PID/mem` **only in the host test binary**, never
in the production library, and still exercises actual seccomp notifications,
pidfds, ADDFD, shared mappings, subprocesses and concurrent stop. It does not
validate Android policy or the production memory syscalls.

The build attempts glibc `cc -m32` workloads/helper when multilib is installed.
It also builds a libc-free static ELF32 workload exercising direct syscalls,
legacy ipc commands/structs, time32/time64 and memfd mmap2. A separate ELF32
execution probe distinguishes outer sandbox ABI rejection. Independently,
`abi-test.c` checks compat wire decoding, high timestamp words and old/IPC_64
structure sizes with mocked notification/memory transport.

LTP is not vendored. Set `LTPROOT` or install case binaries under /opt/ltp,
/usr/lib/ltp or /usr/libexec/ltp; the script runs installed SysV case binaries
through the harness. Without LTP the original tests mirror the behaviors in
these case families (not copied assertions or claimed exact LTP equivalence):

- semget01/02/03: private/key creation, exclusivity, size and lookup errors.
- semop01/02/03/04 and semtimedop01/02: wait/post, zero waits, multiple operations,
  rollback on NOWAIT, duplicate indices, bad pointer/index, timeout, removal,
  killed waiter and undo at process exit.
- semctl01..09: STAT/STAT_ANY, SETVAL/SETALL, GETVAL/GETALL, GETPID, waiter counts,
  mode changes, INFO, out-of-range values and removal.
- shmget01..05, shmat01..03, shmdt01..02, shmctl01..05: metadata, cross-process
  visibility, attachment counts, readonly attach, map collision/cancel, REMAP,
  threaded attaches, LOCK/UNLOCK and RMID after last detach.
- msgget01..03, msgsnd01..04, msgrcv01..08, msgctl01..06: queue creation,
  positive/negative/EXCEPT type selection, size errors/truncation, NOWAIT,
  blocking send/receive, byte limits, copyout fault and removal wakeup.

## Known differences and integration limits

This library is not a drop-in replacement for every Linux IPC edge case:

- SHM_HUGETLB and MSG_COPY are unsupported and return EINVAL. SHM_INFO reports
  allocated pages, but resident/swapped pages and swap counters are zero; memfd
  does not expose equivalent namespace counters. SHM_NORESERVE uses memfd's
  normal sparse allocation rather than kernel SysV reservation accounting.
- SHM_LOCK/UNLOCK pin/unpin a supervisor mapping. They enforce the caller's
  RLIMIT_MEMLOCK and per-UID accounting within this supervisor, but are also
  constrained by the supervisor's own mlock limit. Unlike shmem_lock, this
  eagerly faults pages. No across-supervisor mlock accounting is attempted.
- shmat needs the in-process helper: only it can map the injected memfd.
  A process without it mapped (static binaries, raw-syscall users such as Go,
  musl programs where glibc's $LIB is not expanded) gets ENOSYS from shmat,
  as from a kernel without SysV shm (checked via /proc/<pid>/maps on each
  plain attach). shmget/shmctl, semaphores and message queues still work.
  With the helper, the injected fd is mapped and shmat returns the address.
- Inheritance is tracked through libc pthread_atfork hooks. Raw fork/clone,
  vfork and clone-created address-space-sharing processes are not automatically
  accounted for. Exec cleanup requires the helper to remain preloaded; an exec
  that removes it leaves attachment records until process exit. Direct munmap
  bypassing shmdt likewise leaves records. Process exit always cleans records.
- Partial SHM_REMAP overlaps are rejected with EINVAL. Full replacements and
  exact-address remaps are supported. Helper maps all segments page rounded;
  raw fragmentation/unmapping outside the helper is not tracked.
- SEM_UNDO is shared within a TGID, not across separate TGIDs made with
  CLONE_SYSVSEM. Fork does not inherit undo, as in ordinary Linux fork.
- Credentials/capability interpretation assumes tracees and supervisor have
  compatible PID/user namespaces. Cross-user-namespace capability attribution,
  LSM hooks, audit and configurable IPC sysctls are not emulated. Namespace
  limits are the documented Linux defaults, not host /proc sysctl values.
- A notification can be invalidated immediately after the final validity check;
  seccomp's check/copy/respond operations cannot form a kernel IPC transaction.
  ADDFD uses SEND to avoid the fd-injection/reply race. Other operations can
  commit just before the caller is killed/signalled, as documented by seccomp.
- The serving process must be able to open pidfds and access tracee memory.
  The Android stub is not generally the Linux tracee's ptrace ancestor. Part 2
  must arrange the existing trusted relationship/policy; do not silently replace
  memory transport or loosen permissions in this library.

Kernel sources were read as behavioral specifications only; none were copied:
[sem.c](https://github.com/torvalds/linux/blob/master/ipc/sem.c),
[shm.c](https://github.com/torvalds/linux/blob/master/ipc/shm.c),
[msg.c](https://github.com/torvalds/linux/blob/master/ipc/msg.c), and Linux UAPI
structure definitions in the host headers. No android-shmem or LTP code was reused.

## Integration (part 2)

- **Filter:** `linux/flatpak/session-binder-filter.h` `session_filter_sysvipc()`
  returns `SECCOMP_RET_USER_NOTIF` for the SysV numbers above (x86_64 native,
  i386 compat incl. `ipc`), installed by `session_enter()` after the Binder
  filter. Both session children (portal, app payload) install it, so one app
  has several listeners.
- **Hand-over:** `session_sysvipc_handover()` (`linux/flatpak/app-session.h`)
  sends the listener over `SCM_RIGHTS` to `/data/matonos/linux/tmp/<uid>/bus-sysvipc`
  and closes its copy. Without the socket the calls keep failing with ENOSYS.
- **Supervisor:** `linux/dbus-broker/sysvipc-host.c`, started next to the
  session bus in the stub (`maton_broker_jni.c`). One supervisor per app serves
  every listener (`maton_sysvipc_add_listener`), so all of an app's sandboxes
  share objects and no other app can reach them. It accepts only the app's own
  UID and only a seccomp listener fd. Objects live until `IPC_RMID` or the end
  of the stub's bus.
- **shm helper:** freestanding build (`tools/build-flatpak-helpers.sh`, NDK clang
  `--target=x86_64-linux-gnu` / `i686-linux-gnu`), shipped in the Flatpak APEX
  as `etc/sysvipc/libmaton-shm-<abi>.so`; `matonos-bwrap` binds it under
  `/run/matonos/{lib/<multiarch>,lib64,lib32}/` and sets
  `LD_PRELOAD=/run/matonos/$LIB/libmaton-shm.so`.
- **Architectures:** decoding is x86 only; arm64/riscv64 need their generic
  syscall table in `decode()` and in the filter (the filter installs nothing
  there today, so those calls stay ENOSYS).
- **SELinux (enforcing gauntlet):** stub domain → `matonos_linux_app`
  `process ptrace` (process_vm_readv/writev) and `/proc/<pid>` reads at the same
  MLS level; app domain `connectto` the stub on `bus-sysvipc`; a dedicated file
  type for `etc/sysvipc/*` with read/map/execute for `matonos_linux_app`.
