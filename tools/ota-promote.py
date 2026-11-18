#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path


def fail(message):
    print(f"ota-promote: ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)


def main():
    if len(sys.argv) != 4:
        fail("usage: tools/ota-promote.sh <out-dir> <channel> <update_id>")
    out = Path(sys.argv[1])
    channel, update_id = sys.argv[2:]
    if not re.fullmatch(r"[a-z0-9_]+", channel):
        fail("channel must contain only lowercase letters, digits, and underscores")
    if not re.fullmatch(r"[1-9][0-9]*", update_id):
        fail("update_id must be a positive integer build id")
    build_dir = out / "updates" / update_id
    manifest_path = build_dir / "manifest.json"
    if not build_dir.is_dir() or not manifest_path.is_file():
        fail(f"build does not exist: {build_dir}")
    try:
        manifest = json.loads(manifest_path.read_text())
        build = manifest["build"]
        if int(build["id"]) != int(update_id) or str(build["version"]) == "":
            fail("build manifest metadata does not match update_id or has no version")
        version = str(build["version"])
    except (OSError, ValueError, TypeError, KeyError) as error:
        fail(f"cannot read build manifest metadata: {error}")

    pointer = out / f"updates/{channel}-manifest.json"
    if pointer.exists():
        try:
            current = json.loads(pointer.read_text())
            current_id = int(current["update_id"])
            if current_id <= 0:
                fail(f"current channel pointer has invalid update_id: {pointer}")
        except (OSError, ValueError, TypeError, KeyError) as error:
            fail(f"cannot read current channel pointer {pointer}: {error}")
        if int(update_id) < current_id:
            fail(f"refusing downgrade of channel {channel}: current {current_id}, requested {update_id}")

    pointer.parent.mkdir(parents=True, exist_ok=True)
    staging = pointer.with_name(f".{pointer.name}.tmp")
    staging.write_text(json.dumps({"format": 1, "update_id": int(update_id), "version": version},
                                  separators=(",", ":")) + "\n")
    staging.replace(pointer)
    pointer.with_suffix(pointer.suffix + ".hwfm").write_text(json.dumps({
        "Cache-Control": "public, max-age=60",
        "CDN-Cache-Control": "public, max-age=60"}, separators=(",", ":")) + "\n")
    print(f"Promoted {update_id} to {channel}: {pointer}")


if __name__ == "__main__":
    main()
