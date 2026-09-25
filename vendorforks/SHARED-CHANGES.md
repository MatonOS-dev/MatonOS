# Shared changes requested

- `BoardConfig.mk`, comment under `# minigbm is BlissOS...`: replace the two
  comment lines attributing the gbm_mesa allocator/mapper changes to
  `patches/external/minigbm` and instructing builders to run
  `tools/apply-patches.sh` with: “The `matonos/v1.2` fork branch in
  `manifest/maton.xml` selects gbm_mesa for the AIDL allocator and stable-C
  mapper.” Reason: the AOSP patch is retired; the manifest pins the local
  fork branch.
- `tools/apply-patches.sh`, introductory comment line 5: replace the old
  minigbm-specific example with a neutral example such as
  `patches/<project path>/NNNN-fix.patch applies to <aosp>/<project path>`.
  Reason: no minigbm patch is used after this fork switch.
