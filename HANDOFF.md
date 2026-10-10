## Brave stub icon repair (2026-10-09)

Recovered the old lookup from commit 2206b9f, removed in d9fac76 during
AppStream cleanup. Brave exports SVG but bundles its PNG inside
`files/share/app-info/icons/flatpak/<size>/com.brave.Browser.png`.
Restored lookup of those deployment-local PNGs and app-info media thumbnails
after the exported hicolor PNG lookup, using FlatpakIcon.h from FlatpakManager.c.
No shared AppStream installation or parser was added. Candidates must remain
inside the deployment, be regular files <=256 KiB, and have a PNG signature.
Invalid candidates fall through to the next location instead of masking it.

Bumped STUB_VERSION_CODE from 11 to 12 so existing installed stubs regenerate
through the bridge's periodic reconcile after the updated image boots.
Validation: host flatpak-icon-test passed SVG-only export/bundled PNG,
export precedence, media fallback, oversized/non-PNG rejection and an external
symlink rejection; NDK API-36 FlatpakManager syntax check with warnings as errors
passed; stubgen host tests and diff checks passed. No VM is connected, so no
visual/device verification is claimed. Rebuild linuxd and the bridge/image.

## Desktop app selection (2026-10-09)

Further user additions: Thunderbird 24.0 (`net.thunderbird.android`, vc 33)
and BlackyHawky Clock 2.33 (`com.best.deskclock`, vc 2037), registered as
MatonThunderbird and MatonClock. No Fossify Clock was added. Both APK hashes
and signing certificates match F-Droid's current index; downloaded APKs were
verified with apksigner. Thunderbird includes x86_64; Clock has no native ABI
restriction. Soong optional library lists match their manifests, including
Thunderbird's optional Samsung multiwindow library (Soong filters unavailable
optional libraries). Both pass preprocessed APK checks without the skip flag.
fetch-apps.sh passed with all four pinned apps; bpfmt and diff checks passed.

AOSP Camera2 is already explicitly selected in apps/apps.mk and present in
product staging. Its CameraLauncher alias is exported and has MAIN/LAUNCHER.
The stock boot receiver hides that alias if Android reports no front, rear or
external camera feature. User explicitly accepted this behavior; no Camera2
source or feature spoofing was changed. No VM/runtime validation was run.

User requested removal of Calendar, Clock, Contacts, Messaging and Phone UI,
hiding Maton Wayland, and bundling Android VLC from F-Droid. Removed the
explicit Calendar/Contacts/DeskClock additions in apps/apps.mk. Added
MatonSystemBridge module overrides for Calendar, Contacts, DeskClock,
Dialer and messaging so inherited AOSP packages are also excluded.
Providers, ContactsPicker, TeleService and other framework telephony support
remain. Cleared only those five generated product app directories from the
staging tree so stale APKs cannot carry into a new image.

The compositor manifest has no application LAUNCHER category; its internal
MainActivity is not exported. The Linux host library/service and generated
stub launcher entries remain available.

Android VLC is registered as MatonVLC, pinned to F-Droid's x86_64 release
3.7.1 / 13070108 in apps/apps.lock. It is a normal product app with the
original F-Droid signer, so repository updates can replace it. This change
requires rebuilding the image; the user's QEMU is down.

Verified the downloaded APK's SHA-256 and signer against the current F-Droid
index, confirmed package/version/x86_64 ABI with aapt2, added its two optional
AndroidX window libraries, and enabled skip_preprocessed_apk_checks only
because its JNI libraries are compressed. fetch-apps.sh passed and extracted
its x86_64 native libraries for image loading. Both modified Android.bp files
pass bpfmt; the compositor manifest parses with no launcher entry and retains
the shared library/service. No image build or app runtime test was performed.

## Last-window shutdown for close/reopen (2026-10-09)

User authorized closing the Linux process with the app. The user's QEMU
exited; there is no VM for validation and none was started by this change.

StubActivity now tracks all window activities in its process and terminates
that process when the final activity is destroyed by an explicit finish.
Configuration recreation and non-finishing destruction do not end it;
closing one of several windows keeps the other windows alive. The existing
supervisor observes lifeline EOF and kills/reaps its pinned Android
per-process cgroup (including the Linux payload and portal). A new host PID
uses a different cgroup, so old cleanup cannot kill the newly opened app.
StubService now returns START_NOT_STICKY to prevent headless resurrection.

Focused javac compilation passed for PerAppRuntime, StubActivity, StubService
and WindowLifetime against SDK 36 plus existing generated host/AIDL classes.
WindowLifetimeTest passed last-close, multiple-window, recreation and duplicate
callback cases. No APK/image build or native/device close/reopen test claimed.
Rebuild MatonWaylandHost/the image; no stub manifest regeneration is needed
for this shared-library Java change. Verify Firefox and Brave close/reopen,
multiwindow close, minimize/restore and absence of orphan payload processes.

## Firefox reopening failure (2026-10-09)

Firefox and Brave showed "Application is running, but no window has appeared"
on the current VM while Firefox's payload remained alive. A full Firefox
force-stop and launch restored its rendered window (verified by screenshot:
`out/pc-logs/install-background/firefox-restart-screen.png`). The screenshot
shows Firefox's session recovery page; no profile/session files were removed.
The earlier Mesa library/render-node mapping check did not prove a visible
window. Do not describe that check alone as restored graphics.

Found a host handoff gap: PerAppRuntime delivered native toplevel events only
once and never replayed existing windows to a recreated/reopened activity.
It now records live IDs/dimensions, hands one existing window to a new root
or the exact requested ID to a secondary activity, removes closed windows,
and clears the matching activity callback on destruction. StubActivity uses
this handoff. Focused javac compilation and RuntimeWindowHandoffTest passed
against SDK 36 and the existing host classes. This source change is not
deployed; rebuild the compositor APK/image and test reopening/multiple windows.

## Standard GL runtime extension (2026-10-09)

The current VM is `emulator-5554`. Installs and Firefox launches work.
Both graphics regressions shared a missing Mesa runtime extension: only
base Freedesktop and KDE platforms were deployed, and Firefox's sandbox
`/usr/lib/x86_64-linux-gnu/GL` was empty despite `/dev/dri` being available.
Installed signed `org.freedesktop.Platform.GL.default/x86_64/25.08` into
`/data/matonos/linux/runtime/10093` with the normal Flatpak wrapper as UID
1000. Restarted Firefox; its maps now contain Mesa EGL and GPU render-node
mappings. No image or system APK was replaced. X11 acceleration still
needs a restarted-app verification.

User requested GL to be standard. `install/linuxd/FlatpakPublish.c` now
reads the authenticated base runtime metadata's GL extension declaration,
prefers `versions` over legacy `version`, and stages default Mesa under
the installing stub UID. Offline publication imports it with untrusted
pull-local plus GPG verification, seals it, and deploys it into the shared
runtime installation before deploying the base runtime and app. Required
GL failures prevent success. Console runtimes without a GL declaration
continue without Mesa. KDE's runtime branch is not used as the GL branch.

Validation: host and production C syntax checks with warnings as errors;
`gl-runtime-test.c` verifies metadata/architecture/branch handling;
`gl-publish-test.py` verifies offline deployment order and propagation of
missing-ref/signature/deployment failures using a fake CLI. These do not
replace real GPG or image integration tests. Source changes need the next
image build; the VM repair is already applied.

Other diagnosed issues remain: VLC's
launch log reports `bwrap: Can't mkdir parents for /data/cache: Permission denied`.
Earlier install-completion crashes show a race between activity-owned
download work stopping InstallService and its foreground promotion.

## Build policy fix (2026-10-09)

The image build failed because `matonos_linux_stub` lacked `coredomain`,
so Treble forbade linuxd and the payload domain connecting to its sockets.
Added that attribute in `systembridge/sepolicy/system_ext/private/matonos_system_bridge.te`,
matching the system_ext compositor host. Verified with checkpolicy against
an isolated copy of the failed build's generated policy containing only
that attribute change: exit 0, both neverallow failures resolved. The
existing hal_vm_capabilities_default empty-permission warning remains.
No full image build or VM verification was run; retry the image build.

## Install Me background download diagnosis (2026-10-08)

Read-only checks on VM `127.0.0.1:5555` confirmed stale stub generation.
Discord's installed APK has `foregroundServiceType=1` without the framework
attribute resource ID; Android ignores it and InstallService crashes with
"foregroundServiceType 0x00000001 is not a subset ... 0x00000000". The
subsequent linuxd staging attempt fails DNS resolution.

The source generator already maps the attribute to
`android.R.attr.foregroundServiceType` (modified 21:54). Its host tests pass,
including aapt2 parsing of install dataSync and runtime specialUse types.
The VM bridge APK exactly matches the staged bridge APK built at 20:25,
before that fix. The generator is statically linked into MatonSystemBridge.
The coordinator needs to rebuild/deploy that bridge and regenerate/replace
the affected stub with the existing per-device signing key, then verify a
download while the store is foreground. A compositor-only push cannot fix
the installed stub manifest. No implementation or VM changes were made by
this diagnosis. Evidence: `out/pc-logs/install-background/`.

## Current handover — per-app stock Flatpak portal and Xwayland (2026-10-08)

Read [the runtime handover](docs/HANDOVER-2026-10-08-flatpak-java-portal.md),
especially its latest **Xwayland fixed** section. The user confirmed Brave
works with GPU acceleration in both Wayland and X11 modes. VM 5555 currently
has Brave running with `--ozone-platform=x11`.

The shared host compositor was removed. Its service coordinates verified
launches/DNS only; each stub owns its compositor, Xwayland and private bus.
linuxd's launch chain starts the stock Flatpak portal at that app's UID.
Java desktop portals remain for Android integration; UpdateMonitor will
use our own updater. Stock portal APK versus APEX packaging remains open.

Latest host build and Java portal tests passed. Missing Xwayland crypto
libraries are staged, the per-app socket handoff is automatic, and a different
app UID was denied access to Brave's mode-0600 X11 socket. VM changes use
temporary mounts. A durable image build, enforcing SELinux and lifecycle
validation remain outstanding.

Keep handover docs current as decisions, implementation, builds and VM
verification change, and before ending a session. Preserve the user's VM
on `127.0.0.1:5555` and the existing working tree.

## Software Center repair, 2026-10-02

Current handover: [Software Center and Flatpak repair](HANDOVER-2026-10-02.md).
r14 (20261002, `8d167425…`) is the current build and the one to test the
MC launcher on: the Xwayland baked-path fix (r13's Xwayland never worked
on device), wlroots logs in logcat (MatonWLR), bridge launch-failure
logging, plus all r13 content (xcb-closure APK fix, real Xwayland staged,
offer-both display selection, readable stub names `flatpak.<id>`).
App icons confirmed working by the user on r12. r11-r13 remain preserved.
No build is currently running. No release or push.

# Session handoff

2026-10-09 live app repairs (ADB 127.0.0.1:5555, runtime UID 10091):
The staged wrapper explicitly sets XDG config/cache paths and suppresses
blanket host filesystem exports; Kate launches with it. Installer mode now
sets both FLATPAK_SYSTEM_DIR and FLATPAK_USER_DIR to the verified operation
directory. Flatpak consults USER_DIR/extra-data even for system pulls; the old
shared directory is inaccessible to the installing stub UID. Pinball's signed
app-only staging download and offline deployment/extraction passed in a
disposable operation directory. The normal InstallActivity retry ran out of
space while staging the runtime (VM /data is 7.6 GB, about 1.5 GB free), so
full store completion remains unverified. The temporary test operation was
removed afterward. No installed apps or runtime deployments were removed.
The live wrapper is a temporary bind mount from
/data/local/tmp/maton-launch-fixed-wrapper-v2 in linuxd's mount namespace;
the same binary is staged in linux/flatpak/prebuilt/static/x86_64 for the
next image. The helper source is synced to MatonOS_apexs. NDK compilation
with warnings as errors and the host DNS monitor test passed.

Pending image source also waits for the stub runtime before opening the host
session and waits for both private bus sockets; it starts that runtime after
the permission callback as well. Stub version 13 reconciles installed desktop
names and preserves valid device-signed stub icons during regeneration.
Focused Java compilation and stubgen tests passed. These Java changes have
not been installed in the current VM. LibreOffice still exits before showing
a window; X11 did not fix it. Temporary diagnostic environment overrides were
removed. Steam testing is deferred to the next image at the user's request.

Live coordinator handoff: `out/pc-logs/agents/COORDINATOR-HANDOFF.md`
(agents, builds, current state, next steps). Read `CLAUDE.md` (rules, traps)
and `NOTES.md` (decisions, roadmap) first.

Paths: scripts derive the root; per-machine settings in matonos.local.env (git-ignored).
Build: `out/pc-logs/agents/coord-build.sh -K -M` (coordinator only).
Test: `tools/run-qemu-live.sh -g virgl -r 1920x1080` (adb 127.0.0.1:5555).
Repo: device/maton/pc_x86_64 is the git working copy of github.com/MatonOS-dev/MatonOS.
Zero AOSP patches (forks via local manifest + `forks/` patch series).
