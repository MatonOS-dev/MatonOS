# Handover 2026-10-21 night

## State
- Linux subsystem work: ~/matonos/repos/lsa-gpuint, branch egl-window (NOT pushed). Commits today, oldest first:
  b2019cf GL.maton any runtime branch · 8c77008 open_tree ENOSYS · e25630b new mount API ENOSYS ·
  4a6fcd8 stubs load subsystem from own dex (no uses-library) · bcc9d91 + c38a9da pack backing dirs ·
  f2c59df XDG_RUNTIME_DIR fd survives exec (Xwayland keymap) · 61a45e2 EGL desktop passthrough ·
  7625249 v3-command absolute path · e553155 GPU bridge bootstrap v2 · 9bb0de2 native syscall IDs ·
  2c4516d musl bwrap · fbf0ebb static musl flatpak/app-flatpak/plan helper + simple native rule ·
  2549826 keep resident Android graphics fds across exec (ES works) · 680d7b2 GLX vendor · c93b694 GLX make-current fix.
- Device tree main: df12467 (Mesa fork in build-mesa.sh + FORKS.md), on top of 764c3f4. NOT pushed.
- Mesa fork: https://github.com/MatonOS-dev/mesa branch matonos/v26.10 (26.2.3 + EGL desktop GL on Android, 58a186c),
  checkout ~/matonos/repos/mesa; prebuilt/mesa staged from it. Disk VM runs it via adb-remounted /vendor libEGL_mesa.so.
- Disk VM running headless on adb 127.0.0.1:5556 (adb root, remounted, permissive debug kernel).

## Verified today (disk VM, Tux Racer stub, Platform 25.08)
- ES 3.2 red pixel; desktop GL 4.6 compat red pixel; Vulkan (10-19). Memory: gl-passthrough-milestone-2026-10-21.
- Flatpak installs with the static musl build (Tux Racer deps reinstalled).
- Stub host updates work without reboot (stub dex loader).

## Stopped for the night (nothing running; crons cancelled)
- glx-shim (sol) finished: 2b37b56 (resident pthread sigaltstack before adoption -> Tux Racer no longer SIGSYS) and
  40fa514 (GLX swap readback via Maton thunks). Report: out/pc-logs/agents/glx-shim.final.md.
- Presentation still broken, and NOT only GLX: a plain Xlib XFillRectangle readback is wrong and even a launcher
  screenshot after HOME is black/corrupt (blue/white stripes). Suspect the VM display/screencap path or the compositor's
  X11/Android surface path before the shim.

## Next
1. Find why plain screenshots/Xlib readback are corrupt on the disk VM (screencap of the Android launcher too), then
   GLX visible triangle + Tux Racer screenshot.
2. Install-plan bug: already-installed extension parent missing (GNOME Calculator Locale; flatpak-musl.final.md).
3. Stub dependency repair did not restore uninstalled runtime stubs.
4. v44 image (Mesa fork, xperm rule from 764c3f4) -> regenerate disk VM -> enforcing boot -> push main + egl-window.
   Stop the VM before the image build.
5. Clear ~/.gradle once no Gradle daemon is running (16 GB); /home is 62% (171 GB free).

## Rules learned today (all in memory)
- No bionic on the Linux side of the proxy; musl for our tools. Test briefs: VM agents only remove what they added.
- /mnt/data2 is slow: cold data only. Mesa = MatonOS-dev/mesa fork; check FORKS.md before patching third-party code.
- No framework patch for stale host paths; subsystem updates may relaunch stubs.
