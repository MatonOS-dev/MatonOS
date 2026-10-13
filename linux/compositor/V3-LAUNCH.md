# Linux layer v3 launch from the stub

The generated stub owns its compositor, session bus, DNS forwarding and Linux
process tree. The trusted compositor service coordinates identity and the resolver address; it
requests `ILinuxd.launchSpec` through `getOwnedFlatpakLaunchSpec` in the System
Bridge. linuxd validates the same installed app ref, runtime UID, X11 capability
and metadata as the former graphical launch. It returns `{ok, argv, env, root,
installation, runtimeInstallation}`. No app process is spawned by linuxd and no
privileged cgroup, Downloads or lifeline descriptor is returned in the spec.
The install/query CLI still runs in linuxd; installation layout is unchanged.

`stub.V3Launcher` gets the host's `nativeLibraryDir` from PackageManager and
starts `libmatonos-v3-loader.so` with ProcessBuilder in the stub UID. The host is
already visible in the generated stub's queries. The shared library loader and
PackageManager enforce the image host/update trust. The loader is a freestanding
x86_64 executable, packaged under the APK native-lib name. Gradle preserves its
ELF. Updates extract it normally; build-apps.sh stages the identical ELF for the
image's `/system_ext/priv-app/MatonWaylandHost/lib/x86_64/` nativeLibraryDir.
It must be executable. JNI DSOs continue loading from the stored APK entries.

The stub makes `<dataDir>/home`, its XDG subdirectories and cache. TMPDIR,
FLATPAK_DOWNLOAD_TMPDIR and caches use the stub cache. The existing per-app
runtime directory supplies Wayland, the bus, Pulse and X11. A local resolv.conf
and X11 directory are mapped into the loader's initial virtual view. No
`--android-filter` is passed: the stub's actual filter is already inherited.
`--parent-pid` sets PR_SET_PDEATHSIG=SIGKILL and checks the parent after setting
it, rejecting the fork/exec parent-death race. Virtual exec retains the loader.
Every Linux descendant also remains in Android's app process cgroup. The
existing last-window shutdown remains; explicit management stop uses the bridge
force-stopping the certificate-verified stub package, rather than linuxd PIDs.

Output from both streams goes to MatonV3. The per-process V3Launcher status
reports loader exitCode; StubActivity observes it for every window. Sessions
still use lifeline EOF for cleanup and check EOF before replacing an old PID.
Relaunch stubs after updating the host: pre-alpha Binder interfaces are changed.

Policy gives untrusted_app_all read/search/mmap/execute on the existing code and
deployment types, plus traversal of the Linux storage root. It grants no
execute_no_trans, writes or neverallow exceptions. The loader filter denies
Binder ioctls before its raw-syscall exemption, while allowing the exact
DMA-BUF sync/fence encodings; the Java stub parent keeps its Binder access. The matonos_bwrap,
matonos_flatpak_run, matonos_app_launch and matonos_linux_app domains and their
exec labels are deleted. APEX no longer ships the old bwrap/app-exec helpers.
Archived source fixtures under linux/flatpak/tests/legacy-v2 are unshipped.
The unused data_exec_exempt_domain patch and its preflight entry are retired.
The coordinator must reverse that already-applied patch in the AOSP sepolicy
checkout before building; deleting the patch file alone does not reverse it.

Verification here: NDK loader build, host loader regression suite, NDK loader
parent-death/stale-parent test, C/C++ syntax checks with warnings as errors,
compositor Java/AIDL and bridge Java compilation, native build script, Flatpak
helper builds, DNS-monitor test and Downloads Binder protocol check. These do
not prove a graphical launch or enforcing policy. APK assembly is blocked by
read-only SDK files outside this sandbox; worktree preflight lacks 12 existing
APK inputs. No adb, VM or image/policy build was run.

Open integration checks:

- Fresh enforcing image: Tux Racer install, graphical Wayland/X11, DNS, audio,
  close/reopen, force-stop and disappearance of the whole Linux process tree.
- Stock Flatpak writes/locks against the current installations at an app UID.
  Keep code read-only; do not add broad write grants or neverallow exemptions.
- The old privileged Downloads bind-fd grant and its launch-time prompt are absent; it cannot
  be handed to this launcher. Downloads needs app-openable storage/portal
  integration before claiming the old grant is supported. Do not show a grant
  as effective merely because the retained consent protocol succeeds.
- Controller inventory remains empty, as before. Stock Flatpak portal spawning,
  shm preload injection and v3 IPC remain follow-up work; the old privileged
  portal supervisor is gone. Check Desktop portals on the retained private bus.

Coordinator VM steps are in out/pc-logs/agents/v3-stub-result.md.

Round 8: the Java stub binds UDP/TCP at its UID-derived address on 5353 before
opening a session. There is no DNS socket FD relay or privileged linuxd bind.
V3Launcher passes that exact address/port via --dns-forwarder while resolv.conf
retains logical port 53. See DNS-FORWARDER.md for translation and validation.
