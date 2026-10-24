# mountd shared integration record

The mountd device-side integration has been applied in this working tree:

* `device.mk` imports `mountd/mountd.mk`; `mountd/Android.bp` builds and
  installs the async-init `matonos-mountd` service.
* `mountd/BoardConfig.mk` adds root image mount points `/app` and `/usr`.
* The fixed `sepolicy/matonos` files declare mountd's distinct exec/domain and
  channel service labels, limited mount/proc/data access, dedicated labels for
  MatonOS deployment/home trees, and bridge Binder access. No policy files or
  area policy directory were added.
* `bundle/matonos-channel.xml`, `service_contexts`, and the bridge's known
  targets plus exact `mountd org.matonos.linux` caller row register the channel.
  For `mount_stub`, the bridge now verifies the stub package/UID/signing
  certificate and that the PID is a live matching process, then adds a fresh
  request nonce before forwarding to mountd.

No additional shared device-tree changes are pending. The subsystem still
needs to send the documented four-field bridge request and use proxy fallback;
the bridge expands it to the six-field daemon wire format in `README.md`.
Deployment metadata must supply the verified deployment ID, and its extracted
directory/home ownership and final mount target must match the documented
layout before enabling this fast path in production.
