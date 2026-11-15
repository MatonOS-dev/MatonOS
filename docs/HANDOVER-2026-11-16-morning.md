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
- Verified on VM: F-Droid install (BendyStraw, LocalTube, Organic Maps),
  uninstall, Flathub + F-Droid installs running in parallel with separate
  progress banners.
- Fixed: Cancel was shown for the built-in Flathub source, which has no
  cancel (dead button). Now shown only when the source implements cancel;
  the Flatpak plugin (round 4) brings real cancel.
- Not verified on VM: F-Droid cancel — downloads finish in < 10 s on the VM
  link (tc only shapes egress). Host tests cover it.
- Follow-ups (small, store):
  - Banners above the list shift every row when an operation starts/ends;
    taps land on the wrong app. Put progress in the row or a fixed-height area.
  - Package removed outside the store (pm uninstall / Settings) keeps showing
  "Uninstall" until the store restarts; listen for package changes.
  - Search submitted while the first collection is loading stays on
    "Loading apps…" until resubmitted.

## Running
- flatpak-source4 (sol): Flatpak source on contract c21e8d9 + launch repair via IStore.update + ISA fix.

## Decisions taken
- (none yet)

## Parked
- (none yet)
