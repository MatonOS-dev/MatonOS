# Android-facing audio ports for the v2 roles

The HAL presents a stable set of semantic device ports. PipeWire owns the
physical-node choice and can replace or hotplug nodes without changing Android
port identity. The settings page assigns one preferred PipeWire node and an
ordered fallback list to each role through `IChannel/audio`; it never stores
ALSA card numbers or transient PipeWire node IDs as the role itself.

| HAL port / AIDL device type | v2 role | Route ownership |
|---|---|---|
| `Speaker` / `OUT_SPEAKER` | Speaker | PipeWire default sink; built-in analog first, then configured fallback |
| `Headphones` / `OUT_HEADPHONE` | Headphones | PipeWire sink selected by the user or jack policy |
| `Headset` / `OUT_HEADSET` | Headset output | Same physical headset choice as the paired headset microphone |
| `Headset Mic` / `IN_HEADSET` | Headset microphone | Paired input node; falls back to built-in microphone |
| `Built-In Mic` / `IN_MICROPHONE` | Microphone | Default capture source; user override/fallback order |
| `Display Audio` / `OUT_DEVICE` with connection `hdmi` | HDMI / DisplayPort | PipeWire HDMI/DP sink, selected by display association or user |
| `USB Output` / `OUT_DEVICE` with connection `usb` | USB output | USB node; hotplug changes connected state/routing, not role identity |
| `USB Input` / `IN_DEVICE` with connection `usb` | USB microphone | USB source; hotplug changes connected state/routing, not role identity |

The initial implementation can use the stock primary module's speaker and
microphone ports only. The expanded stable port set and dynamic connection
state are a v2 HAL/policy milestone: HDMI and USB should be exposed as
connected external devices using AIDL `connectExternalDevice` with stable
role-facing identity, while PipeWire resolves the selected physical node.
Keep policy port IDs and mix-port routes aligned with the HAL configuration.
If stable device addresses are required by AudioPolicy, use a role URI such as
`matonos:audio:hdmi` or `matonos:audio:usb-out`, never a kernel card index.

Bluetooth remains the platform Bluetooth audio path. Android's existing
`IModule/bluetooth` handles A2DP, hearing aid, and LE audio endpoints; those
are not duplicated as PipeWire roles. Future call devices (earpiece, telephony
RX/TX, SCO) stay out of the generic-PC role set unless a real telephony use
case is added.

Suggested channel contract for the settings agent:

- `get_state`: available roles, active logical endpoint, connected state,
  selected node display name, and whether fallback is active.
- `set_role_route`: role identifier plus preferred node selector and ordered
  fallback selectors; validate selectors against currently discovered nodes.
- `set_volume_boost`: boolean advanced setting, off by default. When enabled,
  map the top of Android's media-volume range into PipeWire gain up to 1.5x and
  apply a soft limiter. Report enabled state and limiter activity in `get_state`.
- `subscribe(audio_devices)`: emit availability, active-route, and fallback
  changes. Persist selectors, not ephemeral node IDs; resolve them by stable
  PipeWire properties (bus path, device serial when present, profile, and
  port name), then fall back to node class/name.

This is a design only. It must not change the current vendor policy XML until
HAL port configuration, service handoff, and fresh-image verification land.
