# Handover — night 2026-11-20 → morning 2026-11-21

## Done

- **Disk layout merged.** build47 live → installed to a blank virtual HDD in 45 s → the installed disk boots
  (60 s, `ro.boot.matonos.live=0`, slot `_a`, `/data` ext4 44G on dm-60). The
  `metadata-enc` branch was fast-forwarded into `main` (17aa3ff) and pushed. Old VM images
  build45/build46 were deleted; build47-live.img + hdd43.img are kept.
- **Store refuses installs on live boots** (rn-apps/flathub a2f541f, 48df629): it shows a notice
  and `sourceInstall` throws when `ro.boot.matonos.live=1`. On the installed disk no notice appears.
- **Subsystem: killed-install leftovers swept** on service start (5e4de1e6). Verified on VM:
  `kill -9` mid-download (160 MB staged) → the bound service restarts → `store-installs` is empty.
- **Subsystem: one Android package per Flatpak id across branches** (43601de3, branch
  `flatpak-pkg-split`, pushed). GNOME 51 pulls `GL.default` 26.08 **and** 26.08-extra; before the fix
  both mapped to one package name → `Duplicate Android package`. Now:
  `flatpak.org.freedesktop.Platform.GL.default: splits=[base, x86_64.v26_08, x86_64.v26_08_extra]`.
  The launch argv binds both branches in the stock Flatpak order (extra last), so it matches upstream.
- **F-Droid Install → Open** works (Satunes, 72 s).
- **"Store open during subsystem reinstall shows only F-Droid"**: not reproduced on this build.

## Blocker (parked, needs your call): GNOME/glycin apps abort at launch

Klotski installs fine but aborts (exit 134) before showing a window:

```
Gtk:ERROR:gtkiconhelper.c:495: Failed to load .../image-missing.svg:
Loader process exited early with status '126'
Command: flatpak-spawn --sandbox ... prlimit --as=8261753241 /usr/libexec/glycin-loaders/2+/glycin-svg
```

Cause: glycin caps each image loader with `RLIMIT_AS` ≈ 8 GB. Every v3-proxied process already
holds **10–28 GB VSZ** of PROT_NONE reservations from the resident bionic host
(`[anon:scudo:primary_reserve]` ≈ 8 GB, `[anon:cfi shadow]` ≈ 1.8 GB, …; measured on the VM).
The proxy passes `RLIMIT_AS` straight to the kernel (proxy.c `PROXY_SC_prlimit64`/`setrlimit` only
special-case NOFILE), so `prlimit`'s exec of the loader fails → `prlimit` exits 126. This hits
every app whose GTK loads images through glycin (recent GNOME runtimes), not only Klotski. The
Steam/zenity glycin abort in compat-proxy/README "round 9/10" is very likely the same thing.

Recommendation (not implemented): treat the layer's own reservations as our delta
([[cost-relative-to-desktop]]): on set of `RLIMIT_AS`, apply `requested + host-reserved VA` to the
kernel and report `requested` back on get. Alternative: shrink the host's reservations (scudo
primary size / CFI shadow) in the loader. Not done overnight because it changes proxy-core rlimit
semantics.

`flatpak-pkg-split` is **not merged** into subsystem main yet, because the merge condition was
"Klotski launches". The packaging itself is verified; merge whenever you're happy.

## Open notes

- The store shows no progress on the Install button during F-Droid/Flatpak installs (it stays
  "Install" until it flips to "Open").
- The installer step list repeats "Write boot and loader files" four times.
- The Updates badge shows 4 on a fresh install (not investigated).
- Subsystem stub cold start is ~28 s from process start to the loader (Klotski first launch).
- Still waiting on your decisions: F-Droid 41 s index parse, RAM disk sizing, Hardware page
  "Unknown", preapprovals 1–7.

VM: `hdd43.img` installed build47 + test APKs, adb `localhost:5557`, headless.
