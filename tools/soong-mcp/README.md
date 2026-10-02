# MatonOS Soong MCP

Local stdio MCP server, requiring Python 3.11+ and no extra packages. It wraps
the existing coordinator, not Soong internals. All state lives under
`out/pc-logs/soong-mcp/`; no changes to AOSP or the currently running build.

## Connect

Generic MCP client configuration (client working directory must be the
AOSP checkout root; otherwise resolve the script path for your checkout):

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

The AOSP root is inferred from the server location; `--root /path/to/aosp`
overrides it. The coordinator must exist at
`out/pc-logs/agents/coord-build.sh`. This is deliberately a local build-host
adapter, not a distributable build service. The server is implemented for
MCP revisions 2024-11-05, 2025-06-18 and 2025-11-25, using the legacy
initialize handshake and newline-delimited JSON-RPC on stdio. It does not
implement the 2026 stateless protocol.

External image discovery defaults to `~/matonos-images/`. Set
`MATON_IMAGES_DIR` to use another directory. No host-specific checkout or
home directory is embedded in the server.

## Tools

| Tool | Purpose |
| --- | --- |
| `build_status` | Active processes, shared lock, status files, recent progress |
| `build_errors` | Siso error output and failed commands with timestamps |
| `build_log` | Bounded tail of repair, coordinator, managed or error log |
| `build_artifacts` | Image and staged APK paths, sizes and timestamps |
| `build_start` | Start coordinator with a chosen job count (default 16) |
| `build_stop` | Terminate only the MCP-managed build process group |
| `build_resume` | Restart managed build incrementally, optionally changing jobs |

Build control runs the full coordinator with `-K -M`: kernel and Mesa are
skipped; app staging, native staging, Soong, partitions and live packaging
follow the existing coordinator. Resume retains outputs and reruns the
same path; it is not an in-memory checkpoint. It never cleans or publishes.
The shared `out/.maton-build.lock` is held from staging through packaging;
`MATON_BUILD_LOCK_HELD=1` delegates that ownership to the coordinator's
build script. Existing unmanaged builds are observable but cannot be stopped
or resumed through these tools. Stale error logs are identified by timestamp.
The coordinator's existing VM and agent handling still applies.

Workers survive MCP client disconnects. Stop requests signal the whole
managed session and escalate to SIGKILL after ten seconds if necessary.
PID start time is checked before signalling to avoid signalling a reused PID.

Status reports raw progress without estimating completion from scheduling
counts. Artifact timestamps do not establish that a particular fix was
included, or that an image was booted successfully. The standard coordinator
packages its standard image path; the separate repair-r3 driver retains its
own versioned image destination.

Protocol reference: https://modelcontextprotocol.io/specification/2025-11-25

## Validation status

Not tested yet. No MCP build has been launched during the active repair build.
