#!/usr/bin/env python3
"""Fail-closed checks for reusing the already generated AOSP Ninja graph."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: quick_image.py AOSP DEVICE SOONG_NINJA", file=sys.stderr)
        return 2
    aosp, device, ninja = map(Path, sys.argv[1:])
    if not ninja.is_file():
        print(f"ERROR: existing Soong graph is missing: {ninja}", file=sys.stderr)
        return 1
    graph_mtime = ninja.stat().st_mtime_ns
    roots = [device, aosp]
    excluded = {".git", ".repo", "out", ".repo-cache"}
    fresh: list[Path] = []
    for root in roots:
        for current, dirs, names in os.walk(root):
            dirs[:] = [d for d in dirs if d not in excluded and not d.startswith(".git")]
            for name in names:
                path = Path(current, name)
                low = name.lower()
                is_graph_input = name == "Android.bp" or name.endswith(".mk") or low.startswith("product_config")
                if not is_graph_input:
                    continue
                try:
                    if path.stat().st_mtime_ns > graph_mtime:
                        fresh.append(path)
                except FileNotFoundError:
                    continue
    if fresh:
        print("ERROR: configuration input(s) are newer than the Soong graph; run the full build path:", file=sys.stderr)
        for path in sorted(fresh)[:30]:
            print(f"  {path}", file=sys.stderr)
        if len(fresh) > 30:
            print(f"  ... and {len(fresh) - 30} more", file=sys.stderr)
        return 1

    used_path = aosp / "out/soong/soong.environment.used.pc_x86_64.build"
    try:
        used = json.loads(used_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: cannot read the last Soong environment snapshot {used_path}: {exc}", file=sys.stderr)
        return 1
    # PATH/PWD and temporary directories are set by envsetup/lunch and can
    # differ harmlessly between shells. Build-affecting variables are compared
    # exactly, including unset values recorded by Soong as empty strings.
    ignored = {"PATH", "PWD", "SHELL", "TMPDIR", "HOME", "USER", "LOGNAME"}
    changed: list[str] = []
    for row in used:
        key = row.get("Key")
        expected = row.get("Value", "")
        if not key or key in ignored:
            continue
        actual = os.environ.get(key, "")
        if actual != expected:
            changed.append(f"{key}: graph={expected!r}, current={actual!r}")
    # These variables are build-relevant even if the last Soong invocation did
    # not read them during analysis (for example ccache wrappers and jobs).
    for key in ("USE_CCACHE", "CCACHE_DIR", "CCACHE_EXEC", "CCACHE_COMPILERCHECK", "CCACHE_BASEDIR", "CCACHE_SLOPPINESS", "SOONG_INCREMENTAL_ANALYSIS", "SOONG_PARTIAL_ANALYSIS", "ALLOW_MISSING_DEPENDENCIES"):
        if key not in {row.get("Key") for row in used} and os.environ.get(key):
            changed.append(f"{key}: current={os.environ[key]!r}, absent from the graph environment")
    expected_product = "pc_x86_64"
    expected_variant = "userdebug"
    if os.environ.get("TARGET_PRODUCT") != expected_product:
        changed.append(f"TARGET_PRODUCT must be {expected_product!r}, got {os.environ.get('TARGET_PRODUCT', '')!r}")
    if os.environ.get("TARGET_BUILD_VARIANT") != expected_variant:
        changed.append(f"TARGET_BUILD_VARIANT must be {expected_variant!r}, got {os.environ.get('TARGET_BUILD_VARIANT', '')!r}")
    if changed:
        print("ERROR: build environment differs from the one used to generate this graph:", file=sys.stderr)
        for item in changed[:30]:
            print(f"  {item}", file=sys.stderr)
        return 1
    print(f"Graph inputs and build environment match {ninja}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
