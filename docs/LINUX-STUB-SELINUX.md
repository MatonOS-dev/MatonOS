# Linux stub SELinux

Generated device-signed stubs run in stock untrusted_app_* with ordinary
PackageManager-owned per-user data and levelFrom=all categories. linuxd and the
System Bridge authenticate scoped management requests by certificate and live
PackageManager identity. No custom stub domain or MLS trust is added.

StubService creates `<dataDir>/runtime` mode 0700. Its sockets, symlinks, X11,
audio and bus use stock app_data_file policy at the stub's UID/MLS level.
linuxd cannot create, label or access these runtime sockets. No media_rw_data_file
runtime exception, global tmpfs, socket type, input metadata policy or Downloads
bind grant remains.

Shared `/data/linux-app` code uses deployment/code labels with read-only app
access and process leases. Explicit neverallows deny app mutation and
execute_no_trans. The compat-proxy loader blocks Binder ioctls for Linux
payloads; the Java parent retains Binder for authenticated platform work.
See [storage](../install/linuxd/STORAGE-LAYOUT.md) and
[launch](../linux/compositor/V3-LAUNCH.md). Fresh enforcing-image validation is
pending; no new exception should be added to compensate for an unused v2 path.
