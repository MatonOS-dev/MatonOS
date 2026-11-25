# PipeWire stack provenance

`tools/build-pipewire.sh` builds the vendor runtime with Android NDK r30, API
35, and x86_64-v2 code generation. Sources are fetched by immutable commit or
verified release archive into build scratch; no upstream source tree is
vendored into this repository. The staged `buildinfra/native-built/pipewire/SOURCE`
records the selected commits and ALSA-lib archive hash.

| Component | Pin | License reviewed at source pin | Role |
|---|---|---|---|
| BoringSSL | `0cd1f6a94b670e6b61af95a06e71e690ae8e3262` (existing AOSP checkout) | Apache-2.0 plus retained ISC/OpenSSL notices (`LICENSE`) | Static libcrypto in RAOP sink; see `network-speakers/PROVENANCE.md` |
| PipeWire | `8fa27cabdc6c0c1350c69c026af5850ef0af1e26` (1.6.9) | MIT (`COPYING`) | Daemon, libpipewire, SPA support, audioconvert, audiomixer, ALSA, V4L2; RAOP and protocol-simple network sinks |
| WirePlumber | `11e501181fb87dc8e72e55156d672109fbad2434` (0.5.17) | MIT (`LICENSE`) | ALSA/V4L2 monitors and Lua default policy |
| ALSA-lib | 1.2.15; SHA-256 pinned in build script | LGPL-2.1-or-later (`COPYING`) | ALSA client library and UCM runtime |
| alsa-ucm-conf | `28776e1e22a952e4ab01037033380aa111985101` | BSD-3-Clause (`LICENSE`) | UCM profiles |
| GLib | `41eca60845d3fc309af361f5e7f801ba339099aa` (2.84.4) | LGPL-2.1-or-later (`COPYING`) | WirePlumber runtime (GLib/GObject/GIO); bundled PCRE2 is BSD-3-Clause and proxy-libintl is LGPL-2.1 |
| libffi | `83d0cfd00d7d37af4b4349511d29f1f0512621b3` | MIT (`LICENSE`) | GLib FFI dependency |
| libudev-zero | `7a6eee2db11f2eb0f7fd065ae9597fcd270e6734` | ISC (`LICENSE`) | udev-compatible enumeration and hotplug |
| Lua | 5.5.0; source SHA-256 `57ccc32bbbd005cab75bcc52444052535af691789dba2b9016d5c50640d68b3d`; Meson WrapDB patch SHA-256 `69ec4a2dd99ecf8e84830093d418f3a5be1202f16ba8d636b3008b67506e5cca` | MIT (`doc/readme.html`) | Built statically into WirePlumber |

The reviewed license texts contain no GPLv3-licensed linked component. The
ALSA-lib archive is fetched from alsa-project.org and SHA-256 checked before
extraction. PipeWire's own subprojects selected by Meson are built only where
needed; their actual runtime linkage is checked from the final ELF `DT_NEEDED`
tags after each build. The build copies license texts for all listed
components and GLib's static/dynamic fallback dependencies, plus Lua's license, to
`share/licenses/pipewire-stack` in the ODM payload.

## Android compatibility patches

The build checks and applies only the following small local patches to the
pinned source revisions:

- `audio/patches/0001-bionic-data-loop-stop.patch`: bionic lacks
  `pthread_cancel`; use PipeWire's existing loop stop callback.
- `audio/patches/0002-bionic-no-thread-affinity.patch`: bionic lacks the
  optional pthread affinity API used for realtime tuning.
- `audio/patches/0003-respect-pipewire-alsa-option.patch`: gate the ALSA
  compatibility plugin subdirectories on the Meson option. The requested SPA
  ALSA backend is separately enabled; this prevents a disabled compatibility
  plugin from being built merely because ALSA was found.
- `audio/patches/0004-build-alsa-spa-on-android.patch`: Android NDK Meson
  targets report `android`, although the ALSA SPA backend uses Linux kernel
  ALSA interfaces.
- `audio/patches/0005-avoid-glibc-fopencookie-on-bionic.patch`: bionic lacks
  `fopencookie`; optional ALSA diagnostic output uses `/dev/null` on Android.
- `audio/patches/wireplumber/0001-define-po-helper-for-minimal-build.patch`:
  define the `spa_json_po` input consumed by WirePlumber's build when the
  documentation/translation build path is disabled.
- `audio/patches/wireplumber/0002-bionic-optional-libintl.patch`: bionic has no
  libintl library; the single-locale compatibility header provides the
  no-op translation macros used instead.

PipeWire 1.6.9 does not offer a Meson switch for its Pulse protocol module, so
the build removes `libpipewire-module-protocol-pulse.so` and the
`pipewire-pulse` alias from the packaged runtime. `pipewire-alsa` compatibility
plugins are explicitly disabled; the requested ALSA SPA backend remains on.

These patches are not AOSP patches. Their applicability is checked against
the pinned upstream commits by `tools/build-pipewire.sh`; a patch that neither
applies nor is already present fails the build.

The staged runtime is only the build deliverable. This work does not start
PipeWire, register the future AIDL HAL, or claim runtime audio verification.
