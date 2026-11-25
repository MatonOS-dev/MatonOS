# Network speakers: unfinished

RAOP/AirPlay and Snapcast discovery are not enabled by this task. No network
sinks, discovery service, policy permissions or new dependency libraries
have been added. This is unfinished integration work, not a claim that an
AOSP patch is necessary. Existing local audio remains the active path.

The inspected PipeWire pin is
`8fa27cabdc6c0c1350c69c026af5850ef0af1e26` (1.6.9, MIT), already used by
`tools/build-pipewire.sh`. Relevant primary sources:

- [Module build rules](https://github.com/PipeWire/pipewire/blob/8fa27cabdc6c0c1350c69c026af5850ef0af1e26/src/modules/meson.build):
  `raop-discover` and `snapcast-discover` require `avahi-client`;
  `raop-sink` requires OpenSSL. Enabling only `-Davahi=enabled` will fail
  without the client dependency; setting module config does not build it.
- [RAOP discovery](https://github.com/PipeWire/pipewire/blob/8fa27cabdc6c0c1350c69c026af5850ef0af1e26/src/modules/module-raop-discover.c)
  and [Snapcast discovery](https://github.com/PipeWire/pipewire/blob/8fa27cabdc6c0c1350c69c026af5850ef0af1e26/src/modules/module-snapcast-discover.c)
  use Avahi client browser/resolver callbacks. Snapcast discovers server
  control/JSON-RPC services and creates a stream on the server; the server
  must route that stream to its clients. It is not discovery of individual
  speaker hardware.
- `packages/modules/Connectivity/service-t/src/com/android/server/NsdService.java`
  in the current AOSP checkout uses the Java MdnsDiscoveryManager/socket
  client; `mMDnsManager` is null on V and later. A running Android NSD service
  does not implement Avahi's D-Bus client API. Merely linking a client or
  allowing multicast sockets cannot connect the stock modules to it.

Smallest direction with no additional long-running daemon: reuse
`NsdManager` from an existing trusted Android service and forward verified
add/remove/resolve events to multimedia glue which creates/destroys stock
PipeWire sink modules. That bridge/glue has not been written. It needs
network-change handling, IPv6 scope IDs, TXT-to-module-property mapping,
restart reconciliation, sink removal, and authorization on its control
channel. Alternatively, fork the discovery modules to embed Avahi core
instead of its D-Bus client. Stock modules without a fork need an Avahi
service and D-Bus bus; neither is currently shipped in the media APEX.
Those are alternatives for completing the integration, not installed
backends. No new dependency pin or unreviewed Avahi build is introduced.

ROC is deliberately excluded by the task's permissive-licence condition.
[Roc Toolkit's licence](https://github.com/roc-streaming/roc-toolkit/blob/master/LICENSE)
is MPL-2.0 (file-level copyleft), not a permissive licence.
[Upstream's dependency review](https://roc-streaming.org/toolkit/docs/about_project/licensing.html)
also identifies additional optional codec licence obligations. This is a
licence-condition exclusion, not an allegation that Roc uses GPLv3.
`-Droc=disabled` remains appropriate.

No SELinux rules were added, since there is no new network client to grant.
A completed implementation needs only its own domain's UDP/TCP socket
creation/use, outbound sink connection, and discovery transport rights.
With NsdManager the Android NSD service owns multicast discovery; do not add
mDNS socket rights to PipeWire for that architecture. Audit actual AVCs and
source operations before writing the exact rules. Linux app domains must
retain no access to the system PipeWire socket.

Boot safety today: no startup behaviour changed. Future module loads must
be optional (`nofail`), with connection/discovery failure logged and
asynchronous; retain the local/fallback sink when networks vanish. Do not
wait for mDNS, an interface, receiver, or APEX content in init. Do not make
WirePlumber startup depend on a speaker.

Coordinator acceptance after the missing implementation exists:

1. Fresh boot without network/receivers: `getprop sys.boot_completed` is 1,
   local playback works, and the fallback sink exists.
2. With a reachable RAOP receiver and Snapcast server on the test LAN,
   inspect `/apex/com.matonos.hardware.media/bin/pw-cli ls Node` as the audio
   user in the private PipeWire runtime. Confirm actual `Audio/Sink` nodes.
3. Inspect `dumpsys media.audio_policy`, select each mirrored device using
   the current settings app, play PCM and verify audible output at the
   receiver. Validate volume, disconnect and sink removal while playing.
4. Repeat after Wi-Fi roam, interface loss, IPv4/IPv6 changes, receiver
   restart, PipeWire restart and duplicate advertisements. Local audio must
   remain usable throughout. Inspect logcat and SELinux AVCs.

No receiver, network discovery, build or playback test was run here.
