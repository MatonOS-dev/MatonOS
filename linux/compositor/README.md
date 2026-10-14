# Per-app compositor host

Each signed stub runs PerAppRuntime, the wlroots native core, Xwayland,
SessionBus, JavaPortal, SessionAudio and DNS forwarding in its own process and
UID. The host APK supplies the shared library and native assets; it does not
coordinate another app's Linux launch. V3Launcher runs the compat-proxy loader
from the host nativeLibraryDir. See [v3 launch](V3-LAUNCH.md),
[stub format](STUBS.md) and [DNS](DNS-FORWARDER.md).

StubService creates `<dataDir>/runtime` itself, canonicalizes the path and
sets mode 0700. Wayland, bus, portal-backend, pulse and X11 sockets stay below
that PM-owned app data directory. No privileged runtime preparation, shared
socket namespace, registration supervisor or shared compositor remains.

Native sources and pinned dependencies are built outside Soong with
build-compositor.sh/build-xwayland.sh, using scratch/output under out/.
The in-process JNI broker remains because SessionBus loads it; its standalone
entry, privileged session control and unused SysV supervisor are deleted.
Closing the final explicit app window ends the stub runtime; minimizing or
recreating an activity preserves it. Relaunch apps after host updates during
pre-alpha. MatonOS is based on AOSP.

Local javac, native broker and compat-proxy results are recorded in
out/pc-logs/agents/v2-retire-result.md. Fresh-image enforcing validation remains:
verify Wayland and X11 rendering, portals, audio, DNS, close/reopen and updates,
and denial of another UID opening the app's sockets. Repeat on the supported
PC test machines. SysV emulation and broad shared-folder access remain gaps.

### Keys without scan codes (round 16)

WindowActivity and StubActivity forward `KeyEvent.getScanCode()` unchanged
through JNI. Positive scan codes are raw Linux evdev codes and retain their
existing path. wlroots adds the XKB offset internally. When no scan code is
available, `native/android_keymap.c` translates the framework keycode using
an inverse of AOSP `frameworks/base/data/keyboards/Generic.kl`. The first
numeric mapping in file order wins; duplicate choices are recorded in the
generated file. HID usage mappings are excluded because they are not evdev
codes. Unmapped events are dropped before focus or keyboard state changes,
with one debug log per process; no numeric keycode offset fallback remains.

Regenerate from the local AOSP checkout, then review the diff:

```sh
python3 linux/compositor/native/tools/generate_android_keymap.py /mnt/data/aosp > linux/compositor/native/android_keymap.c
cc -std=c11 -Wall -Wextra -Werror linux/compositor/native/tests/android_keymap_test.c linux/compositor/native/android_keymap.c -o /tmp/android-keymap-test
/tmp/android-keymap-test
```

Validation: standalone host test passed, all 240 mappings were checked against
Generic.kl, and regeneration was byte-identical. `build-compositor.sh` passed
(dependencies), then the native CMake target compiled and linked with NDK r30
in a separate out/ directory. No APK/image build or device test was performed.

After the coordinator builds a fresh image, on each supported PC launch a
Linux text editor and verify physical keyboard letters, modifiers and arrows.
Send scan-less DPAD_DOWN/UP/LEFT/RIGHT, ENTER and DEL events with
`input keyevent`; verify cursor movement, newline and backspace. Check an
unmapped keycode produces no key in the app, and IME-delivered KeyEvents use
the same mapping. IME text commits that do not produce KeyEvents are outside
this change. No shared-file or kernel changes are required.
