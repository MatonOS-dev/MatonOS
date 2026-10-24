# mountd spike

This directory contains the design outcome of the 2026-10-24 mountd spike.
No service, init rule, policy or build integration is active. Read
[DESIGN.md](DESIGN.md) for the proposed daemon and the measured limits.

## Validation completed

* On enforcing v46, Tux Racer ran under UID 10123. Its loader mount namespace
  was distinct from the root shell's namespace. Root `nsenter` could bind
  mount an app tree in that namespace; mounts disappeared with the temporary
  test setup.
* The test VM root has no `/app` or `/usr` directories and is read-only. A
  chroot-backed test root supplied equivalent mountpoints for syscall
  measurements.
* Native and EROFS open/stat benchmarks ran against the extracted Tux Racer
  payload. EROFS read-only loop mounting worked in the VM.
* A host-only `--test-direct-prefix` switch in the compat proxy skipped VFS
  translation for a real bind mount. In the same rootless user+mount namespace,
  this improved open by 17% and stat by 6% versus translated proxy paths. It
  did not bypass user-dispatch syscall handling, which remained the dominant
  cost (about 32× native).
* A cold stub process displayed the Tux Racer player-selection menu 10.42 s
  after `am start` in one run. This is a baseline only; no mount-enabled menu
  comparison was possible.

## Follow-up tests on a fresh image

After root mountpoints and a proxy syscall fast path are implemented:

1. Boot a fresh image with SELinux enforcing and set
   `persist.vendor.maton.sleep_idle_s=0` in the VM.
2. Install the same Tux Racer stub and launch it five times in each mode:
   translated VFS, real `/app`+`/usr` mounts with direct syscalls, and mounts
   with the proxy fallback forced. Time from activity start to the visible
   player-selection menu; preserve cache conditions between modes.
3. Check `/proc/<stub-pid>/mountinfo` from root. Confirm only `/app`, `/usr`
   and the expected home mount are added in the stub's mount namespace.
4. Stop and relaunch the stub. Confirm stale namespace requests fail cleanly,
   fresh requests mount the current deployment, and no leftover loop device
   or mount remains.
5. Launch a nested Flatpak bwrap command and `flatpak-spawn --host`; confirm
   their out-of-view paths continue through the compatibility proxy.

Real hardware follow-up: repeat cold launch and file workload on the Ryzen
5800X/RX 6600 build PC, Surface Pro 3 and HP ProDesk 600 G1. Check that each
stub's mount namespace remains private and that daemon or EROFS failure
falls back without delaying app startup.
