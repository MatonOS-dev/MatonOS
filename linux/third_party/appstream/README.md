# AppStream

Pinned upstream release: AppStream 1.2.0, official freedesktop.org source tarball `AppStream-1.2.0.tar.xz`.

- URL: `https://www.freedesktop.org/software/appstream/releases/AppStream-1.2.0.tar.xz`
- SHA-256: `901919378910550271feb2d826034c9af6522f3ad218de04d3d6d35ddba5cb45`
- Extracted source: `upstream/AppStream-1.2.0/`
- NDK scratch build: `out/matonos/flatpak-ndk/appstream/`

Configure as a library-only Meson build: no GI, docs, stemming, compose, Qt, Vala, or tests. AppStream 1.2.0 needs libxmlb and libfyaml; this tree also pins libyaml for the YAML dependency path requested in the portability spike. No Android.bp is added before the NDK build succeeds.
