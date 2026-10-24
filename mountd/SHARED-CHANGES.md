# mountd shared changes requested

No shared device-tree file was edited by this spike.

Before integrating the daemon, the coordinator needs to make these shared
changes in one batched build update:

1. Add fixed root-image directories `/app` and `/usr` using the existing
   device root-files mechanism; v46 has neither and `/` is read-only.
2. Import `mountd/mountd.mk` from the device make graph and register the
   native binary plus init rc in the ODM bundle build.
3. Add `matonos-mountd` rules to the existing fixed daemon-zone SELinux files
   in `sepolicy/matonos/` (do not add an area policy directory): proc/pidfd
   identity inspection, `setns`, loop device setup, EROFS mounts and bind
   mounts only.
4. Register the stable channel instance
   `vendor.matonos.channel.IChannel/mountd` and allow the system bridge to
   forward an authenticated stub identity (package, UID, signer, PID and
   process start identity). Do not expose an app-facing Binder service.
5. Confirm the system bridge's existing caller checks are sufficient for the
   mount request. If they do not forward verified PID/package identity,
   request a narrowly scoped bridge method; do not trust values supplied by
   the stub itself.

Reason: mountd needs privileges that Linux stub apps must not receive. The
namespace mount operation belongs in a privileged daemon, and the image must
provide stable target directories before the daemon can attach verified
deployment sources.
