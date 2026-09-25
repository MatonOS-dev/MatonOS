#!/usr/bin/env python3
"""Fast static checks for MatonOS' device tree and ODM bundle."""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import sys
from pathlib import Path


DEVICE = Path(__file__).resolve().parents[1]
AOSP = DEVICE.parents[2]
ERRORS: list[str] = []


def error(path: Path | str, message: str) -> None:
    try:
        shown = Path(path).relative_to(DEVICE)
    except (ValueError, TypeError):
        shown = path
    ERRORS.append(f"{shown}: {message}")


def files(root: Path, patterns: tuple[str, ...], *, prune: tuple[str, ...] = ()) -> list[Path]:
    found: list[Path] = []
    for current, dirs, names in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in prune and not d.startswith(".git"))
        for name in names:
            if any(Path(name).match(p) for p in patterns):
                found.append(Path(current, name))
    return found


def strip_comments(source: str) -> str:
    # Keep quoted strings intact while removing both Soong comment forms.
    token = re.compile(r'("(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*)', re.S)
    return token.sub(lambda m: m.group(0) if m.group(0).startswith('"') else " " * len(m.group(0)), source)


def module_blocks(source: str):
    clean = strip_comments(source)
    starts = re.finditer(r"(?m)^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\{", clean)
    for match in starts:
        depth = 1
        pos = match.end()
        quote = False
        escaped = False
        while pos < len(clean) and depth:
            char = clean[pos]
            if quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quote = False
            elif char == '"':
                quote = True
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
            pos += 1
        if depth == 0:
            yield match.group(1), clean[match.end():pos - 1]


def string_property(block: str, name: str) -> str | None:
    match = re.search(rf"(?m)^\s*{re.escape(name)}\s*:\s*\"([^\"]*)\"", block)
    return match.group(1) if match else None


def check_product_makefiles() -> None:
    for path in files(DEVICE, ("*.mk",)):
        lines = path.read_text(errors="replace").splitlines()
        logical_lines: list[tuple[int, str]] = []
        pending = ""
        start = 1
        for lineno, line in enumerate(lines, 1):
            if not pending:
                start = lineno
            piece = re.sub(r"#.*$", "", line).rstrip()
            if piece.endswith("\\"):
                pending += piece[:-1] + " "
                continue
            logical_lines.append((start, pending + piece))
            pending = ""
        if pending:
            logical_lines.append((start, pending))

        for lineno, line in logical_lines:
            clean = re.sub(r"#.*$", "", line).strip()
            if not clean:
                continue
            # PRODUCT_PACKAGES from this device tree must only name local modules.
            if "PRODUCT_PACKAGES" in clean and re.search(r"\b(?:lib[a-zA-Z0-9_.+-]+|lib32_[A-Za-z0-9_.+-]+)\b", clean):
                # Local module names are checked against module declarations below;
                # AOSP lib modules are never valid here because they also install
                # their system variant. Exempt modules owned by this device tree.
                values = re.sub(r".*?\+=", "", clean).strip()
                for module in re.findall(r"\b(?:lib[a-zA-Z0-9_.+-]+|lib32_[A-Za-z0-9_.+-]+)\b", values):
                    if module not in LOCAL_MODULES:
                        error(path, f"line {lineno}: PRODUCT_PACKAGES names library {module!r}; device packages must not pull AOSP /system libraries")

            if "PRODUCT_COPY_FILES" in clean and "+=" in clean:
                rhs = clean.split("+=", 1)[1]
                # Dynamic firmware/Mesa globs use nested $(shell)/$(foreach)
                # calls whose whitespace is Make syntax, not a destination
                # path. Static copy rows below are checked token by token.
                if "$(shell" in rhs or "$(foreach" in rhs:
                    continue
                for item in rhs.split():
                    if ":" not in item:
                        # Make variables/functions may expand to file pairs at
                        # build time. A literal token here is usually one half
                        # of a path accidentally split by an embedded space.
                        if "$" not in item:
                            error(path, f"line {lineno}: malformed PRODUCT_COPY_FILES entry (possible space in path): {item!r}")
                        continue
                    source, destination = item.split(":", 1)
                    if " " in source or " " in destination:
                        error(path, f"line {lineno}: PRODUCT_COPY_FILES path contains spaces: {item!r}")
                    if re.search(r"(?:^|/)vintf/.*\.xml$|(?:^|/)manifest/.*\.xml$", destination) or re.search(r"(?:^|/)vintf/.*\.xml$|(?:^|/)manifest/.*\.xml$", source):
                        error(path, f"line {lineno}: VINTF XML must not use PRODUCT_COPY_FILES: {item!r}; use the ODM bundle or DEVICE_MANIFEST_FILE")


def check_android_bp() -> None:
    for path in files(DEVICE, ("Android.bp",), prune=("build", "out", "prebuilt")):
        source = path.read_text(errors="replace")
        for module_type, block in module_blocks(source):
            name = string_property(block, "name") or "<unnamed>"
            if module_type in ("android_app", "android_app_import"):
                partitioned = any(re.search(rf"(?m)^\s*{prop}\s*:\s*true\b", block) for prop in ("vendor", "system_ext_specific", "product_specific", "soc_specific", "device_specific"))
                if not partitioned:
                    error(path, f"{module_type} {name} has no non-/system partition flag")
            if module_type == "android_app_import":
                if re.search(r'(?m)^\s*certificate\s*:\s*"PRESIGNED"', block):
                    error(path, f"android_app_import {name} sets certificate: \"PRESIGNED\"; use preprocessed: true for byte-preserved prebuilts")
                apk = string_property(block, "apk")
                if apk and not (path.parent / apk).is_file():
                    error(path, f"android_app_import {name} apk is missing: {apk}")
            # Check literal src/srcs files for prebuilt module declarations.
            if module_type.startswith("prebuilt_") or "prebuilt" in module_type:
                for prop in ("src", "apk"):
                    value = string_property(block, prop)
                    if value and not (path.parent / value).is_file():
                        error(path, f"{module_type} {name} {prop} is missing: {value}")
                srcs = re.search(r"(?m)^\s*srcs\s*:\s*\[([^\]]*)\]", block, re.S)
                if srcs:
                    for value in re.findall(r'"([^\"]+)"', srcs.group(1)):
                        if not any(c in value for c in "*?") and not (path.parent / value).is_file():
                            error(path, f"{module_type} {name} srcs file is missing: {value}")


def check_local_modules() -> None:
    global LOCAL_MODULES
    LOCAL_MODULES = set()
    for path in files(DEVICE, ("Android.bp",)):
        for _, block in module_blocks(path.read_text(errors="replace")):
            name = string_property(block, "name")
            if name:
                LOCAL_MODULES.add(name)


def check_bundle_registry() -> None:
    registry = DEVICE / "bundle/contents.list"
    if not registry.is_file():
        error(registry, "ODM bundle registry is missing")
        return
    for lineno, line in enumerate(registry.read_text(errors="replace").splitlines(), 1):
        try:
            fields = shlex.split(line, comments=True)
        except ValueError as exc:
            error(registry, f"line {lineno}: cannot parse registry row: {exc}")
            continue
        if not fields:
            continue
        kind = fields[0]
        if kind in ("file", "tree") and len(fields) == 3:
            source = DEVICE / fields[1]
            expected = source.is_file() if kind == "file" else source.is_dir()
            if not expected:
                error(registry, f"line {lineno}: {kind} source is missing: {fields[1]}")
        elif kind == "prop" and len(fields) == 2 and "=" in fields[1]:
            continue
        else:
            error(registry, f"line {lineno}: invalid bundle row: {' '.join(fields)}")


def check_fixed_sepolicy_files() -> None:
    allowed = {"matonos_driver.te", "matonos_bridge.te", "file_contexts", "property.te", "property_contexts", "service_contexts"}
    root = DEVICE / "sepolicy/matonos"
    if not root.is_dir():
        error(root, "fixed MatonOS SELinux policy directory is missing")
        return
    for path in root.iterdir():
        if path.is_file() and path.name not in allowed:
            error(path, f"new file violates the fixed sepolicy/matonos file set (allowed: {', '.join(sorted(allowed))})")


def check_patches() -> None:
    root = DEVICE / "patches"
    if root.exists():
        for path in root.rglob("*"):
            if path.is_file():
                error(path, "AOSP patches are disabled by the zero-patches rule; retire or move this file out of patches/")


def check_bpfmt() -> None:
    bp_files = files(DEVICE, ("Android.bp",), prune=("build", "out", "prebuilt"))
    if not bp_files:
        return
    tool = AOSP / "out/host/linux-x86/bin/bpfmt"
    if not tool.is_file():
        tool = Path("bpfmt")
    try:
        result = subprocess.run([str(tool), "-l", *map(str, bp_files)], text=True, capture_output=True, check=False)
    except OSError as exc:
        error("Android.bp", f"cannot run bpfmt -l: {exc}")
        return
    if result.returncode:
        error("Android.bp", f"bpfmt -l failed (exit {result.returncode}): {result.stderr.strip()}")
    for line in result.stdout.splitlines():
        error(line, "Android.bp is not formatted; run bpfmt -w on this file")


def main() -> int:
    global LOCAL_MODULES
    LOCAL_MODULES = set()
    check_local_modules()
    check_product_makefiles()
    check_android_bp()
    check_bundle_registry()
    check_fixed_sepolicy_files()
    check_patches()
    check_bpfmt()
    if ERRORS:
        print(f"Preflight failed with {len(ERRORS)} issue(s):", file=sys.stderr)
        for item in ERRORS:
            print(f"  FAIL: {item}", file=sys.stderr)
        return 1
    print("Preflight passed: device modules, prebuilts, ODM registry, SELinux file set, patches, and Android.bp formatting are clean.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
