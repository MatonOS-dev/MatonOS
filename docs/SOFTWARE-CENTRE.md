# Software Center

The Software Center is the shipped Expo app in `rn-apps/flathub`. It presents
two Sources in one catalogue: F-Droid APKs and Flathub Flatpaks. Search results
carry a source badge; Installed, Updates, and operation progress are shared.
There is no separate software service app and no `linuxd` dependency in the
F-Droid path.

## Sources

### F-Droid

The built-in `https://f-droid.org/repo` repository is enabled by default and
cannot be removed. The client is original Java code based on F-Droid's
published index format. It verifies the `entry.jar` signature against the
official signing fingerprint documented by
[F-Droid](https://f-droid.org/docs/Release_Channels_and_Signing_Keys/), then
checks the SHA-256 of `index-v2.json` against the signed `entry.json`. Index
caches are private to the app and keyed by repository URL and signer.

Users can add repositories from the Repositories screen or open a
`fdroidrepos://<host>/<path>?fingerprint=<sha256>` link. A repository can only
be trusted after entering a 64-digit SHA-256 signer fingerprint and reviewing
the host and fingerprint. Internationalized hostnames are displayed in
punycode. The index is verified immediately after trust; a repository whose
verification or download fails is marked broken and its apps are not silently
accepted as trusted content. Removing a repository removes its apps from
catalogue search and update checks; already-installed packages remain
installed.

Downloads are checked against the signed index hash before they are passed to
PackageInstaller. Multiple APKs are installed in one session, with update
ownership requested where the platform supports it. Uninstall also uses
PackageInstaller. PackageManager reconciliation lists installed packages that
appear in an enabled repository. Updates require a higher `versionCode` and
an exact match between the installed signing certificate set and the index's
APK signer set. WorkManager checks enabled repositories in the background;
opening Updates also refreshes the list. Updates to centre-owned packages use
the install path and update ownership.

### Flathub

`FlathubSource` wraps the app's existing Flathub catalogue and Flatpak bridge
operations. Flatpak search and install behavior remains in the app's existing
bridge integration. Its results participate in the same browse, Installed,
Updates, source badges, and progress UI.

## Verification

The native host tests are run with:

```sh
rn-apps/flathub/modules/matonos-flathub/android/test-host.sh
```

They cover signed and damaged JARs, a wrong repository signer, an index hash
mismatch, catalogue parsing, APK hash rejection before session commit, update
signer/version checks, required repository fingerprints, punycode host display,
and removal filtering. The app checks are:

```sh
cd rn-apps/flathub
npm run typecheck
npm run lint
cd ../..
MATON_AOSP_ROOT=/mnt/data/aosp MATON_APPS_ONLY=matonos-flathub tools/build-apps.sh
```

On a freshly built image, verify the built-in repository, search across both
Sources, install and uninstall an F-Droid app, inspect download progress,
install an index entry with split APKs, and confirm Installed reconciliation.
Add a test repository using a fingerprint from its operator, verify that its
host and fingerprint appear in the trust screen, then remove it and confirm
its results disappear while its installed package remains. Also test a bad
fingerprint and an unreachable repository; both should appear as broken and
must not produce accepted app results. On a network-connected PC, leave the
Software Center installed through a WorkManager interval and confirm that
updates appear only for matching signers and higher versions.

The APK Source does not provide repository key rotation or revocation UI,
anti-feature labels, permission-risk comparisons, or rollback retention. Those
remain future work.
