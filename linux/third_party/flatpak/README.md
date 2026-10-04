# Flatpak bionic consumer fork

Current release: **1.18.4**, NDK r30 / API 35 / x86_64-v2. See
`FORK-REVISION.md` and `../source-pins.json` for exact source provenance.
The former two patch files are retired; build the reviewed fork directly.

Run `MATON_AOSP=/path/to/aosp bash linux/flatpak/build-seccomp.sh` with the
matching NDK dependency prefix and AOSP product libraries. Maximum four jobs.
The script uses Meson, builds all targets, checks `ENABLE_SECCOMP`, compares
filter syscall numbers against libseccomp's table, checks BPF-export linkage,
and regenerates the shipped CLI, portal and revokefs helpers. It refreshes
`seccomp-config.h` and `seccomp.sha256` for staging. No image is built.

Dependencies retain the existing NDK GLib/GIO, JSON-GLib, libarchive, GPGME,
OSTree, AppStream and related stack. HTTPS/XML link the matching Android
platform libcurl/BoringSSL and libxml2. Seccomp stays linked to
`libseccomp_matonos.so`. Systemd, system helper, SELinux module, xauth,
introspection, docs, tests, dconf, malcontent, zstd and Wayland security-context
integration are disabled in this consumer cross-build. Apps still connect
through the existing Wayland/X11 socket and per-app broker plumbing.

Bionic retains the libglnx macros, consumer-only CLI/icon exclusions and host
libc/ldconfig exclusions. New GNU sort calls use GLib on Android. See the fork
revision notes for exact files and rationale. Runtime filter installation,
signed installation/update/removal, metadata, portal spawning and graphical
apps require the later r24 device test.

The broker minimum remains 1.14.10: RequestSession's signature/version and
monitor `path` key still match. Packaged version and compatibility floor are
separate. No `portals.c`, `session-control.h` or compositor source was changed.
