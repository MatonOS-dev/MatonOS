# File-backed installer operation contract

This document describes the current Settings installer operation shapes. The
name is retained for links from older installer documentation; this profile
does not create an A/B disk layout. OTA and inactive-slot handling are a
separate task.

## Fresh-install profile

The app generates four GPT partitions: a 2 GiB EFI system partition, a 16 GiB
`maton_system` ext4 backing partition, a 64 MiB ext4 `metadata` partition,
and `userdata` using the remaining aligned space. The app selects F2FS for a
non-rotational target and ext4 for a rotational target. The executor validates
the requested partition references and bounds without embedding layout or
slot policy.

## Operation wire shape

`OperationRequestV1` retains its existing envelope. The app sends `write_gpt`,
filesystem `format`, `populate_images`, and fixed-catalog `write_files`
operations in order. The new `populate_images` operation names both target
PARTUUIDs and lists exactly five allowed live-to-target image pairs:

```json
{
  "apiVersion": 1,
  "targetDiskId": "259:0",
  "targetDiskIdentity": "sys=/sys/devices/...;seq=42;wwid=...;serial=...",
  "operation": {
    "kind": "populate_images",
    "matonSystemPartGuid": "<PARTUUID>",
    "metadataPartGuid": "<PARTUUID>",
    "images": [
      {"name": "system_a", "livePartitionName": "system_a"},
      {"name": "system_ext_a", "livePartitionName": "system_ext_a"},
      {"name": "product_a", "livePartitionName": "product_a"},
      {"name": "vendor_a", "livePartitionName": "vendor_a"},
      {"name": "odm_a", "livePartitionName": "odm_a"}
    ]
  }
}
```

The executor mounts the target filesystems through a sector-zero dm-linear
mapping, then invokes the small `matonos-fiemap-helper` CLI. That helper only
provides `create <name> <size-bytes>`, `map <name>`, `unmap <name>`, and
`list`; it contains no slot policy. Each source image is copied to the mapped
device, flushed and read back before unmapping.

Each request includes a fresh selected-disk identity. The service re-enumerates
before writes and checks the opened target disk sequence. Layout sizes and
partition order belong to Settings; the native service checks GPT boundaries,
identity, safety state, allowed image names and target partition names.

## Current limitations

The app currently submits a destructive fresh-layout sequence only. A distinct
repair flow and downgrade protection are not implemented. There is no slot
choice or OTA behavior in this profile. Conditional userdata mounting and
first-stage access to the file-backed images also require shared platform
integration; see `SHARED-CHANGES.md`.
