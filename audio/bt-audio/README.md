# Bluetooth audio in the media APEX

Status: all provider sources compile with NDK r30/API 35 and the checkout's
Bluetooth audio v6 generated headers. The factory test also compiles. Server
Soong linking, test execution, APEX packaging, boot and radio playback are
unverified. See `../mediahal1.report.md` for coordinator commands and scope
that remains unfinished.

The software factory is whole-linked into `matonos-audio-hal`. Existing AOSP
`ModuleBluetooth` registers it before registering `IModule/bluetooth`.
It shares the stock in-process BluetoothAudioSession utility with that
module, which is important: moving just the factory to another process
would split the session state used for PCM transport. AudioFlinger uses
the existing Bluetooth policy routes. PipeWire handles the primary PC
endpoints; Bluetooth PCM uses AOSP's FMQ path to the Bluetooth stack, whose
software codecs and HCI transport send it to the controller.

The factory covers nine software sessions: A2DP encode/decode, LE unicast
encode/decode, LE broadcast encode/decode, hearing aid encode and HFP
encode/decode. These paths are established by source inspection and object
compilation only. No successful Bluetooth session or audible playback is
claimed. There are no hardware-offload providers in the fork.

Boot safety: factory construction performs no controller discovery, socket
connect, hardware wait, or codec session startup. The stock module uses
`AServiceManager_checkService`, not a service wait. FMQs are allocated on
`openProvider`/`startSession`, after clients request them. Bluetooth has no
attached/default policy device and creates no route until an endpoint
connects; the primary fallback stays routable. No `critical` service, init
wait, or new startup dependency is introduced. This removes the historical
missing-provider condition, but a fresh no-controller boot is still required
to verify the complete image.

`audio.mk` enables every profile in `Config.LE_AUDIO_UNICAST_PROFILES`,
broadcast source/assistant and HAP, and explicitly declares offload
unsupported. No overlay is needed: in this checkout the profile `isEnabled`
methods consume BluetoothProperties. Controller capability checks remain
stock. In particular, `AdapterService.updateLeAudioProfileServiceState`
disables unicast profiles without CIS-central support. A Bluetooth-off or
radio-less system can return ERROR_BLUETOOTH_NOT_ENABLED; an older controller
can report FEATURE_NOT_SUPPORTED. Software LC3 does not add ISO to those
controllers. The feature acceptance requires a controller advertising the
corresponding CIS/BIS features, plus a paired compatible endpoint for actual
playback. Do not spoof those capabilities.

No SELinux rules were added. `IBluetoothAudioProviderFactory/default` is
already `hal_audio_service`; `hal_audio_default` is its server and the
Bluetooth domain is already a `hal_audio` client. Existing Binder/FMQ rights
apply. Linux app domains gain no access to PipeWire.

Run the source-only compile check (no Soong invocation):

```sh
python3 audio/bt-audio/tests/check-sources.py --aosp /path/to/aosp --ndk /path/to/android-ndk-r30
```

The check needs previously generated platform headers in the supplied out
tree. It compiles seven objects and `FactoryTest.cpp` with `-Wall -Werror`,
removing its temporary output on exit. It does not link or run them. On the
build server, build/run `matonos_bluetooth_audio_software_test`. It checks
software factories/PCM capabilities, offload/unknown rejection and reuse of
output parameters. `PROVENANCE.md` documents the source fork and licence.

Real hardware tests, on Ryzen/Intel 7265, Surface Pro 3/Marvell 88W8897 and
HP ProDesk 600 G1 with its chosen controller:

1. Build a complete fresh image; boot with Bluetooth off or no adapter.
   Confirm boot completion, primary playback, and provider/module registration.
2. Enable Bluetooth and run the public-API probe in the report. Record the
   controller's actual ISO capabilities alongside its API result. Do not
   treat lack of ISO on an older controller as a provider failure.
3. Pair a classic A2DP speaker/headset. Play, pause, change volume and unpair;
   check clean return to the local speaker, then repeat after a fresh boot.
4. On an ISO-capable controller, pair LE Audio headphones. Check API support,
   Settings' LE Audio control, output and microphone capture, reconnect and
   switching between local and LE audio. Test broadcast with a BIS-capable
   controller and receiver. Hearing-aid and HFP sessions need their own devices.
5. Unplug/replug the active USB controller while playing. Confirm primary
   audio and the rest of the system stay usable; inspect provider and HCI logs.
