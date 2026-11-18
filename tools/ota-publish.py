#!/usr/bin/env python3
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FILES = ("system.erofs", "system_ext.erofs", "product.erofs", "vendor.erofs",
         "odm.erofs", "vbmeta.img", "matonos-a.efi", "matonos-b.efi")

def fail(message):
    print(f"ota-publish: ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)

def prop(path, key):
    match = re.search(rf"^{re.escape(key)}=(.*)$", path.read_text(), re.M)
    return match.group(1).strip() if match else ""

def main():
    if len(sys.argv) != 4:
        fail("usage: tools/ota-publish.sh <images-dir> <channel> <out-dir>")
    images, channel, out = map(Path, sys.argv[1:])
    channel_s = str(channel)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", channel_s): fail("invalid channel name")
    key = os.environ.get("MATON_OTA_KEY")
    if not key: fail("MATON_OTA_KEY must name the Ed25519 private key")
    key = Path(key)
    if not key.is_file(): fail(f"private key does not exist: {key}")
    repo = Path(__file__).resolve().parents[1]
    public_key = Path(os.environ.get("MATON_OTA_PUBLIC_KEY", repo / "install/ota/ota-ed25519.pub"))
    if not public_key.is_file(): fail(f"public key does not exist: {public_key}")
    build_prop = images / "system" / "build.prop"
    if not build_prop.is_file(): fail(f"missing {build_prop} (system build properties are required)")
    build_id = prop(build_prop, "ro.build.date.utc")
    version = prop(build_prop, "ro.build.version.release")
    if not build_id.isdigit() or int(build_id) <= 0: fail("ro.build.date.utc must be a positive integer")
    if not version: fail("ro.build.version.release is missing")

    build_id = str(int(build_id))
    target = out / channel_s / build_id
    if target.exists(): fail(f"refusing to overwrite existing release directory: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".matonos-ota-", dir=target.parent) as temp_name:
        temp = Path(temp_name)
        derived = temp / "signing.pub"
        subprocess.run(["openssl", "pkey", "-in", str(key), "-pubout", "-out", str(derived)], check=True)
        expected = temp / "expected.pub"
        subprocess.run(["openssl", "pkey", "-pubin", "-in", str(public_key), "-pubout", "-out", str(expected)], check=True)
        if derived.read_bytes() != expected.read_bytes(): fail("MATON_OTA_KEY does not match the installed OTA public key")
        for name in FILES:
            source_name = {"matonos-a.efi": "matonos-installed-a.efi",
                           "matonos-b.efi": "matonos-installed-b.efi"}.get(name, name)
            source = images / source_name
            if name.endswith(".erofs") and not source.is_file():
                source = images / (name.removesuffix(".erofs") + ".img")
            if not source.is_file(): fail(f"missing input file: {source}")
            destination = temp / name
            if name.endswith(".erofs"):
                with source.open("rb") as stream: magic = stream.read(4)
                if magic == bytes.fromhex("3aff26ed"):
                    tool = shutil.which("simg2img")
                    if not tool: fail("sparse partition image requires simg2img on PATH")
                    subprocess.run([tool, str(source), str(destination)], check=True)
                else:
                    shutil.copyfile(source, destination)
            else:
                shutil.copyfile(source, destination)

        entries = []
        for name in FILES:
            data = (temp / name).read_bytes()
            entries.append({"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        manifest = {"format_version": 1, "channel": channel_s,
                    "build": {"id": int(build_id), "version": version}, "files": entries}
        manifest_bytes = (json.dumps(manifest, ensure_ascii=True, separators=(",", ":"), sort_keys=False) + "\n").encode()
        (temp / "manifest.json").write_bytes(manifest_bytes)
        subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(key),
                        "-in", str(temp / "manifest.json"), "-out", str(temp / "manifest.sig")], check=True)
        target.parent.mkdir(parents=True, exist_ok=True)
        temp.rename(target)
    manifest_hash = hashlib.sha256((target / "manifest.json").read_bytes()).hexdigest()
    relative = f"{channel_s}/{build_id}/"
    latest = {"build_id": int(build_id), "version": version, "path": relative,
              "manifest_sha256": manifest_hash}
    parent = out / channel_s
    latest_path = parent / "latest.json"
    staging = parent / ".latest.json.tmp"
    staging.write_text(json.dumps(latest, separators=(",", ":")) + "\n")
    staging.replace(latest_path)
    (parent / "latest.json.hwfm").write_text(json.dumps({
        "Cache-Control": "public, max-age=60", "CDN-Cache-Control": "public, max-age=60"}, separators=(",", ":")) + "\n")
    print(f"Published locally: {target}")
    print(f"Manual upload command (not run): rsync -aS {out}/ <user@host:/document-root/>")

if __name__ == "__main__": main()
