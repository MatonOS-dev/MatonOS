# Shared changes

- DONE — `systembridge/src/org/matonos/systembridge/SystemBridgeService.java`:
  added marked `// sleep:` construction/start and teardown hooks for the new
  `AndroidWakeStateForwarder`; reason: run polling from the platform-signed
  bridge lifecycle without changing its Binder API.
- DONE — `systembridge/AndroidManifest.xml`: requested `android.permission.DUMP`
  in a marked sleep comment; reason: PowerManager's lock table is available
  through its privileged Binder dump.
- DONE — `systembridge/sepolicy/system_ext/private/matonos_system_bridge.te`:
  added a marked `power_service` lookup rule; reason: let only the bridge
  inspect the stock power service.
- NO CHANGE NEEDED — `buildinfra/bundle/contents.list`,
  `buildinfra/init/matonos-sleepd.rc`, and `bundle/matonos-channel.xml` already
  ship/start sleepd and declare the sleep channel.
- BLOCKED — shared bridge owner should investigate why
  `org.matonos.systembridge` is not registered from its
  `/system_ext/priv-app/MatonSystemBridge/MatonSystemBridge.apk`. On a fresh
  QEMU boot, PackageManager omitted it from `pm list packages`; direct install
  failed with `INSTALL_PARSE_FAILED_UNEXPECTED_EXCEPTION`:
  `ParsedProviderImpl cannot be cast to java.lang.String`. Without the bridge
  process, the sleep forwarder cannot run, so the end-to-end hold/release test
  is blocked. The sleep agent made no edits to other agents' provider entries.
