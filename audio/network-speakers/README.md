# Network speakers

The existing system bridge browses RAOP `_raop._tcp`, Snapcast
`_snapcast-jsonrpc._tcp` and `_snapcast-ctrl._tcp` with NsdManager. Persistent
service-info callbacks follow address/TXT updates and loss. Browsers and
failed watches retry on its worker thread. Duplicate RAOP names and Snapcast
numeric address/port pairs share one sink; loss of one duplicate does not
remove a surviving announcement. Network loss removes affected watches.

The bridge sends desired add/remove/reset commands over the existing
`IChannel/audio`. The HAL queues them, then forwards them through PipeWire
metadata on its private Unix connection. `NetworkSpeakerModules.c` runs
inside the existing PipeWire process and loads/unloads the stock RAOP sink
or the Snapcast integration. There is no additional daemon and no Avahi.
AOSP forbids IP sockets in audio HAL domains: moving the socket work into
the existing PipeWire process is necessary for stock policy/CTS compliance.

Module loads are asynchronous to Binder/startup and optional. Failures log
with receiver/module/error context and retry every three seconds. The bridge
replays every five seconds; both transport and daemon use 15-second monotonic
leases to clean up after a lost bridge/HAL. Restarted PipeWire metadata is
rebound and current desired state is replayed. Receiver failure never removes
the local sink or the existing `maton-fallback` object. Actual resulting
`Audio/Sink` nodes go through the HAL's existing registry/device mirroring.

RAOP TXT mapping, dependency symbols, licences and policy are recorded in
`PROVENANCE.md` and `../mediahal2.report.md`. RAOP supports resolved IPv4 and
scoped IPv6 (including discovery-interface suffix for link-local addresses).
Snapcast follows the pinned discover module's IPv4 selection. It creates a
48 kHz / S16LE / stereo stock protocol-simple sink and asks the server to add
its PCM TCP stream. **The server connects back to a local ephemeral listener**;
this is the stock protocol and requires LAN/firewall reachability in that
direction. Users must assign that stream to Snapcast clients on the server.
Roc is allowed (MPL-2.0) but optional and not included.

Run `check-native.sh AOSP NDK PW_SOURCE PW_BUILD PW_PREFIX OUTPUT` for NDK
object/link checks without Soong. The existing PipeWire build must provide
its RTP common archive and installed libpipewire. This passed locally,
including building pinned BoringSSL and linking all three network modules
with `--no-undefined`. The complete updated stack recipe, APEX/image, policy
compilation and real receiver playback remain unverified.

Coordinator acceptance on a fresh image:

1. Without a network/receivers, check `sys.boot_completed=1`, local playback,
   `maton-fallback` existence, and absence of boot waits.
2. Advertise RAOP and Snapcast on a reachable LAN. Inspect private PipeWire
   nodes and `dumpsys media.audio_policy`; select each mirrored device and
   verify audible PCM, volume and local fallback after removal.
3. Test duplicate IPv4/IPv6 advertisements, both Snapcast service types,
   TXT/address changes, Wi-Fi roam/loss and receiver restarts. After all
   advertisements disappear, confirm the remote sink disappears.
4. Restart PipeWire and the HAL with the coordinator's normal fresh-image
   procedure; confirm reconciliation, with no duplicate sinks. Check logcat
   and AVCs. Kill/restart the bridge and verify stale sinks expire.
5. Repeat on Ryzen/RX6600/Intel7265, Surface Pro 3/Marvell and HP ProDesk
   Haswell with the normal real network adapter. No VM USB/VFIO passthrough.

No receiver, VM, policy-runtime or audible playback test was run here.
