# Handover — morning 2026-11-16 (store night)

Living doc; updated through the night.

## Done
- Image 7.3p built (248 min), x86-64-v2 ISA audit PASS.
- Device tree 2de9f50: `store-sources/fdroid` is a submodule
  (MatonOS-dev/store-source-fdroid); store `rn-apps/flathub` → bb32bc9,
  contract → c21e8d9 (REVISION 1, `running()` keep-alive).
- Store main bb32bc9 (fast-forward of contract-v2): ownership record, IStore
  update service, cancel, per-app parallel operations, keep-alive polling,
  "<label> stopped" / "source unavailable".
- F-Droid source: catalogue caching, conditional entry.jar, parallel repos,
  running(); two VM-found fixes pushed:
  - entry.jar verified with JarFile (F-Droid puts entry.json before the
    manifest; JarInputStream reported every real repo as unsigned).
  - HTTP validators stored only after a verified index exists (a failed load
    left an ETag → permanent "304 without cached index").

## VM store smoke test (disk VM, headless, port 5556)
- Store launches, Flathub browse + search work.
- Sources page renders; F-Droid listed as Enabled.
- F-Droid search works (f-droid.org index streamed, a93c1b9). Results were
  appended after every Flathub hit and labelled with the plugin package name;
  store now interleaves sources round-robin and shows the label.
- Open: F-Droid ranking — "newpipe" ranks BendyStraw above NewPipe (exact
  name match should come first; plugin-side).
- Still to test: install, cancel, two parallel installs, uninstall, keep-alive.

## Running
- flatpak-source4 (sol): Flatpak source on contract c21e8d9 + launch repair via IStore.update + ISA fix.

## Decisions taken
- (none yet)

## Parked
- (none yet)
