# In-process D-Bus portals

Each v3 stub runs its private JNI session bus (`linux/dbus-broker`) and
JavaPortal in its own process and UID. The retained broker routes bus messages
and forwards reserved Desktop portal requests through MBP1 to JavaPortal.
It accepts only same-UID peers. There is no privileged session supervisor,
monitor path or standalone broker service.

The pure-Java codec consists of DBusSignature, DBusReader, DBusWriter and
PortalWire. PortalChannel, PeerConnection and LocalSocketChannel provide the
connection; PortalBackend owns portal state and authorization. LocalSocket
carries SCM_RIGHTS for granted descriptors. Android Context/platform work
stays in JavaPortal at the stub's identity. No dbus-java or SLF4J dependency
or alternate portal implementation is shipped.

The bus and portal sockets are private under `<stub dataDir>/runtime` (0700).
Closing the stub process closes the runtime; linuxd manages installations only.
See [runtime](PER-APP-RUNTIME.md) and
[launch](../linux/subsystem/V3-LAUNCH.md).

Host verification: `linux/subsystem/host/test-portals.sh OUTPUT_DIRECTORY`
checks codec validation, peer connections and portal policy. The retained
broker Makefile's `check-inhibit`, `check-portal-wire` and
`check-policy-wildcard` targets exercise C/Java forwarding, descriptor passing,
and bus-name policy. Fresh-image checks must cover granted/denied portal
requests, repeated open/close, descriptor lifetime and cross-UID socket denial
under enforcing SELinux. Host tests do not prove those device properties.
