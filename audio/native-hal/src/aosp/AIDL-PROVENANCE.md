# Frozen AIDL API provenance

Frozen NDK snapshots are copied from AOSP `hardware/interfaces` revision
`0162af698935100a590b7359581ac8b1b80693e5` and `system/hardware/interfaces`
revision `9c3469aa3d4aeec5b2bc8e8ae8fdb4437dab4edb`; their original `.hash`
files are preserved in `../../aidl-snapshots/`. The standalone generator must
use these snapshots and their hashes, never the mutable `current/` API. Audio
core is v4; exact imported package versions are recorded in
`../../stable-aidl.list`.
