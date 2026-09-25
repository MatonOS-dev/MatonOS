# Standalone HAL source pins

The copied sources are build inputs owned by this tree. Builds do not read
AOSP libraries, intermediate outputs, or source directories. ../SHA256SUMS
records every copied source/header/AIDL file and is verified before codegen.
The frozen AIDL .hash files are preserved and passed to the NDK generator.
Updating a snapshot requires an intentional review and regenerated checksums.

| Snapshot | Upstream project | Pinned revision | License |
|---|---|---|---|
| hardware_interfaces/ | hardware/interfaces | 0162af698935100a590b7359581ac8b1b80693e5 | Apache-2.0; source headers retained |
| frameworks_av/ | frameworks/av | 475269ec44acec82792be6b01fb8e16357d4d2c8 | Apache-2.0; source headers retained |
| frameworks_native/ | frameworks/native | ae266dcb706d083868578cfedce381ef44488a07 | Apache-2.0; source headers retained, including Binder NDK public/platform headers |
| hardware_libhardware/ | hardware/libhardware | 97ee3e33ec5ba48c0eca8341ed377ac4497a0678 | Apache-2.0; public audio hardware headers |
| system_core/ | system/core | 545d2487e38192a2ce25040897ced877cf6b4f53 | Apache-2.0; source headers retained |
| system_fmq/ | system/libfmq | d525961b46fbebd138018a102e4ab983c9582045 | Apache-2.0; source headers retained |
| system_libbase/ | system/libbase | fef4173e19d97cd4b4fc09a06bccbf800d0d3937 | Apache-2.0; source headers retained |
| system_logging/ | system/logging | d35ba620451b28512f46ebb5421f783fd77968d5 | Apache-2.0; liblog headers |
| system_media/ | system/media | 440eb272fd3da0ca7bdae932e61e7fd9be1fd324 | Apache-2.0; source headers retained |
| fmtlib/ | external/fmtlib | e3bef0ad2e9d4ea38ba3b4b2b59e0514f7025fed | BSD-2-Clause; upstream license retained |
| xsdc/ | system/tools/xsdc | f1f0c793dc404e716ac73beb1a8bef9cef97a40d | Apache-2.0; source headers retained |

Frozen AIDL package versions and provenance are in stable-aidl.list and
src/aosp/AIDL-PROVENANCE.md. The narrow static source closure built by CMake is
listed in `../static-dependencies.txt`; CMake compiles those sources into
local static archives. The standalone executable is staged only after all
expected archives exist and its dynamic dependencies pass the NDK allowlist
audit.

Three copied-source adaptations are explicit: libbase `file.cpp` replaces
C++23-only `resize_and_overwrite` with equivalent C++20 append/read logic;
FMQ error logging uses stable liblog logging instead of `android_errorWriteLog`;
HAL stream tracing is stubbed off, spatializer property lookup is removed, and
the copied SoundDose implementation is a no-op. The HAL does not expose
spatializer/offload/effect processing, so these paths are intentionally absent.
All edits remain under the original license headers and are covered by
`../SHA256SUMS`.
