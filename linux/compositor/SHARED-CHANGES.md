# Shared changes required

The v1 stub test proved that the current single-APK layout cannot register its
dynamic shared library: stock PackageManager rejects packages that both
declare a dynamic library and contain JNI libraries. Do not patch PackageManager.

Split the current host into:

- A Java-only `MatonWaylandHost` provider APK, package
  `org.matonos.compositor`, declaring the dynamic library
  `org.matonos.linuxhost` and containing the host-facing stub classes and
  launcher UI. Keep its existing `buildinfra/apps/apps.list` row,
  `android_app_import` in `buildinfra/Android.bp`, and product package entry in
  `buildinfra/buildinfra.mk`.
- A JNI-bearing engine APK, package `org.matonos.compositor.engine`, containing
  the current native libraries and compositor service. Add its Gradle project
  to `buildinfra/apps/apps.list`, an `android_app_import` to
  `buildinfra/Android.bp` (`system_ext_specific: true`, `preprocessed: true`),
  and `MatonWaylandEngine` to `PRODUCT_PACKAGES` in `buildinfra/buildinfra.mk`.
  Connect the Java provider to the engine with an explicit bound service.

Reason: AOSP `frameworks/base/services/core/java/com/android/server/pm/PackageAbiHelperImpl.java`
throws `INSTALL_FAILED_INTERNAL_ERROR` for a package recognized as a dynamic
shared-library provider when that package also has native libraries. The
current host APK bundles JNI libraries, so PackageManager does not register
the provider and generated stubs fail with `INSTALL_FAILED_MISSING_SHARED_LIBRARY`.
The source APK is under `/system_ext/app/MatonWaylandHost`, but presence on the
partition does not make it a registered package. The host provider must be
Java-only; the native engine needs its own package. These registry edits are
shared ownership and must be applied by the coordinator after the provider and
engine split is implemented.

The system bridge and Flathub store still need a narrow trusted-call surface
to invoke `MatonLinuxStubGenerator`, plus policy defining which signed callers
may mint stubs. Those changes remain out of scope for this task.
