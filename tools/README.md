# Live ISO status

Bootable ISO output is dropped under the v1.2 zero-AOSP-patches rule.

The ISO9660 design was tested far enough to verify UEFI optical boot and to
attach the ISO's `super.img` as `/dev/loop0` in the live pre-init shim. Android
first-stage init then remounts `/dev` as a new tmpfs, which removes the shim's
`/dev/block/by-name/loop0` link. First-stage mount resolves the super device
through `/dev/block/by-name/<super_partition_name>` and fails before Android
boots. The loop block device also lacks the platform device metadata used by
the stock device handler to recreate a by-name alias.

There is no supported configuration in our files to create that alias after
the `/dev` remount. Completing this requires an AOSP first-stage init change,
such as supporting an explicit super-device path or creating the by-name link
for loop devices after `/dev` is mounted. That change is not included because
the current user rule prohibits AOSP patches.

The existing GPT live disk remains available through `tools/make-live.sh`.

## SELinux boot profiles

Live and installed boot entries default to `androidboot.selinux=enforcing`.
`MatonOS Live (debug)` adds verbose kernel logging and the serial root console;
**debug does not imply permissive SELinux**.

For development only, `MATON_SELINUX_PERMISSIVE=1 tools/make-live.sh ...`
makes both live menu entries permissive and labels both
`(DEVELOPMENT: SELinux permissive)`. The installed A/B UKIs on that media,
the Settings installer entries, and the installer test plan always enforce.
Selecting the live debug entry never makes the installed system permissive.

The legacy `make-payload.sh` also accepts `MATON_SELINUX_PERMISSIVE=1` to
produce an explicitly permissive development payload. Installing it with
`tools/installer.sh` requires that option again in the installer's environment
(e.g. preserve it explicitly when invoking through sudo). Otherwise the
installer rejects permissive normal **or debug** options before disk selection
or writes. A custom `-c` cannot disable SELinux without the development option;
`make-live.sh -c` cannot select SELinux at all. Raw kernel disabling options,
LSM replacement, duplicate SELinux modes, and `--` are rejected.

Release packaging must use `-R` on either packager or `MATON_RELEASE=1`
(inherited through `build.sh` and `quick-image.sh`). Both packagers refuse
`MATON_SELINUX_PERMISSIVE=1` before signing, staging or writing an image.
The existing `MATON_REQUIRE_2023_SHIM=1` release profile and a `user`
`TARGET_BUILD_VARIANT` also refuse it. `-S` selects Secure Boot and is not a
release marker. There was no separate release packaging entry point in this
repository; `-R` defines that contract. Enforcing by default does not establish
that the current policy can boot: see [the r24 test plan](../sepolicy/r24-enforcing-test-plan.md).

Run the host-only profile regression tests with
`python3 tools/tests/test_selinux_boot.py`. They create no images and use no VM.
