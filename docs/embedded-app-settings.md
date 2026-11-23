# Embedded app settings

Apps can expose a settings page inside MatonOS Settings by exporting a bound service protected by `org.matonos.permission.BIND_EMBEDDED_SETTINGS` and declaring the `org.matonos.appsettings.EMBEDDED_SETTINGS` intent action. The permission is `signature|privileged`; MatonOS Settings is the privileged client.

Implement `org.matonos.appsettings.IEmbeddedSettings` and its callback `IEmbeddedSettingsHost` from the AIDL contract in `org.matonos.appsettings`. The host calls `open(hostToken, displayId, widthPx, heightPx, host)`. The app creates a `SurfaceControlViewHost` on that display using the supplied token, sends its `SurfacePackage` with `host.setSurface`, and may call `setPreferredHeight`. It handles `resize(widthPx, heightPx)` by relaying out the hosted surface and releases the host on `close()`.

The service may declare string metadata `org.matonos.appsettings.label` and drawable-resource metadata `org.matonos.appsettings.icon` to customize its list entry. The label and icon fall back to the app label and icon. The service must be exported, require the binding permission, and be safe to bind repeatedly; the Settings host closes the session when its page leaves the screen.
