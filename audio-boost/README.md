# Settings volume boost

## Design

The Settings Hardware page exposes a 100–200% slider. The persisted value is
`Settings.Secure.matonos_volume_boost_percent`; 100% releases the effect and
200% applies +6 dB. `VolumeBoostService` owns a public API
`DynamicsProcessing` effect on output session 0, with its limiter enabled at
−1 dBFS. The Settings boot receiver starts the sticky service, and a secure
setting observer applies slider changes while the service runs. Opening the
Hardware page also starts the service again if a boost was saved.

The Settings app already has `WRITE_SECURE_SETTINGS` in both matching
privileged allowlists. It declares `MODIFY_AUDIO_SETTINGS` for the output-mix
effect and `RECEIVE_BOOT_COMPLETED` for boot restore. No additional privileged
permission or AOSP patch is needed.

The product inherits `hardware/interfaces/audio/aidl/default/audio_effects.mk`.
That installs `audio_effects_config.xml`, whose default catalog contains the
`dynamics_processing` effect backed by `libdynamicsprocessingaidl.so`.

## Validation and limitations

The Java service compiles against API 36.1's public `android.jar`. A full
Settings APK build and device test were not run by this task. Android force-stop
terminates the Settings package and its session-0 effect; `START_STICKY` cannot
override force-stop. The saved value is restored when Settings is started again
and the Hardware page reads it. Continuous boost through force-stop would need
an independent always-running package or system-side owner.

## Real hardware test

On a fresh image with speakers or headphones connected:

1. Play a fixed audio sample and set the system media volume to 100%.
2. Open Settings → Hardware. Confirm Volume boost starts at 100%.
3. Move the slider through 100%, 150%, and 200%; confirm output gain increases
   and loud peaks are limited without audible clipping.
4. Set 150%, restart the PC, open the Hardware page, and confirm it still shows
   150% and the boost is active.
5. Force-stop Settings, then reopen Settings → Hardware. Confirm the saved
   value is restored and the effect becomes active again. The effect is absent
   during the interval after force-stop and before Settings restarts.
6. Return to 100% and confirm normal output returns.
