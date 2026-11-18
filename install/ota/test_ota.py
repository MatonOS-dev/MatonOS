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
    (images / "system/build.prop").write_text("ro.matonos.build_id=420\nro.build.version.release=1.2\n")
    for index, name in enumerate(NAMES):
        source_name = {"matonos-a.efi": "matonos-installed-a.efi", "matonos-b.efi": "matonos-installed-b.efi"}.get(name, name)
        (images / source_name).write_bytes((f"fake image {index} " * 40).encode())
    output = temp / "published"
    env = os.environ.copy(); env["MATON_OTA_KEY"] = str(key); env["MATON_OTA_PUBLIC_KEY"] = str(pub)
    run(str(ROOT / "tools/ota-publish.sh"), str(images), "stable", str(output), env=env)
    release = output / "stable/420"
    run(str(verifier), str(release), str(pub), "419")
    assert json.loads((output / "stable/latest.json").read_text())["build_id"] == 420
    assert (output / "stable/latest.json.hwfm").is_file()

    (release / "system.erofs").write_bytes(b"tampered")
    run(str(verifier), str(release), str(pub), "419", ok=False)
    # Restore content by re-publishing into a separate output, then check downgrade.
    output2 = temp / "published2"
    run(str(ROOT / "tools/ota-publish.sh"), str(images), "stable", str(output2), env=env)
    run(str(verifier), str(output2 / "stable/420"), str(pub), "420", ok=False)
print("OTA publisher/signature/hash/tamper/downgrade tests passed")
