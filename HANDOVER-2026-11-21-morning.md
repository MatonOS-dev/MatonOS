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

## Update morning 2026-11-21: Klotski WORKS on `flatpak-pkg-split` (pushed, not merged)

- 50799198 proxy: emulate `RLIMIT_AS` as requested + host-reserved VA (scudo/CFI/bionic ranges);
  glycin loaders now exec and serve images.
- c060203c stub: window activities reuse the process Flatpak ref. With launcher splits the ref
  lives only on the activity-alias, so every per-window StubActivity returned silently and
  stayed on "Starting application…". Verified on VM: Klotski window renders.
- Cold start after force-stop: tap → window output ≈ 3 s (the ~28 s was first launch after install).
- **New: Minecraft (Flathub, extra-data) install fails in apply_extra**: `bwrap: Unable to open lock
  file /usr/.ref` + `tar: Minecraft.tar.gz: Cannot open`. The package and its icon still get
  installed on this build. Codex report-only investigation: out/pc-logs/agents/minecraft-extra.final.md.
  Full log: out/pc-logs/agents/minecraft-apply-extra-error.txt.

## Update 2026-11-21 14:05: Minecraft installs; launcher blocked on getifaddrs

- c22eb28a (flatpak-pkg-split, pushed): images carry Flatpak's empty 0644 `files/.ref`
  (apply_extra's bwrap `--lock-file /usr/.ref` is unconditional — this was the real cause, the tar
  error was a side effect of Flatpak removing the checkout), and extra-data app images are now rooted
  at `files/` like every app image (were under `/app/` → `/app/app/bin/minecraft`, execvp ENOENT).
  VM: install → Open, launcher binary runs.
- Next blocker: launcher says "No network connection available". glibc `getifaddrs` needs netlink
  `bind` + `RTM_GETLINK`, both AOSP neverallows for untrusted apps. Codex job `netlink-getifaddrs`
  (proxy emulation, report only/uncommitted) running.

## (Original) Blocker: GNOME/glycin apps abort at launch

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

- ~~Store shows no install progress~~: false alarm, the button shows "Installing…" during installs.
- Installer step list repeated "Write boot and loader files" ×4 → fixed (metadata-enc ba98b1e,
  not yet live-verified).
- Updates badge 4 is **correct**: the image ships Fennec 156 (F-Droid 160), Thunderbird 33 (34),
  microG GmsCore + Companion 0.3.16 (microG repo 0.3.18). Bumping the prebuilts in the image clears it.
- ~~Subsystem stub cold start ~28 s~~: no longer reproduces (see 20:15 note).
- Still waiting on your decisions: F-Droid 41 s index parse, RAM disk sizing, Hardware page
  "Unknown" (by design: settings `HardwareModule.kt` returns null for Wi-Fi/BT/audio/GPU until a typed
  vendor-daemon summary is exposed through the bridge; needs a decision on that API), preapprovals 1–7.

VM: `hdd43.img` installed build47 + test APKs, adb `localhost:5557`, headless.

## Update 2026-11-21 15:45: Minecraft launcher runs (CEF up), no window yet, DNS fails in-app

- 392cf20a (flatpak-pkg-split, pushed): proxy emulates NETLINK_ROUTE autobind + RTM_GETLINK
  (getifaddrs works → network check passes, launcher downloads 315 MB), and `/proc/self/exe` now
  keeps the exec path instead of the ambiguous reverse-map (Flatpak `--persist` made the bootstrap
  re-exec forever, ~90 loaders). VM: bootstrap starts the real launcher once, CEF initialises.
- Open: launcher shows no window ("Application is running, but no window has appeared"), and its
  curl reports "Could not resolve host" for every Mojang host, although Android resolves them.
  Proxy log shows only expected nscd/resolved ENOENT connects. Next: check /etc/resolv.conf and the
  :53 path inside the Minecraft sandbox.
- 15:55 — DNS fixed: the proxy's netd DNS helper silently dropped queries netd failed (-EIO, 16 per launch),
  so glibc timed out → "Could not resolve host". Now SERVFAIL + loud logs (flatpak-pkg-split, pushed).
  Launcher UI JS loads, a "Minecraft Launcher" window appears but stays **white** — next: CEF rendering.
  Open question: why netd resnsend returns -EIO for some queries (likely AAAA on IPv4-only VM net).

## Update 2026-11-21 16:40: white Minecraft window = stale binder in the forked GPU process

- The Unzipper `std::bad_function_call` loop is gone once the update is current; not the cause (codex
  report cef-unzipper.last.md, inconclusive anyway).
- Real cause (VM pid 14770 = CEF GPU process): the browser process initialised the Vulkan forwarder
  (binder open + mmap), then fork+exec'd the GPU process; v3 **resident exec** keeps the bionic host, so
  the child has binder fd 13 but no binder mmap → `IPCThreadState: Driver returned error (BAD_VALUE)`,
  `rust_binder: get_current_thread was called from the wrong process`, `GraphicBufferAllocator ... -129`
  → `CreateSwapchainKHR VK_ERROR_OUT_OF_DEVICE_MEMORY` → zink no swapchain → white window.
  Generic: any Linux app whose parent touches the GPU before fork+exec (Chrome escapes via zypak).
- Fix in progress: codex `resident-exec-binder` — on exec, if Android state was initialised by another
  pid, hand off to a fresh resident via the existing kernel-exec path.

## Update 2026-11-21 17:40: Minecraft Launcher RENDERS (sign-in screen)

- 06966be8 (flatpak-pkg-split, pushed): binder opens record the owner pid; an exec in another pid takes
  the reset-loader handoff (fresh resident) and drops resident-owned fds. Host tests incl. new
  tests/fresh-resident.sh pass. VM: 6× `android-state-fresh-exec`, no allocator/binder errors, Minecraft
  Launcher shows the language picker + SIGN IN. Klotski still renders. Screenshot scratch mc-run8.png.
- Not tested: actually signing in / starting the game (needs a Microsoft account).
- Seen on the way (not fixed): after a subsystem APK reinstall, a still-running stub dies on the old
  /data/app lib path; Android then restarts StubService without its activity → `ref=null`, the stub
  waits forever for the command socket and the app never opens until force-stop. Known class
  ([[update-restarts-linux-apps]]); a restarted StubService with no ref should stop itself loudly.

## Update 2026-11-21 18:45: small follow-ups (flatpak-pkg-split, pushed, not merged)

- 1b393ab3 stub: a StubService start redelivered after a crash (START_FLAG_RETRY) stops loudly instead
  of coming back headless with `ref=null` (the reinstall-under-running-stub hang above).
- d7fd76fb proxy: DNS port ownership is a 64K-bit bitmap (removed the 256-socket table cap); also fixed
  tests/dns-translation's mock (aborted on fstat since d9bd7c01, pre-existing). DNS host tests PASS.
- netd `-EIO` explained, **not our bug**: `resolv: send_dg 0 terrno: 62` = upstream rejected the query;
  `dumpsys dnsresolver`: QEMU slirp DNS 10.0.2.3 → `NOERROR:80 SERVFAIL:48`. Proxy correctly answers
  SERVFAIL. Real networks unaffected. Minecraft launcher still renders.
- Host `tests/run.sh` stops at "MatonOS i386 peer startup" on this host, identically on the baseline.
- 19:15 installer labels ba98b1e **live-verified**: build47 live + rebuilt Settings, real install to a
  throwaway 40 G NVMe → steps read "Write the boot loaders / MatonOS boot image / boot menu settings /
  boot entry / Add MatonOS to the firmware boot menu"; "Installation complete". ba98b1e is on
  `metadata-enc`, not yet in device-tree main. Side note: `matonos-settings://install` VIEW shows an
  "Open with" chooser (InstallAlias + MainActivity both match) when launched by URI; launcher tile unaffected.
  VM back on hdd43.img (installed).
- 19:40 'Open with' chooser fixed (metadata-enc, settings plugin): alias VIEW filter dropped; URI opens MainActivity directly on VM.
- 19:45 (f) first launch with app data cleared (pm clear Klotski): process start → first buffer **5.2 s** (stub 0.55 s to loader, rest = glycin loader spawns). The ~28 s happens only right after install, not on first-run setup; to measure it a fresh store install is needed (≈6 min). Not chased further tonight.
- 20:15 (f) **closed**: fresh store install of Klotski (7 min 50 s) → first launch: process start 20:14:26.9 → loader 27.5 → first frame 31.4 = **4.6 s**. The old ~28 s no longer reproduces on flatpak-pkg-split (likely gone with the RLIMIT_AS / per-window ref fixes).
  Side notes: `pm uninstall flatpak.org.gnome.Klotski` leaves `flatpak.org.gnome.Klotski.Locale` installed (orphan extension package, nothing removes it); right after an outside uninstall the store tile showed "Open" (→ "No launch activity" error) until the next foreground refresh.
- 20:25 **Decision needed: unused Flatpak packages are never removed.** Both store uninstall (SourcePlugins.uninstall → PackageInstaller on the app package only) and `pm uninstall` leave the app's `.Locale` extension and any now-unused runtimes/GL extensions installed. Desktop `flatpak uninstall` removes the app's related refs (Locale) itself; unused runtimes need `--unused` (GNOME Software does that automatically). Proposed: after an uninstall finishes (and on store foreground for outside uninstalls), the store asks the Flatpak source for carrier packages no installed app needs any more (stock Flatpak `--unused` semantics, computed in the subsystem) and uninstalls them. Not implemented — waiting on the user.
- 22:40 store 4449bd1 (software-centre main, pushed): a failed Open (package removed outside the store) keeps the error and reloads the installed list → tile returns to Install. **VM-verified 23:55** (store rebuilt with the shipped b305 key): pm uninstall under the open store → tap Open → error shown, tile back to Install within ~20 s. Device-tree submodule pointer not bumped (rule).
- 23:30 **App signing keys were split across machines.** Canonical (= what images ship) is build-server `~/.matonos-build/keys-old/`. Host `~/.config/matonos-keys` matched it except `matonos-software` (host had a copy of the old flathub key 8e25…, images ship b305…) and lacked pointer-test; server `~/.config/matonos-keys` had only a stray software key generated 11-20 (bb90…, never shipped). Fixed: both machines' `~/.config/matonos-keys/matonos-software.*` = shipped b305 key (+ pointer-test); old copies kept in host `~/.config/matonos-keys-replaced-2026-11-21/` and server `~/.config/matonos-keys-stray-2026-11-20/`. 7a915fe makes a missing key a build error (MATON_CREATE_APP_KEYS=1 for new apps); its message blames the stray server key for build47 — wrong, build47 used keys-old. Server `prebuilt/apps-built/Software.apk` currently = 8e25 build from 22:35 → replaced at 23:35 with the b305 build of 4449bd1 (prebuilt + buildinfra).
  **Committed `systembridge/res/raw/caller_cert_allowlist.txt` is stale** (store 8e25…, no pointer-test); the server's uncommitted copy (store b305…, pointer-test 7cb2…) is what images use. Not committed (standing rule) — needs your OK to commit the server version.
  Also seen: after a store install the tile flips to Open only ~1 min after the package lands — cause: every package change re-fetched AppStream + Flathub API. FIXED subsystem 0d3d48ba (flatpak-pkg-split): rebuild ~9 s, VM-verified; snapshot queries still serialise on the catalogue lock. Klotski currently uninstalled on the VM.
