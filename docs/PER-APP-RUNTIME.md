# Per-app runtime (v3, implemented shape)

Each generated stub owns PerAppRuntime → NativeCompositor, Xwayland, SessionBus,
JavaPortal, DNS forwarding and V3Launcher in its own process and UID.
StubService retains the per-app runtime; StubActivity receives window, inhibit
and open-URI callbacks directly in Java. There is no central coordinator or
shared session service. V3Launcher constructs the Flatpak command from the stub
manifest ref, using existing installation paths and read permissions.

PortalFiles is declared in generated version-15 stubs with a per-package
authority; its descriptor registry stays with the portal. The host package is
only a shared library plus its own demo/support UI, under stock priv_app policy.
Install/update operations retain the authenticated bridge/linuxd path.

See [v3 launch](../linux/compositor/V3-LAUNCH.md) for details and pending enforcing
image validation. The old staged service-split RFC is superseded.
