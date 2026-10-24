# mountd shared integration record

The mountd device-side integration has been applied in this working tree:

* `device.mk` imports `mountd/mountd.mk`; `mountd/Android.bp` builds and
  installs the async-init `matonos-mountd` service.
* `mountd/BoardConfig.mk` adds root image mount points `/app` and `/usr`.
* The fixed `sepolicy/matonos` files declare mountd's distinct exec/domain and
  channel service labels, limited mount/proc/data access, and bridge Binder
  access. No policy files or area policy directory were added.
* `bundle/matonos-channel.xml`, `service_contexts`, and the bridge's known
  targets plus exact `mountd org.matonos.linux` caller row register the channel.

No additional shared changes are pending for the device tree. The generic
bridge currently authenticates the installed subsystem caller and target;
mountd independently verifies PID, UID, process name and PackageManager's
package/UID mapping. The subsystem still needs to send the documented
`mount_stub` request and use proxy fallback. The deployment source layout and
home mount target must be reconciled with the subsystem's final verified
extraction layout before enabling it in production.
