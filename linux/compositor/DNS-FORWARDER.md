# Per stub DNS forwarder

`StubActivity` loads `libmaton_dns_forwarder.so` in the generated app package
and holds a process-local reference while its activities are alive. The native
worker binds both UDP and TCP on `127.10.(uid >> 8 & 255).(uid & 255)`. It first
tries port 53, then port 1053 if either socket cannot bind port 53. The port is
reported as `address:port` by `DnsForwarder.endpoint()`.

The endpoint crosses the authenticated `IEmbeddedHost.openSession` call and is
carried by the compositor's `launchOwnedFlatpak` call. `SystemBridgeService`
uses that endpoint as the linuxd `dnsServers` value only when it is a valid
`127.10.x.y:53` endpoint. linuxd's existing environment handoff
(`MATON_FLATPAK_DNS`) and monitor-file writer then generate the app's
`/etc/resolv.conf` with the loopback address. The static-apex resolver writer
is not changed here.

## Current constraints

An ordinary Android app UID cannot normally bind a port below 1024. The 1053
fallback is therefore expected unless the platform explicitly grants low-port
binding. `resolv.conf` names server addresses but cannot encode a port, and the
current writer accepts IP addresses only. In fallback mode linuxd retains its
existing DNS server list, so the per-app forwarder is not used. Options to
close this gap are: arrange a narrowly scoped platform grant for port 53,
provide a privileged port-53 relay, or change the Linux resolver stack to
support a per-server port. The last option requires a resolver/writer change
outside this patch.

The design's `android_res_nquery()` wording does not match the NDK API:
`android_res_nquery()` builds a query from a domain name and RR type. This
implementation uses `android_res_nsend(NETWORK_UNSPECIFIED, raw_query, ...)`
followed by `android_res_nresult()` to retain raw query support and Android's
resolver network, Private DNS, VPN, UID policy and accounting.

`NETLINK_SOCK_DIAG` returns a socket's UID for a connected address/port tuple.
The listener compares the exact source and destination tuple and drops when
there is no exact match or the UID differs. An unconnected UDP `sendto()`
socket has no destination in its diagnostic tuple and consequently fails
closed; whether app DNS clients use connected UDP sockets must be checked on
device. A platform alternative is needed if they use unconnected sockets.
This app-domain access also needs an SELinux allow for creating and sending
`NETLINK_SOCK_DIAG` diagnostic requests. Socket bind/connect permissions for
loopback UDP/TCP must be allowed by the generated stub's app domain. Do not
grant broader network diagnostic or low-port access without validating the
specific AVCs and platform mechanism.

The requested address formula uses only the low 16 UID bits, so Android
multi-user UID ranges can collide. Device validation should confirm the
supported user model or replace the mapping before multi-user use.

## Host unit test

Build and run the pure C tests without Android headers:

```sh
cc -std=c11 -Wall -Wextra -Werror \
  -o /tmp/dns-forwarder-core-test \
  linux/compositor/native/tests/dns_forwarder_core_test.c \
  linux/compositor/native/dns_forwarder_core.c
/tmp/dns-forwarder-core-test
```

## On-device instrumentation plan

1. Install a generated Flatpak stub and start it. Confirm the listener address
   uses the stub UID and that UDP and TCP bind on the same chosen port.
2. From the Linux app, resolve an A/AAAA name and a response larger than the
   UDP limit. Confirm regular replies are byte-identical to `android_res_nresult`
   output and truncated responses retry over TCP.
3. Send a query from a different Android UID to the listener and confirm no
   response. Repeat with a same-UID app socket, an unconnected UDP socket and
   connected UDP/TCP sockets; inspect `sock_diag` results and AVCs.
4. Test active Private DNS, a VPN, network switching and per-UID network rules;
   compare results and resolver traffic accounting with the Android resolver.
5. Exercise token exhaustion/refill, malformed TCP lengths, resolver timeout,
   activity recreation, multiple windows, process death and relaunch.
6. Verify `MATON_FLATPAK_DNS`, generated `/etc/resolv.conf`, and port selection.
   Specifically verify whether the platform allows port 53; if it selects
   1053, confirm the documented resolver limitation is visible and understood.

The host unit test covers UID-check logic with a mock diagnostic lookup,
token-bucket behavior, TCP framing and address formatting. The Android socket,
SELinux and resolver behaviors above remain device validation items.
