# Shared changes needed

1. Add `flatpak org.matonos.flathub` to
   `device/maton/pc_x86_64/systembridge/res/raw/target_caller_allowlist.txt`.
   The existing bridge and linuxd already implement the required calls and
   progress event. The current ACL only authorizes `org.matonos.settings`, so
   this app cannot install or remove apps until its package is authorized.
2. Add `Flathub.apk` as a fixed `android_app_import` in
   `device/maton/pc_x86_64/buildinfra/Android.bp`, include the
   `privapp-permissions-org.matonos.flathub` file, and grant
   `org.matonos.permission.SYSTEM_BRIDGE` in that file. Expo apps use their own
   generated certificate, which `tools/build-apps.sh` adds to the certificate
   allowlist at build time. This imports the APK produced from this app's
   `apps.list` row into the image so built-in app authorization applies.

No AOSP or systembridge implementation change is needed.
