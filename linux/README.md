# Linux app stack

The Flatpak APEX uses the static Alpine 3.24 stack documented in
[`flatpak/APEX.md`](flatpak/APEX.md). Its selected pair is Flatpak 1.16.6 and
bubblewrap 0.12.0, with OSTree 2025.7, DullPGP and BoringSSL. The former
bionic build glue is gone; nothing under this tree points at it any more.

The APEX payload is one static multicall ELF, applet dispatch through
`argv[0]` (Soong drops prebuilt-binary symlinks), the static-musl bwrap and
app-exec launch helpers, the static-musl host launcher and stage-only store
helper, and the minimum Flathub key/remote files.
`linux/third_party/source-pins.json` records the selected Flatpak, OSTree and
bubblewrap pins. Artifact hashes are in `flatpak/prebuilt/static/SOURCE`.
