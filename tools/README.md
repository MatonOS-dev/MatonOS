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
