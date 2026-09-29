# microG and app installation

MatonOS is based on AOSP. This area installs the hash-pinned, upstream microG GmsCore and Companion APKs as privileged product apps. `gms.lock` records their versions, APK SHA-256 values and actual signer digest; `fetch-gms.sh` verifies each download. GsfProxy is omitted because the current bundle does not need its legacy C2DM role.

Neo Store replaces F-Droid Basic and the removed Privileged Extension. It is a pinned privileged app with `INSTALL_PACKAGES` and `DELETE_PACKAGES`, so the PackageInstaller session path can install apps without a system confirmation. Neo Store's own `installer_type` preference defaults to `session`; users can select **Settings → Installer → System** after first launch.

## Signature compatibility

Patch 0002 contains the PackageManager hook and policy-call diagnostics. The hook supports both `GET_SIGNATURES` and `GET_SIGNING_CERTIFICATES`, and signature comparisons use the SystemBridge provider's exact package/certificate allowlist. The hook now requires the caller to hold `android.permission.FAKE_PACKAGE_SIGNATURE`. Bridge failure and metadata/policy mismatch fail closed. Patch 0003 keeps the pinned microG-to-Google update signer exception separate from spoofing.

The permission belongs to the platform `android` package. Declaring an `android.*` permission from the `org.matonos.systembridge` APK did not register it and the image omitted SystemBridge at runtime. The permission definition is therefore in the existing signature-spoofing patch's `framework-res` manifest hunk. GmsCore is listed in both the privileged permission allowlist and the default runtime permission grants. The Android platform permission policy disallows OEM apps from defining permissions in the `android` namespace; see the [AOSP permission policy test](https://android.googlesource.com/platform/cts/+/c72d7f99ecdef7c61117ad61967548ffb552dd02/tests/tests/permission2/src/android/permission2/cts/PermissionPolicyTest.java).

## Build and verify

The coordinator runs `gms/fetch-gms.sh`, applies the patches, and builds the image. `tools/preflight.sh` passed before the latest request. The 08:17 image built successfully, but a fresh QEMU run showed the permission was still missing (`DefaultPermGrantPolicy: Permission not found`) and `pm path org.matonos.systembridge` returned no package. The cause was the attempted OEM permission definition in the bridge manifest; that declaration has been removed and moved to `framework-res`. A corrected full-image build and fresh-boot verification are pending.

On a fresh image, verify in this order:

1. `pm list permissions | grep FAKE_PACKAGE_SIGNATURE` lists the permission.
2. `pm path org.matonos.systembridge` and `pm path com.google.android.gms` both return APK paths. `dumpsys package com.google.android.gms` reports `FAKE_PACKAGE_SIGNATURE` granted.
3. Open GmsCore → Self-Check. Confirm the signature-spoofing permission, system spoof, GmsCore signature and Companion signature rows are green. Capture a screenshot and save `logcat -d` lines tagged `PackageManager` and `MatonGmsSpoof` to confirm the hook flags, permission state, metadata and bridge decision.
4. Install a new app from Neo Store and verify no system confirmation dialog appears.
5. Attempt to update a system app with a different-signed APK and verify PackageManager rejects it.

The 05:56 image previously booted and Neo Store silently installed Fossify File Manager; evidence is `/mnt/data/aosp/out/pc-logs/gms/neo-install-20260928.png`. On that image, microG Self-Check failed signature checks (`microg-selfcheck-final-20260928.png` and `.xml`). The 08:17 image reached `sys.boot_completed=1`, but had no SystemBridge package and no signature-spoof permission. These prior results do not verify the corrected build.

## Real Play / GMS transition

Google APKs are not included. On a disposable device, obtain genuine x86_64 Play Store and Play Services APKs and confirm they use one of the pinned Google signing certificates. Sideload each and verify it updates the matching microG system app while retaining system and privileged status. Confirm the bridge no longer spoofs a replaced package. This path remains untested.

## Real PC steps

Test the fresh image on the Ryzen 5800X / RX 6600 / Intel 7265 PC, HP ProDesk 600 G1 and Surface Pro 3. Verify Self-Check, silent Neo Store installs, rejection of a mismatched system-app update and package paths. Use a disposable install for Google APK transition testing.

## Open issues

- The corrected framework permission definition, grant and signature spoof behavior still need a successful fresh-boot check.
- Neo Store's private installer preference remains `session` until the user selects `System`.
- Neo Store self-update retention is untested because no newer upstream release is available.
- Google update transitions and mismatched system-app update rejection remain untested.
