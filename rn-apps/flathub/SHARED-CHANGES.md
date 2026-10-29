> v3.5: linuxd is retired. Any linuxd/shared-store paths below describe the
> historical design. See [current ownership and device checks](../../docs/RETIRE-LINUXD.md).

# Shared changes needed

1. Add `flatpak org.matonos.flathub` to
   `device/maton/pc_x86_64/systembridge/res/raw/target_caller_allowlist.txt`.
   The existing bridge and linuxd already implement the required calls and
   progress event. The current ACL only authorizes `org.matonos.settings`, so
   this app cannot install or remove apps until its package is authorized.
2. Add a fixed `android_app_import` module (for example, `MatonOSFlathub`) to
   `device/maton/pc_x86_64/buildinfra/Android.bp`, with
   `apk: "apps-built/Flathub.apk"`, `system_ext_specific: true`,
   `privileged: true`, `preprocessed: true`, and the required permission
   module. Add a `prebuilt_etc` module sourcing
   `privapp-permissions/privapp-permissions-org.matonos.flathub.xml`.
3. Add both modules to `PRODUCT_PACKAGES` in
   `device/maton/pc_x86_64/buildinfra/buildinfra.mk`, so the APK and its
   privileged permission grant enter the image. The APK is already staged
   from `buildinfra/apps/apps.list`; its permission XML is present in
   `buildinfra/privapp-permissions/`, and `tools/build-apps.sh` has registered
   the app certificate.

4. Phase 2 requests `android.permission.INSTALL_PACKAGES` and
   `android.permission.DELETE_PACKAGES`; both must be granted in the app's
   privileged permission XML. `QUERY_ALL_PACKAGES` is declared in the app
   manifest as a normal permission and must not be listed as a privileged
   grant. Include the updated permission XML in the image. No bridge method is
   needed: the privileged Software Center owns the APK PackageInstaller
   session.

No bridge implementation change is needed. The current successful image was
checked and contains no Flathub APK, so QEMU UI verification must wait until
the shared packaging entries above are included in an image.
