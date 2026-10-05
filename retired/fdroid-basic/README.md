# F-Droid Basic

MatonOS builds the upstream F-Droid Client `basic` flavor from F-Droid
Client 2.0.0, pinned to commit
`30f467b2c6b8f661191a70ae004af933dad5b5f0`. The source tarball is checked
against a SHA-256 before use. The source patch adds `INSTALL_PACKAGES` and
`DELETE_PACKAGES` to the app manifest and adds the enabled MatonOS repo to the
client's bundled `default_repos.json`; it is kept in
`0001-privileged-install.patch`. The repo certificate is embedded as DER and
matches SHA-256 fingerprint
`61B4BEA4330F25886C2271E8821F5D93A2A9A981BEC37F155C1C8629F3BE011F`. The resulting `org.fdroid.basic` APK is
signed with MatonOS's persistent per-app development key and imported as a
privileged product app. Its allowlist grants only those two permissions.

F-Droid Basic's installer uses PackageInstaller sessions and requests
`USER_ACTION_NOT_REQUIRED`. With the platform-granted `INSTALL_PACKAGES`
permission, the session can install new apps and updates without launching
the system confirmation UI. No private API or AOSP patch is involved. The
existing `install-unknown-apps` setup grant is absent, and is unnecessary for
this privileged installer path.

## Build

Run after the shared image build has finished (never during a coordinator
build):

```sh
tools/build-apps.sh
apps/fdroid-basic/build.sh
tools/fetch-apps.sh
tools/preflight.sh
```

`build.sh` downloads only the pinned upstream source into temporary storage,
checks and applies the local manifest patch, builds
`:app:assembleBasicDefaultRelease`, aligns and signs the APK using
`~/.config/matonos-keys/fdroid-basic.jks`, and stages the ignored
`FdroidBasic.apk` next to this README. The key/password persist across builds
and must be backed up securely. The APK is a build output, not a source file.

The script uses the same local Android SDK, JDK 21, 4-worker maximum and
per-app key directory conventions as `tools/build-apps.sh`. Build this APK
before requesting an image build so Soong can find the staged import.

## Verification

The build script checks the APK signature, package ID and presence of both
manifest permissions. `tools/fetch-apps.sh` verifies the unchanged Fennec,
Fossify and Open Camera APK pins and cleans only the downloaded-APK directory.
`MATON_BUILD_COORDINATOR=1 bash tools/preflight.sh` checks app packaging and
the fixed SELinux file set.

On a fresh QEMU boot, confirm:

1. `pm path org.fdroid.basic` resolves under `/product/priv-app/` and
   `dumpsys package org.fdroid.basic` shows both install/delete permissions
   granted.
2. F-Droid Basic refreshes the official `https://f-droid.org/repo` catalog.
3. Install a new small app from that repo. It should complete without a
   system package-install confirmation dialog.
4. Update a preinstalled F-Droid app (for example, Fossify Calendar)
   from its official F-Droid listing. Confirm the installed version changes
   and no system confirmation dialog appears.
5. `pm list packages` contains `org.fdroid.basic` and no obsolete store
   package from the removed preinstall remains.

On real hardware, repeat steps 2–5 on the Ryzen 5800X/RX 6600/Intel 7265 PC,
Surface Pro 3 and HP ProDesk 600 G1. Use a new app not already on the image
for the silent-install check, then update one preinstalled app from the same
F-Droid repo.

## Status and open items

Verified 2026-09-28 on a fresh QEMU boot from
`out/target/product/pc_x86_64/matonos-live-x86_64.img` (ADB port 5558):

- `sys.boot_completed=1`; `pm path org.fdroid.basic` resolved to
  `/product/priv-app/MatonFdroidBasic/MatonFdroidBasic.apk`.
- `dumpsys package org.fdroid.basic` showed `INSTALL_PACKAGES` and
  `DELETE_PACKAGES` granted. The app's official F-Droid repo loaded.
- F-Droid Basic installed `org.fossify.filemanager`; PackageManager logged
  installation completion under `/data/app/`. F-Droid Basic remained the top
  resumed activity while its UI showed “Preparing installation...”; no system
  installer confirmation activity appeared.
- It updated preinstalled Fossify Calendar from version code 20 (`1.10.3`) to
  22 (`1.11.0`). PackageManager updated it from `/product/app/` into
  `/data/app/` and retained its data.
- `pm list packages` showed `org.fdroid.basic` and not the removed
  `com.machiav3lli.fdroid` package.
- `tools/check-selinux-labels.sh` reported all nine init-started vendor
  programs labeled, with no failures. Fetch-apps and preflight passed.

QEMU screenshots and serial output are in `/mnt/data/aosp/out/pc-logs/apps/`.
Real-hardware install/update checks on the Ryzen 5800X/RX 6600/Intel 7265 PC,
Surface Pro 3 and HP ProDesk 600 G1 remain to be done. A clean checkout must
run `apps/fdroid-basic/build.sh` before its image build because the APK is a
local generated output and is intentionally not checked into git. F-Droid may
later change its flavor names or signing/build setup; keep the source commit,
archive checksum and expected output/package assertions pinned until
deliberately updating them.

## Package reservation placeholders

`apps/placeholders.list` pins the incoming signing-lineage root for each
package. `apps/build-placeholders.sh` builds a manifest-only APK per row
(versionCode 1, no activity, icon, or code), using a persistent MatonOS key
per package. The APKs are staged under `apps/placeholders/` and imported as
privileged apps. Aurora retains its existing privapp allowlist. The two
allowed signer transitions are hard-coded in
`patches/frameworks/base/0003-pinned-gms-update.patch`: Aurora preload and
Google's Play-signed YouTube can replace their own placeholder only.

Current list: `com.aurora.store` uses Aurora's pinned root
`4C626157AD02BDA3401A7263555F68A79663FC3E13A4D4369A12570941AA280F`;
`com.google.android.youtube` uses the Google-signed YouTube cert root
`3D7A1223019AA39D9EA0E3436AB7C0896BFB4FB679F4DE5FE7C23F326C8F994A`.
The placeholder stand-in certificate digests are generated with the private
per-package keys and recorded as lowercase values in the transition table.
Keep these keys backed up; rebuilding with different keys invalidates the
transitions until the table is updated.

Build these APKs after any source/list change and before the image build:

```sh
bash apps/fdroid-basic/build.sh
bash apps/build-placeholders.sh
```

On QEMU, also confirm the enabled MatonOS repo at
`https://download.hanro50.net.za/matonos/fdroid/repo` appears and refreshes.
After boot, install the Aurora preload APK with `adb install`; it should replace
the placeholder and remain under a privileged app path. Search for YouTube in
Aurora and install an offered compatible update over the versionCode-1
placeholder. The default x86_64 PC profile currently reports no known YouTube
versions, so this last step requires a compatible Google Play build to be
available for the device profile. A different APK signed with an unrelated key
but using either package name must fail with an update signature mismatch. The
placeholders have no launcher entry; check with `cmd package
query-activities` for `MAIN`/`LAUNCHER`.

Fresh-image verification on 2026-09-28 used the coordinator's 16:52 full image
in QEMU on ADB port 5556. The serial log and screenshots are under
`/mnt/data/aosp/out/pc-logs/apps/`:

- `com.aurora.store` and `com.google.android.youtube` resolved from
  `/product/priv-app/MatonAuroraPlaceholder/` and
  `/product/priv-app/MatonYoutubePlaceholder/`; both were versionCode 1 and
  `PRIVILEGED PRODUCT`. PackageManager found no `MAIN`/`LAUNCHER` activity for
  either package.
- The MatonOS repository appeared enabled in F-Droid Basic's Repositories
  screen (`fdroid-box-v3-final.png`).
- `adb install -r AuroraStore-preload-4.8.4.apk` succeeded over its placeholder.
  The resulting versionCode 76 app remained `PRIVILEGED PRODUCT`; PackageManager
  still listed `INSTALL_PACKAGES` among its requested permissions.
- Wrong-key APKs using both reserved package names were rejected with
  `INSTALL_FAILED_UPDATE_INCOMPATIBLE` (the Aurora attempt used `adb install
  -r -d` because the real preload had already replaced the placeholder).
- Aurora searched YouTube and displayed Google's listing, but with the default
  MatonOS x86_64 PC profile its Manual download version picker reported “No
  known versions found.” Thus this VM could not download or install a real
  YouTube APK, nor compare that APK's lineage root. The pinned YouTube root
  remains `3D7A1223019AA39D9EA0E3436AB7C0896BFB4FB679F4DE5FE7C23F326C8F994A`;
  no conflicting APK evidence was obtained, so `placeholders.list` and patch
  0003 were not changed and no READY swap request was needed.

Real-hardware install/update checks on the Ryzen 5800X/RX 6600/Intel 7265 PC,
Surface Pro 3 and HP ProDesk 600 G1 remain to be done. A clean checkout must
run `apps/fdroid-basic/build.sh` before its image build because the APK is a
local generated output and is intentionally not checked into git. F-Droid may
later change its flavor names or signing/build setup; keep the source commit,
archive checksum and expected output/package assertions pinned until
deliberately updating them.
