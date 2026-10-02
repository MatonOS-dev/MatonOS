# MCP servers for MatonOS development

This file lives in `device/maton/pc_x86_64/` within an AOSP checkout.
Paths below are relative to the checkout root unless stated otherwise.
Read this directory's `NOTES.md` and `CLAUDE.md` before
implementation work. This file documents MCP capabilities and setup status;
it does not authorize installing servers, stopping user VMs or publishing.

## Local Soong/build MCP

Implementation:
`device/maton/pc_x86_64/tools/soong-mcp/server.py`

Documentation:
`device/maton/pc_x86_64/tools/soong-mcp/README.md`

**Status:** implemented, not tested or connected to an MCP client as of
2026-10-01. Do not assume its tools are available in a session merely because
the source exists. Requires Python 3.11+, with no external dependencies.

Start over stdio:

```sh
python3 device/maton/pc_x86_64/tools/soong-mcp/server.py
```

Generic client configuration (set the client working directory to the
checkout root, or resolve the script path to your checkout location):

```json
{
  "mcpServers": {
    "matonos-soong": {
      "command": "python3",
      "args": ["device/maton/pc_x86_64/tools/soong-mcp/server.py"]
    }
  }
}
```

| Tool | Purpose |
| --- | --- |
| `build_status` | Build processes, shared lock, status files and recent progress |
| `build_errors` | Siso error output and failed commands, with timestamps |
| `build_log` | Bounded log tail; repair, coordinator, managed or Siso errors |
| `build_artifacts` | Image and staged APK paths, sizes and modification times |
| `build_start` | Start the coordinator with a chosen job count; default 16 |
| `build_stop` | Stop only the MCP-managed build process group |
| `build_resume` | Restart a managed build incrementally, optionally changing jobs |

### Build behavior and limits

- Wraps `out/pc-logs/agents/coord-build.sh --full -K -M -j <jobs>`.
  Kernel and Mesa are skipped. App/native staging, Soong, partition images
  and live image packaging use the existing coordinator.
- Holds `out/.maton-build.lock` throughout staging and packaging, passing
  `MATON_BUILD_LOCK_HELD=1` to the build scripts. Refuses competing builds.
- Infers the checkout root from the script location; `--root` overrides it.
  External images default to `~/matonos-images/`; `MATON_IMAGES_DIR`
  overrides that directory.
- Writes managed state/logs under `out/pc-logs/soong-mcp/`. Workers survive
  client disconnects. Stop checks the worker PID's start time and signals
  its process group, escalating after ten seconds if necessary.
- Observes existing external builds, including the repair image driver,
  but cannot stop or resume them. Do not start a competing build to adopt
  an external build into MCP management.
- Resume reruns the coordinator using existing outputs; it is not a
  checkpoint. Does not clean, publish, release or push.
- Scheduling counts are not completed compilation. Compare timestamps
  before treating error files as current. Artifact modification times do
  not prove a fix is included or that an image works after boot.
- Supports the legacy MCP initialize handshake for protocol versions
  2024-11-05, 2025-06-18 and 2025-11-25; not the 2026 stateless protocol.
- The coordinator's existing VM handling and agent handling still apply.
  Standard coordinator packaging uses its standard image destination;
  the separate repair-r3 driver uses its own versioned destination.

## Android Source Explorer MCP (candidate)

Repository: https://github.com/mrmike/android-source-explorer-mcp

**Status:** reviewed as a candidate; not installed or connected by this work.

Provides AOSP framework and AndroidX source navigation:

- `search_classes`, `lookup_class`, `lookup_method`, `list_class_members`.
- `get_class_hierarchy`, `search_in_source`.
- Optional LSP tools: `goto_definition`, `find_references`, `get_type_info`.
  Enable with `ANDROID_SOURCE_LSP=true`; download language servers with
  `android-source-explorer sync --lsp`.

Useful for Binder/framework behavior, Activity lifecycle and Compose
internals. It is a source explorer, not a build controller.

Upstream setup examples (instructions, not actions already performed):

```sh
uv tool install git+https://github.com/mrmike/android-source-explorer-mcp
android-source-explorer sync --api-level 36 --androidx "compose,lifecycle,activity"
android-source-explorer serve
```

Requires Python 3.11+ and Git; upstream recommends uv. Sources are cached
under `~/.android-sources/`: AOSP framework sparse checkouts and AndroidX
source JARs. It also prioritizes available `$ANDROID_HOME` sources.

**Version matching matters:** do not assume API 36 or downloaded latest
sources match this workspace. Use the checked-out source as authoritative
for this build. Confirm indexed revisions before applying conclusions from
this server. Compatibility with our full local AOSP checkout has not been
verified.

## QEMU-MCP (candidate)

Repository: https://github.com/Kevin4562/QEMU-MCP

Package/entrypoint: `mcp-qemu-lab`.

**Status:** reviewed as a candidate; not installed, connected or adapted
to MatonOS by this work.

Designed for Linux binary analysis in QEMU guests. Documented tools include:

- `vm_create`, `vm_start`, `vm_status`, `vm_stop`.
- `vm_snapshot_save`, `vm_snapshot_load`, `vm_logs_tail`.
- `guest_wait_ready`, `guest_exec`, `guest_copy_in`, `guest_copy_out`.
- `process_list`, `process_maps`, debugger attach/breakpoint/continue/
  register/detach tools, `process_dump_core`, `guest_dump_memory`.
- `artifacts_list`; artifact content and index via MCP resources.

Requires Python 3.11+, uv, QEMU/qemu-img and OpenSSH client tools.
Upstream launch example:

```sh
uvx --from git+https://github.com/Kevin4562/QEMU-MCP.git mcp-qemu-lab
```

Prefer a reviewed commit pin when installing. Runtime workspace is set
with `MCP_QEMU_LAB_WORKSPACE`. Guest networking defaults to disabled; guest
SSH tools require user networking. Upstream documents a guest command
allowlist, no host directory sharing by default and audit JSONL entries.

### MatonOS fit

Potentially useful for VM lifecycle, snapshots and native crash analysis.
Its documented guest control uses SSH rather than ADB. It does not directly
provide the Android screenshots, logcat, package state and activity control
needed for our workflow. Existing-VM attachment and compatibility with our
launch script have not been verified.

Before adoption, integrate with:

- `device/maton/pc_x86_64/tools/run-qemu-live.sh` for MatonOS launch settings.
- ADB for Android guest inspection, screenshots and package diagnostics.
- Existing VM ownership rules: preserve the user's VM on port 5555; stop
  agent VMs through `out/pc-logs/agents/vm.sh kill <port>`.
- Image ownership: never overwrite an image attached to a running VM.

Any modifications to these third-party servers belong in a real fork,
following the device tree's fork policy.

## Possible future additions

A small ADB/QEMU adapter around our existing scripts would directly cover
screenshots, logcat, installed packages, activity state and VM status.
A SELinux diagnostics adapter could correlate denials with policy sources.
These are proposals; neither has been implemented in this work.
