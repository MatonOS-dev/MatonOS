# Linux app stack

The Flatpak APEX uses the static Alpine 3.24 stack documented in
[`flatpak/APEX.md`](flatpak/APEX.md). Its selected pair is Flatpak 1.16.6 and
bubblewrap 0.12.0, with OSTree 2025.7, DullPGP and BoringSSL. The former
bionic build glue and generated bionic prebuilts are retained under
`../retired/flatpak-bionic/` for reference and are not included by Soong.

The current APEX payload contains one static multicall ELF, applet symlinks,
two static-NDK launch helpers, a bionic host launcher and store helper, and
the minimum Flathub key/remote files. `linux/third_party/source-pins.json`
records the selected Flatpak, OSTree and bubblewrap pins. Artifact hashes are
in `flatpak/prebuilt/static/SOURCE`.
