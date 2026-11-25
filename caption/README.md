# MatonOS caption provider

The `org.matonos.caption` APK is the default caption provider. It implements
the version 1 `org.matonos.caption` AIDL contract and hosts one
`SurfaceControlViewHost` per task. A separate `org.matonos.appsettings` service exposes its settings surface
through the generic MatonOS App settings contract. Shell retains window movement, resize, focus, immersive and
fallback behavior.

The provider draws nothing and reports no interactive regions when the task is
client decorated or buttons are disallowed. Otherwise, the caption contains
close, maximize/restore and minimize actions, plus the task icon and title.
Preferences live in the provider's private SharedPreferences and immediately
redraw attached task captions. Settings discovers this surface under App settings and binds the exact
service component. The caption provider remains selected independently for
window decorations.

The app is registered in `buildinfra/apps/apps.list` and installed as the
fixed `MatonCaption` product APK import from `buildinfra/buildinfra.mk`.
The caption provider service requires
`org.matonos.permission.BIND_CAPTION_PROVIDER`; the separate embedded
settings service requires `org.matonos.permission.BIND_EMBEDDED_SETTINGS`.
Settings is allowlisted for both permissions. See
[`docs/embedded-app-settings.md`](../docs/embedded-app-settings.md) for the
service contract. The app itself does not
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
4. Open Settings → App settings → MatonOS Captions. Change button side,
   style, title alignment and accent, then confirm attached task captions
   update immediately.
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

## Third-party assets

The window button icons in `app/src/main/res/drawable/caption_*.xml` are
Lucide icons (`octagon-x`, `diamond-plus`, `diamond-minus`), converted to
Android vector drawables. Lucide is ISC-licensed: Copyright (c) for portions
of Lucide are held by Cole Bemis 2013-2022 as part of Feather (MIT), all
other copyright (c) for Lucide are held by Lucide Contributors 2022.
https://lucide.dev/license

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
    SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
    OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
    CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
