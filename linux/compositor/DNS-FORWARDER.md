# Per-stub DNS forwarder

Each generated app stub runs a native DNS forwarder under its own Android UID.
The loopback address is `127.(10 + ((uid >> 16) & 0x3f)).((uid >> 8) & 0xff).(uid & 0xff)`.
This maps into `127.10.0.0` through `127.73.255.255` and never overlaps
`127.0.0.0/16`. The mapping preserves UID bits 0–21; tests include distinct
multi-user UIDs. The stub always advertises `address:53`; there is no
high-port fallback.

## Privileged socket handoff

`StubActivity` sends its UID-derived address through the authenticated
`IEmbeddedHost.openSession` Binder call. The compositor verifies the signed
stub, asks `SystemBridgeService` to have linuxd create and bind UDP and TCP
sockets at that address on port 53, and returns the resulting
`ParcelFileDescriptor`s in the same Binder response. Both the bridge and
linuxd check the caller/UID and address formula before binding. linuxd has the
low-port capability as an init service; the bridge relays the descriptors and
retains no sockets. The stub native worker takes ownership of the received
descriptors and only receives, sends, accepts and serves on those sockets; it
never binds a port.

The authenticated `openSession` call carries the address to linuxd in the
existing `launchOwnedFlatpak` request. The system bridge passes it as linuxd's
`dnsServers` value, and linuxd's existing `MATON_FLATPAK_DNS` monitor writer
places it in the app's `/etc/resolv.conf`. The static-apex resolver writer is
unchanged. Because this resolver format has no port field, port 53 is required;
failure to bind either socket fails session setup.

## Query attribution and forwarding

For TCP and connected UDP, `NETLINK_SOCK_DIAG` checks the exact source and
destination tuple. For UDP `sendto()` sockets, the worker issues an IPv4 UDP
inet_diag DUMP filtered by the source port from `recvfrom()`. It accepts a
candidate only when its local address is the exact source address or wildcard
`0.0.0.0`. Connected entries must also match the destination tuple. If matching
`SO_REUSEPORT` candidates carry different UIDs, the packet is dropped. Missing,
malformed or incomplete diagnostics fail closed. The host C test feeds mocked
DUMP entries through the same selector used by the netlink parser.

The worker rate-limits queries with a token bucket (100 burst, 50 queries per
second), uses bounded DNS-over-TCP framing, and passes raw packets to
`android_res_nsend(NETWORK_UNSPECIFIED, ...)` followed by
`android_res_nresult()`. This retains Android's default-network, Private DNS,
VPN, per-UID resolver policy and accounting while returning the resolver's
answer bytes.

## SELinux and runtime requirements

linuxd's init service and SELinux domain grant low-port bind capability and
DNS-port UDP/TCP socket binding. The forwarder app domain must be able to use
`NETLINK_SOCK_DIAG` for inet_diag dumps and receive the passed sockets; validate
the precise AVCs on device before adding any app-domain policy. No policy was
added for the generated stub domains in this change. Binding, descriptor
transfer, sock_diag visibility and resolver behavior still need device
validation.

## Host unit test

```sh
cc -std=c11 -Wall -Wextra -Werror \
  -o /tmp/dns-forwarder-core-test \
  linux/compositor/native/tests/dns_forwarder_core_test.c \
  linux/compositor/native/dns_forwarder_core.c
/tmp/dns-forwarder-core-test
```

The test covers exact tuple UID checks, mocked UDP inet_diag DUMP attribution
(including wildcard/exact local binds and conflicting reuseport owners),
token-bucket behavior, TCP framing and UID-derived addresses for multiple
Android users.

## On-device validation plan

1. Start a generated stub and confirm the privileged bridge binds both UDP and
   TCP on its full-UID-derived address at port 53, then transfers both FDs.
2. Resolve A/AAAA records from an unconnected UDP `sendto()` socket and a
   connected socket. Confirm another UID receives no answer and differing
   `SO_REUSEPORT` owners are dropped.
3. Resolve an answer requiring TCP fallback; check the framing and answer bytes.
4. Test Private DNS, VPN, network switching and per-UID network rules against
   Android's resolver behavior.
5. Confirm `/etc/resolv.conf` always names the per-app address, and verify
   sockets close when the stub process exits and can be rebound on relaunch.
6. Inspect SELinux AVCs for linuxd low-port binding and stub sock_diag;
   exercise process death, activity recreation and multiple windows.
