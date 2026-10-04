# r24 fork revision — Flatpak 1.18.4

Upstream `1.18.4`: `a02d0ba48abe9aacc377de15699e5d8c024b5669`.
Release archive SHA-256:
`b899a7a00c48d2c626cb8ec33fe556720b376c25805d7222430c0fdca4c6ac8d`.

Local `upstream/` is a real Git checkout on `matonos/v1.2`, populated with
this exact release archive, including vendored libglnx. MatonOS changes are
uncommitted per user instruction; no new fork commit was published.
`source-pins.json` independently hashes the complete reviewed build tree.
Review snapshot: ignored `build/deps-update/flatpak-fork.tar.xz`.

- Rebase 0001 libglnx compatibility: `strdupa` stack-copy and `IFTODT` mode
  conversion, both guarded by `__BIONIC__` and absence of the upstream macro.
- Replace 0002 Autotools edits with Meson: on Android omit the nine build-only
  CLI sources, their command entries/prototypes, gdk-pixbuf and icon validator.
  Keep consumer commands and bundle installation.
- Rebase 0002 host OS export and ldconfig exclusions. Preserve the rest of
  upstream's new fd-relative/chaseat security validation.
- New API-35 compatibility: replace the two GNU `qsort_r` calls with GLib's
  `g_qsort_with_data` under `__BIONIC__`; non-Android builds retain upstream.
- Retire the old patch files; no new patch series.

`linux/flatpak/build-seccomp.sh` now builds the complete Meson payload with
`seccomp=enabled`, matching libseccomp_matonos and platform libcurl/libxml2.
It stages the same shipped helper inventory (CLI, portal, revokefs), checks
seccomp syscall numbers/linkage and refreshes the config/hash guard. Other
built helpers remain private build artifacts; session-helper host spawning
is not shipped. The broker emulates the safe session-helper API.

Flatpak 1.18.4 still requires only OSTree >=2020.8 and GLib >=2.46; existing
NDK dependency pins are retained. Host ABI libraries must match the final
system image. No image or VM build/run was performed here.
