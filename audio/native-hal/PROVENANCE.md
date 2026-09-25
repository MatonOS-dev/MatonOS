# Audio HAL source provenance

The HAL implementation snapshot under `src/aosp/` is copied from
`hardware/interfaces/audio/aidl/default` at revision
`0162af698935100a590b7359581ac8b1b80693e5` (Apache-2.0). Its interface
snapshots are the frozen AIDL APIs documented in `stable-aidl.list` and
`src/aosp/AIDL-PROVENANCE.md`. All copied sources and headers are covered by
`SHA256SUMS`; the build verifies it before generating code.

The helper sources copied under `third_party/` are pinned in
`third_party/PINNED-SOURCES.md`. Each upstream revision is listed there.
These files are local source snapshots, not references into the AOSP checkout.
