# Conditional shared change

No split is requested while the `android:multiArch="true"` host APK test is
pending. The current host APK contains JNI libraries and declares the dynamic
library `org.matonos.linuxhost`; its rebuilt manifest now enables multiarch.
The next image build and QEMU install test will determine whether stock
PackageManager accepts it.

If PackageManager still rejects the provider with
`Shared library with native libs must be multiarch`, the stock-compatible
fallback is a Java-only `MatonWaylandHost` provider package
`org.matonos.compositor`, plus a JNI-bearing engine package
`org.matonos.compositor.engine`. That fallback would require shared registry
changes: add the engine Gradle project to `buildinfra/apps/apps.list`, import
`MatonWaylandEngine` from `buildinfra/Android.bp` with
`system_ext_specific: true` and `preprocessed: true`, and add it to
`PRODUCT_PACKAGES` in `buildinfra/buildinfra.mk`. Move the JNI/native service
into that engine package and connect it to the Java provider with an explicit
bound service. Do not make these changes unless the multiarch attempt fails.

The system bridge and Flathub store still need a narrow trusted-call surface
to invoke `MatonLinuxStubGenerator`, plus policy defining which signed callers
may mint stubs. Those changes remain out of scope for this task.
