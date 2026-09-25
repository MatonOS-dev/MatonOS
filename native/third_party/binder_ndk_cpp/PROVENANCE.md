# Binder NDK C++ header snapshot

- Upstream: AOSP `frameworks/native/libs/binder/ndk/include_cpp/android/`
- Revision: `ae266dcb706d083868578cfedce381ef44488a07` (2026-04-23, 26Q2-release)
- License: Apache License 2.0; each upstream header carries its license notice.
- Purpose: header-only C++ RAII/interface helpers paired with the NDK r30
  `libbinder_ndk` C API headers and API 35 target sysroot. These headers add no
  native ABI symbols; they are inline/template wrappers. Service-manager
  declarations are platform-only and are provided by the matching AOSP
  `include_platform` header; the daemon links the matching device-branch
  vendor `libbinder_ndk.so` for those API 29+ service-manager exports. Other
  Binder NDK ABI calls are available at API 35 in the NDK sysroot.

Refresh only from this pinned AOSP revision, preserving the Apache headers.
