# Handover — 2026-11-11 night

Read this first. Earlier: HANDOVER-2026-11-08-night.md.

## State
- Disk VM up on adb `localhost:5556` with virtio-sound (`MATON_QEMU_SOUND` now defaults to virtio). The image is userdebug, so `adb root` works.
- No builds or codex agents are running. The subsystem tree is clean, and all work is pushed (subsystem `main` + `dbus-bridge`, device tree).
- Megabonk runs and the user is playing it. Its audio is user-verified clean.

## Done this session
- **Steam updater black bars (user-verified):**
  - Stubs have a 96dp minimum window size.
  - Outdated app stubs are regenerated after a subsystem update. A refresh job runs on MY_PACKAGE_REPLACED, covering `app/` refs only (725a158).
  - Launch bounds include the measured desktop caption height (48 px on the VM, stored in prefs "window"/"caption") (0893916).
- **Megabonk audio:**
  - `pipewire_android.c`: the prime is now AAudio burst + max quantum and the ring is the next power of two ≥ 2× the prime. The old fixed 2048-frame prime was shorter than the 4098-frame burst (50ee3e2).
  - `run-qemu-live.sh`: the pa backend now uses a 48 kHz host stream with 40 ms buffers (1a5c311).
  - The remaining clicks were QEMU's Intel HDA emulation. virtio-sound is now the default (3cd4fcd), and the user confirmed the clicking is gone.
  - Lesson: capture the bridge output before blaming guest code. The capture was clean and AudioFlinger reported 0 underruns.

## Dead ends (audio)
These were tried and reverted:
- DLL rate control
- AAudio-driven PipeWire (driver mode; caused slow-motion audio)
- a larger pulse tlength
- the full-capacity AAudio buffer

## Open backlog
1. **Steam toast placement:** needs a screenshot of a toast; none has been seen since.
2. **Tray items:** the Steam tray icon shows in the Launcher3 taskbar pill. Menus and other apps still need checking.
3. **Geometry Dash and Factorio bring-up.**
4. **Real-PC audio:** this session fixed only the VM path. The HDA + HAL path on real hardware is untested.

## Commands
- Build: the full `./build.sh` env line is in the 11-08 handover / memory, logging to `out/pc-logs/main-out.log`. "Repository corruption encountered" in that log is expected.
- Install: `adb -s localhost:5556 install -r ~/matonos/build/main-out/MatonLinuxSubsystem.apk`
- Megabonk: `adb -s localhost:5556 shell am start -a android.intent.action.VIEW -d steam://rungameid/3405340 -p flatpak.app.com_valvesoftware_Steam.x86_64.stable`
- VM restart: run `out/pc-logs/agents/vm.sh kill 5556`, then the persistent launch line (MATON_QEMU_PERSISTENT=1, qemu-venus, `-g native -m 12288 -a 5556`).
