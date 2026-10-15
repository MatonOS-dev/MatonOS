# Current app ownership

The earlier privileged launcher prototype is retired. The stub owns its own
compositor, private bus, DNS forwarder and Linux process tree, inheriting its
stock app domain/filter. PackageManager owns its private HOME and cache.
linuxd controls only the shared system-wide code installations and transient
signed-download operations. See [STORAGE-LAYOUT.md](STORAGE-LAYOUT.md) and
[v3 launch](../../linux/subsystem/V3-LAUNCH.md) for current details and tests.
