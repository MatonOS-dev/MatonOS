# Shared changes needed outside this worktree

The tray host APK source is in `/home/hanro50/matonos/repos/linux-subsystem-app`, outside the authorized `dt-components` worktree. In `linux/subsystem/host/dbus-tray/src/main/AndroidManifest.xml`, update the `TrayHostService` declaration as follows so only callers granted the platform permission can bind:

```xml
<service android:name=".TrayHostService"
    android:exported="true"
    android:permission="org.matonos.permission.BIND_TRAY_HOST">
    <intent-filter>
        <action android:name="org.matonos.tray.HOST" />
    </intent-filter>
</service>
```

The permission is declared as `signature|privileged` in `apps/MatonSystemUI/AndroidManifest.xml`. The Launcher3 patch requests it and adds the allowlist module to Launcher3's existing `required` list. `buildinfra/privapp-permissions/privapp-permissions-com.android.launcher3.xml` grants it, and `buildinfra/Android.bp` installs that XML into `system_ext/etc/permissions`.
