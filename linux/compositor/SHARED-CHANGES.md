# Shared changes required

The compositor host app needs two integration edits before it can ship in the image:

- Append `matonos-wayland-host<TAB>linux/compositor/host<TAB>:app:assembleRelease<TAB>MatonWaylandHost.apk` to `buildinfra/apps/apps.list`. This builds and stages the Java host APK with its app-specific MatonOS development key.
- Add an `android_app_import` named `MatonWaylandHost` in `buildinfra/Android.bp` with `apk: "apps-built/MatonWaylandHost.apk"`, `system_ext_specific: true`, `preprocessed: true`, and `dex_preopt.enabled: false`; add `MatonWaylandHost` to `PRODUCT_PACKAGES` in `apps/apps.mk`. This installs the signed host app in the image. It requests no privileged permissions.

These files are shared build registries and have no `# compositor:` block. They remain unchanged pending coordinator integration.

- Add a `MATON_APPS_ONLY` key filter to `tools/build-apps.sh`, or equivalent
  supported per-app selection, so `MATON_APPS_ONLY=matonos-wayland-host`
  builds only this Java app. The current all-app script exits on the earlier
  Settings `npm ci` failure, preventing this independent app's Gradle build
  from running. This tool change is coordinator-owned under the common rules.
