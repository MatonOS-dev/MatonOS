#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NAMES = ("system.erofs", "system_ext.erofs", "product.erofs", "vendor.erofs",
         "odm.erofs", "vbmeta.img", "matonos-a.efi", "matonos-b.efi")

def run(*args, ok=True, env=None):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    if (result.returncode == 0) != ok:
        raise AssertionError(f"command {args} returned {result.returncode}: {result.stdout}{result.stderr}")
    return result

with tempfile.TemporaryDirectory(prefix="matonos-ota-test-") as temp:
    temp = Path(temp)
    key, pub = temp / "key.pem", temp / "pub.pem"
    run("openssl", "genpkey", "-algorithm", "ED25519", "-out", str(key))
    run("openssl", "pkey", "-in", str(key), "-pubout", "-out", str(pub))
    verifier = temp / "ota-manifest-verify"
    run("c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        str(ROOT / "install/ota/verify_main.cpp"), str(ROOT / "install/service/OtaManifest.cpp"),
        "-lcrypto", "-o", str(verifier))

    images = temp / "images"
    (images / "system").mkdir(parents=True)
    (images / "system/build.prop").write_text("ro.build.date.utc=1795430400\nro.build.version.release=18\n")
    for index, name in enumerate(NAMES):
        source_name = {"matonos-a.efi": "matonos-installed-a.efi", "matonos-b.efi": "matonos-installed-b.efi"}.get(name, name)
        (images / source_name).write_bytes((f"fake image {index} " * 40).encode())
    output = temp / "published"
    env = os.environ.copy(); env["MATON_OTA_KEY"] = str(key); env["MATON_OTA_PUBLIC_KEY"] = str(pub)
    run(str(ROOT / "tools/ota-publish.sh"), str(images), str(output), env=env)
    release = output / "updates/1795430400"
    run(str(verifier), str(release), str(pub), "17")
    manifest = json.loads((release / "manifest.json").read_text())
    assert "channel" not in manifest
    assert manifest["build"]["android_base"] == 18
    run(str(ROOT / "tools/ota-promote.sh"), str(output), "testing", "1795430400")
    pointer = output / "updates/testing-manifest.json"
    assert json.loads(pointer.read_text()) == {"format": 1, "update_id": 1795430400, "version": "18", "android_base": 18}
    assert pointer.with_suffix(".json.hwfm").is_file()
    run(str(ROOT / "tools/ota-promote.sh"), str(output), "main", "1795430400")

    (release / "system.erofs").write_bytes(b"tampered")
    run(str(verifier), str(release), str(pub), "17", ok=False)
    # Restore content by re-publishing into a separate output.
    output2 = temp / "published2"
    run(str(ROOT / "tools/ota-publish.sh"), str(images), str(output2), env=env)
    run(str(verifier), str(output2 / "updates/1795430400"), str(pub), "18")
    newer_images = temp / "newer-images"
    (newer_images / "system").mkdir(parents=True)
    (newer_images / "system/build.prop").write_text("ro.build.date.utc=1795430401\nro.build.version.release=18\n")
    for index, name in enumerate(NAMES):
        source_name = {"matonos-a.efi": "matonos-installed-a.efi", "matonos-b.efi": "matonos-installed-b.efi"}.get(name, name)
        (newer_images / source_name).write_bytes((f"new fake image {index} " * 40).encode())
    run(str(ROOT / "tools/ota-publish.sh"), str(newer_images), str(output), env=env)
    run(str(ROOT / "tools/ota-promote.sh"), str(output), "testing", "1795430401")
    # Same Android base allows promoting an older build id.
    run(str(ROOT / "tools/ota-promote.sh"), str(output), "testing", "1795430400")
    older_base_images = temp / "older-base-images"
    (older_base_images / "system").mkdir(parents=True)
    (older_base_images / "system/build.prop").write_text("ro.build.date.utc=1795430402\nro.build.version.release=17\n")
    for index, name in enumerate(NAMES):
        source_name = {"matonos-a.efi": "matonos-installed-a.efi", "matonos-b.efi": "matonos-installed-b.efi"}.get(name, name)
        (older_base_images / source_name).write_bytes((f"older base image {index} " * 40).encode())
    run(str(ROOT / "tools/ota-publish.sh"), str(older_base_images), str(output), env=env)
    older_base_id = "1795430402"
    run(str(verifier), str(output / "updates" / older_base_id), str(pub), "18", ok=False)
    run(str(ROOT / "tools/ota-promote.sh"), str(output), "testing", older_base_id, ok=False)
print("OTA publisher/signature/hash/base-downgrade tests passed")
