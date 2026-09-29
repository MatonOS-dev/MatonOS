# Shared changes requested

No shared device-tree files need changes for the current bwrap/libseccomp
compile. Run `linux/third_party/apply-patches.sh` before the module-only
request; it is already applied to the pinned source and is idempotent.

The most recent unrelated full image build failed in
`install/linuxd/aidl/org/matonos/systembridge/ILinuxd.aidl`: AIDL expected
`./out/org/matonos/systembridge/ILinuxd.cpp` but generated
`./out/aidl/org/matonos/systembridge/ILinuxd.cpp`. The linuxd owner must fix
its AIDL output path for a full image build. This spike only needs the
module-only targets for the next step.
