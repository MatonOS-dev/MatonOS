<!-- SPDX-License-Identifier: Apache-2.0 -->
# Settings volume boost

## Design

The Settings Hardware page exposes a 100–200% slider and stores its value in
`Settings.Secure.matonos_volume_boost_percent`. MatonSystemUI owns the output
mix effect because SystemUI stays running after Settings leaves the foreground.
`MatonSystemUIInitializer` creates one `VolumeBoostController` at SystemUI
startup; the controller observes the secure setting and owns a session-0
`DynamicsProcessing` effect with a −1 dBFS limiter. 100% or a missing setting
releases the effect; 200% applies up to +6 dB.

Settings only reads and writes the secure setting. Its boot receiver remains
for enabling the live-image install alias, so `RECEIVE_BOOT_COMPLETED` remains
needed. Settings no longer needs `MODIFY_AUDIO_SETTINGS`; the stock SystemUI
manifest already declares that permission. No AOSP patch or additional
privileged permission is required. The product's default audio-effects
catalog includes `dynamics_processing` backed by
`libdynamicsprocessingaidl.so`.

## Validation and limitations

The controller is compiled directly with `javac` against API 36.1's public
`android.jar`; see the task report for command and exit status. A full APK
build and VM/device test were not run. On devices with unsupported dynamics
processing, SystemUI logs the failure and continues without the effect.

## Real hardware test

On a fresh image with speakers or headphones connected:

1. Play a fixed audio sample and set system media volume to 100%.
2. Open Settings → Hardware and confirm Volume boost starts at 100%.
3. Move the slider through 100%, 150%, and 200%; confirm output gain increases
   and loud peaks are limited without audible clipping.
4. Set 150%, leave Settings in the background for over a minute, and confirm
   boost remains active.
5. Force-stop Settings and confirm boost remains active; reopen Settings and
   confirm the slider still reads 150%.
6. Restart the PC and confirm 150% is restored and active after boot.
7. Return to 100% and confirm normal output returns.
