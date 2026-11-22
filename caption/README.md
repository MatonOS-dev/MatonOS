# MatonOS caption provider

The `org.matonos.caption` APK is the default caption provider. It implements
the version 1 `org.matonos.caption` AIDL contract and hosts one
`SurfaceControlViewHost` per task. A second binding action exposes its caption
settings surface. Shell retains window movement, resize, focus, immersive and
fallback behavior.

The provider draws nothing and reports no interactive regions when the task is
client decorated or buttons are disallowed. Otherwise, the caption contains
close, maximize/restore and minimize actions, plus the task icon and title.
Preferences live in the provider's private SharedPreferences and immediately
redraw attached task captions. The Settings slot binds the active package
named by `Settings.Secure/matonos_caption_provider`, defaulting to
`org.matonos.caption`, and shows an inline message if binding or surface
creation fails.

The app is registered in `buildinfra/apps/apps.list` and installed as the
fixed `MatonCaption` product APK import from `buildinfra/buildinfra.mk`.
Settings requests `org.matonos.permission.BIND_CAPTION_PROVIDER`; its
privileged permission allowlist includes the grant. The app itself does not
request privileged permissions.

## Validation

No Gradle, Soong, VM or adb build/test was run for this change, per the task
constraints. JSON parsing for the Expo module/app manifests and `git diff
--check` passed. Coordinator build and fresh-image verification are still
required.

## Device verification

After the coordinator has built and booted a fresh image:

1. Open two undecorated tasks with captions enabled; confirm titles and icons
   match each task and the selected dark/light palette follows task state.
2. Click close, maximize/restore and minimize separately. Confirm the shell
   receives each action and retains drag/resize behavior outside the reported
   button/title regions.
3. Test a client-decorated task and a task with buttons disallowed; both
   captions should be transparent and report an empty region list.
4. Open Settings → Window title bars. Change button side, style, title
   alignment and accent, then confirm all attached task captions update
   immediately.
5. Set `Settings.Secure/matonos_caption_provider` to an uninstalled provider
   component and confirm Settings shows an explanatory text line. Clear it to
   restore the built-in provider.
6. Repeat on the Ryzen/RX 6600 desktop, Surface Pro 3 and HP ProDesk 600 G1;
   caption rendering should not depend on optional PC hardware.

## Open items

- The coordinator must provide the existing `matonos-caption` signing key
  before running the registered Gradle build; this task intentionally did not
  create or rotate keys.
- The caption-host implementation and an Android 17 compile/image test are
  needed to validate the exact SurfacePackage transport and interactive
  region coordinate interpretation end to end.
