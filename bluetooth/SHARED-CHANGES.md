# Shared changes requested

Please update the system bridge in its owned directory:

1. Remove `bluetooth org.matonos.settings` from
   `systembridge/res/raw/target_caller_allowlist.txt`; the Bluetooth control
   channel and adapter picker no longer exist.
2. Remove `"bluetooth"` from `knownTargets()` in
   `systembridge/src/org/matonos/systembridge/SystemBridgeService.java` so the
   retired target cannot be trusted or called.

No app-side typed Bluetooth wrapper or adapter picker exists in this checkout.
There is no audio code dependency on `vendor.maton.bluetooth.present`. Audio
now uses the static stock A2DP/LE Audio module and provider; it does not need a
Bluetooth control channel or a bridge API.
