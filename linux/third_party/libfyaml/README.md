# libfyaml

Pinned upstream 0.9.6 release archive:

- URL: `https://github.com/pantoniou/libfyaml/releases/download/v0.9.6/libfyaml-0.9.6.tar.gz`
- SHA-256: `a59cc3331e2eb903ec36933ad52a45888041cac31e44f553a00511131242c483`
- Extracted source: `upstream/`
- NDK scratch build: `out/matonos/flatpak-ndk/libfyaml/`

AppStream 1.2.0 requires libfyaml >= 0.8. Build with NDK r30 for x86_64 Android API 35; omit tests and docs. Portability fixes belong in `patches/` over pristine upstream.

NDK portability patch: `patches/0001-bionic-no-libpthread.patch` avoids linking `-lpthread` for Android because bionic exposes pthread symbols from libc. It passed apply-check against pristine upstream and the patched NDK build/install succeeded.
