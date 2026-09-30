# System bridge authorization

Package-scoped bridge calls are attributed to a caller only when
`PackageManager.getPackagesForUid()` returns exactly one installed package.
Binder identifies a process by UID, not by package, so a shared UID cannot
safely be attributed to whichever package happens to have a trust record.
The same rule applies to selected navigation-provider calls and to creating
third-party trust. Package UID and signing certificate are checked again at
authorization time; trust remains keyed by package and current certificate.
Ambiguous UID denials are recorded in the bridge log.

## Verification

`MatonSystemBridgeAuthorizationTests` covers singleton attribution, denial
when a trusted package shares its UID, reevaluation after package removal,
and authorization without a current trust candidate. Run it with:

```sh
atest MatonSystemBridgeAuthorizationTests
```

The change was compiled into a successful full image. The six test methods
also passed in a standalone Java harness. A fresh QEMU boot reached
`sys.boot_completed=1`, and ActivityManager reported
`org.matonos.systembridge/.SystemBridgeService` active. The coordinator's
`droid` image target did not emit the host test module, so `atest` itself was
not run.

The policy test does not install signing-matched shared-UID APK fixtures. A
device-level test should install packages A and B with the same signer and
legacy shared UID, trust A, verify bridge calls from the shared UID are denied,
then uninstall B and verify A succeeds. Repeat after changing A's signer and
after revoking A's trust; both must deny. The denial entry should identify the
ambiguous UID in logcat before B is removed.
