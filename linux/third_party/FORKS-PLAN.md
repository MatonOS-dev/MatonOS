# Linux third-party fork plan

Local repositories have been prepared under each component's `upstream/`
directory on branch `matonos/v1.2`. Each branch starts at the actual upstream
Git tag, then imports the complete pinned release source snapshot, then carries
each MatonOS compatibility change as a separate commit named after its former
patch. The snapshot import is needed because the tested release archives carry
generated files and, for Flatpak, vendored subprojects which are absent from
the Git tag tree. These are local histories only: no remote was created and
nothing was pushed. The coordinator should create the MatonOS-dev repositories
and manifest entries from these pins.

Flatpak-specific handoff: `0001-bionic-glnx-compat` edits
`subprojects/libglnx`, which is a Git submodule in the upstream Git tag but is
vendored in the release archive used for the bionic build. Preserve that
release snapshot import when creating the MatonOS fork, or fork/re-pin libglnx
separately before removing the import layer.

## Components with MatonOS changes

| Component | Upstream git URL | Release tag (resolved commit) | MatonOS commits |
|---|---|---|---|
| flatpak | https://github.com/flatpak/flatpak.git | `1.14.10` (`5a2503f1e8ef94364e3060ec440546cc47af7fa5`) | `0001-bionic-glnx-compat`; `0002-bionic-prefix-host-paths` |
| ostree | https://github.com/ostreedev/ostree.git | `v2024.5` (`f3b66e8c2db4953c14cef048f59a39c039d53433`) | `0001-bionic-strdupa`; `0002-bionic-iftodt`; `0003-bionic-version-string-compare`; `0004-bionic-endian-conversion` |
| bubblewrap | https://github.com/containers/bubblewrap.git | `v0.10.0` (`dc63ec667e6546f34e0b6c088cdf8ae7c7dea0f3`) | `0001-c23-bool-guard`; `0002-bionic-getcwd`; `0003-bionic-root-bind-realpath`; `0004-bionic-newroot-subpath-realpath` |
| glib | https://gitlab.gnome.org/GNOME/glib.git | `2.84.4` (`41eca60845d3fc309af361f5e7f801ba339099aa`) | `0001-bionic-avoid-c23-bool-identifier` |
| gnupg | https://github.com/gpg/gnupg.git | `gnupg-2.5.2` (`84e1781201489e50888c9415bb2625f9dd27cb8a`) | `0001-only-build-gpg-disable-agent` |
| npth | https://github.com/gpg/npth.git | `npth-1.8` (`64905e765aad9de6054ef70a97fc30bd992ce999`) | `0001-bionic-probe-pthread-create` |
| libgpg-error | https://github.com/gpg/libgpg-error.git | `libgpg-error-1.51` (`b0bb9266010d84b30fa2dc6a2127b7e40dc03660`) | `0001-bionic-x86_64-lock-object` |
| libfyaml | https://github.com/pantoniou/libfyaml.git | `v0.9.6` (`1ed9bae3b1e5fc57e31a14a2ea12810fdf423473`) | `0001-bionic-no-libpthread` |
| appstream | https://github.com/ximion/appstream.git | `v1.2.0` (`ce52fb058e06f1f36320911e93e6773c6e3fe82e`) | `0001-library-only-meson-options` |

## Pinned upstreams without MatonOS patches

| Component | Upstream git URL | Release tag (resolved commit) |
|---|---|---|
| gdk-pixbuf | https://gitlab.gnome.org/GNOME/gdk-pixbuf.git | `2.42.12` (`e4315fb8553776e13d39e3f2e0ea8792db61720c`) |
| gpgme | https://github.com/gpg/gpgme.git | `gpgme-1.23.2` (`1a26db717575068f0ab0d00a437ae870a93e1bb8`) |
| json-glib | https://gitlab.gnome.org/GNOME/json-glib.git | `1.8.0` (`ebdcfface7c6464e1683cae99ccac4295baddf2b`) |
| libarchive | https://github.com/libarchive/libarchive.git | `v3.7.7` (`b439d586f53911c84be5e380445a8a259e19114c`) |
| libassuan | https://github.com/gpg/libassuan.git | `libassuan-3.0.1` (`c9e902705a50abaf532c9d24347dfd1f3b5779fb`) |
| libgcrypt | https://github.com/gpg/libgcrypt.git | `libgcrypt-1.11.1` (`81ce5321b1b79bde6dfdc3c164efb40c13cf656b`) |
| libjpeg-turbo | https://github.com/libjpeg-turbo/libjpeg-turbo.git | `3.2.0` (`c85e6b905bf237038faa936dab160ebfc5da0344`) |
| libksba | https://github.com/gpg/libksba.git | `libksba-1.6.7` (`b14e68b97df754b2bb7a90bb904d143d8e896afb`) |
| liblzma (XZ) | https://github.com/tukaani-project/xz.git | `v5.8.4` (`d3e650e63c110e830fd5391e7f8b45df0b91d3da`) |
| libpng | https://github.com/pnggroup/libpng.git | `v1.6.58` (`3061454d980de7d53608f594194cfac722721d2a`) |
| libseccomp | https://github.com/seccomp/libseccomp.git | `v2.5.5` (`f0b04ab0b4fc0bc2cde6da1f407b4a487fe6d78f`) |
| libxmlb | https://github.com/hughsie/libxmlb.git | `0.3.26` (`f969b4dd491a7367f7500f55f576c810985363d7`) |
| libyaml | https://github.com/yaml/libyaml.git | `0.2.5` (`2c891fc7a770e8ba2fec34fc6b545c672beb37e6`) |

The 13 unpatched components remain clean release snapshots in this tree; the
coordinator can fetch their listed upstream Git tags into MatonOS-dev repos.
`gdk-pixbuf`, `libpng`, and `libjpeg-turbo` are intentionally unused by the
current Flatpak build; they are listed because their pinned source trees remain
present.

## Verification

Each patched local history was checked for its branch name, clean worktree,
upstream tag parent, release snapshot import, and one commit per listed
MatonOS change. Upstream tags and commit IDs above are the pins used for the
tested source trees.
