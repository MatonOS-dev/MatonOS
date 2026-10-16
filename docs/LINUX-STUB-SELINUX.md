# Linux stub SELinux (v3.5)

Generated device-signed stubs run in stock untrusted_app_* with ordinary
PackageManager-owned per-user data and levelFrom=all categories. The subsystem
authenticates store/repair calls using the caller UID, sole package, pinned
signer and declared ref. There is no linuxd Binder service or domain.

StubService creates its private runtime sockets. Staging is inside the
subsystem app's private cache; APK code is owned by PackageManager. There is
no shared /data/linux-app store, code/staging label, CHOWN capability or lease.
Flatpak executables are native libraries shipped in the subsystem APK and
resolved beside its v3 loader. The compat-proxy loader blocks Binder ioctls for
Linux payloads; the Java parent keeps Binder for scoped platform work.

See [retirement and device checks](RETIRE-LINUXD.md). Fresh enforcing-image
validation remains the coordinator's task.
