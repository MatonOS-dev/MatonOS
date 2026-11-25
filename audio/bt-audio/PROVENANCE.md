# Software Bluetooth audio provider provenance

Forked from `hardware/interfaces/bluetooth/audio/aidl/default` in AOSP
`hardware/interfaces`, commit `0162af698935100a590b7359581ac8b1b80693e5`.
The copied source paths were clean in that checkout when copied. Original
Apache-2.0 copyright and licence headers are retained. No new external
library dependency is introduced: this uses the existing Apache-2.0 AOSP
Binder/FMQ/audio-session libraries already linked by the media HAL.

Copied files: `BluetoothAudioProvider.{h,cpp}`,
`BluetoothAudioProviderFactory.{h,cpp}`, `A2dpSoftwareAudioProvider.{h,cpp}`,
`LeAudioSoftwareAudioProvider.{h,cpp}`, `HearingAidAudioProvider.{h,cpp}`,
`HfpSoftwareAudioProvider.{h,cpp}`, and `service.cpp`.

Changes from upstream:

- The factory creates software providers only, including LE unicast input
  and output, broadcast input and output, A2DP input and output, hearing aid
  output, and HFP input and output. Unknown and hardware-offload sessions
  return no capabilities and cannot be opened. `getProviderInfo` does not
  advertise hardware codecs. Result objects are cleared before reuse.
- Removed offload-provider includes, codec-factory member and unused
  codec-factory include in the base provider. The existing shared session
  utility still contains its upstream codec helpers; they are not offered
  as hardware sessions by this provider.
- Added missing `override` declarations, invalid-FMQ error logs and an error
  log for factory registration failure. Death-link failures now log status
  and fail the session. Unlink uses the exact cookie supplied to link (the
  upstream code incorrectly supplied `this`) and logs failure. NDK's
  `onUnlinked` callback retains sole responsibility for freeing the cookie.
- Built as `libmatonos_bluetooth_audio_software`, whole-linked into the HAL.
  AOSP `ModuleBluetooth` still calls the same C factory entrypoint; no AOSP
  source changes are needed. `--as-needed` drops the unused shared stock
  provider, including the dependency inherited from the stock core archive.
  The APEX builder rejects a HAL that still imports that provider and removes
  any copy inherited from the bootstrap effects payload.
- The Bluetooth audio v6 VINTF entry is in the media APEX's existing manifest,
  rather than a second bootstrap vendor fragment.

The Binder/FMQ implementation requires C++ and is retained in upstream's
language. No new native daemon, private API, Linux-app access, driver or
kernel option is added. The default module and its PipeWire connection are
unchanged.
