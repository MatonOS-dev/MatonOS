#!/usr/bin/env python3
"""Exercise offline publication commands with a fake OSTree/Flatpak CLI.

This checks ordering and failure propagation, not real GPG verification.
Usage: python3 gl-publish-test.py OUTPUT_DIRECTORY
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

tests = Path(__file__).resolve().parent
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
driver = output / "gl-publish-driver"
subprocess.run(["cc", "-DMATONOS_PUBLISH_HOST_TEST", "-Wall", "-Wextra", "-Werror",
                str(tests.parent / "FlatpakPublish.c"), str(tests.parent / "RuntimeRefs.c"), str(tests / "flatpak-publish-driver.c"),
                "-o", str(driver)], check=True)
fake = output / "gl-fake-cli"
fake.write_text('''#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
if "install" in args and os.getenv("PUBLISH_DB_DIR"):
    with open(os.environ["PUBLISH_DB_DIR"] + "/refs-0.json") as database:
        owners = json.load(database)
    assert 10120 in owners[os.environ["PUBLISH_OLD_REF"]]
    assert 10120 in owners[os.environ["PUBLISH_NEW_REF"]]
with open(os.environ["GL_TEST_LOG"], "a") as log:
    log.write(json.dumps({"args": args, "system": os.getenv("FLATPAK_SYSTEM_DIR")}) + "\\n")
if "config" in args and "get" in args:
    print("true")
elif "cat" in args:
    if os.getenv("GL_TEST_METADATA"):
        data = json.loads(os.environ["GL_TEST_METADATA"])
        print(data[args[args.index("cat") + 1][0]])
        sys.exit(0)
    print("[Extension org.freedesktop.Platform.GL]\\nversions=25.08;25.08-extra;1.4\\nversion=1.4")
elif "refs" in args:
    # Like real ostree: trailing arguments filter by ref-name prefix.
    filters = [a for a in args[args.index("refs") + 1:] if not a.startswith("-")]
    for ref in json.loads(os.getenv("GL_TEST_REFS", "{}")):
        if all(("flathub:" + ref).startswith(f + "/") for f in filters):
            print("flathub:" + ref)
elif "rev-parse" in args:
    if os.getenv("GL_TEST_REFS"):
        refs = json.loads(os.environ["GL_TEST_REFS"])
        ref = args[-1].split(":", 1)[-1]
        if ref not in refs:
            sys.exit(1)
        print(refs[ref] * 64)
        sys.exit(0)
    if ".GL.default/" in args[-1] and os.getenv("GL_TEST_FAIL") == "missing":
        sys.exit(1)
    print(("c" if ".GL.default/" in args[-1] else "a" if "app/" in args[-1] else "b") * 64)
elif "pull-local" in args and ".GL.default/" in args[-1] and os.getenv("GL_TEST_FAIL") == "signature":
    sys.exit(1)
elif "install" in args and ".GL.default//" in args[-1] and os.getenv("GL_TEST_FAIL") == "deploy":
    sys.exit(1)
''')
fake.chmod(0o755)
# Metadata and staged refs emulate the authenticated installer transaction.
app = "app/org.example.Test/x86_64/stable"
runtime = "runtime/org.kde.Platform/x86_64/5.15-25.08"
base_app = "app/org.example.Base/x86_64/24.08"
plugin = "runtime/org.example.Test.Plugin/x86_64/stable"
codec = "runtime/org.example.Codec/x86_64/25.08"
locale = "runtime/org.kde.Platform.Locale/x86_64/5.15-25.08"
child = "runtime/org.example.Test.Plugins.Audio/x86_64/v2"
compat = "runtime/org.freedesktop.Platform.Compat.i386/x86_64/25.08"
gl32 = "runtime/org.freedesktop.Platform.GL32.default/x86_64/25.08"
gl = "runtime/org.freedesktop.Platform.GL.default/x86_64/25.08"
refs = {app: "a", runtime: "b", gl: "c", base_app: "d", plugin: "e",
        codec: "f", locale: "1", child: "2", compat: "3", gl32: "4"}
metadata = {
    "a": """[Application]
name=org.example.Test
runtime=org.kde.Platform/x86_64/5.15-25.08
base=org.example.Base
base-version=24.08
[Extension org.example.Test.Plugin]
[Extension org.example.Test.Plugins]
versions=v1;v2;
subdirectories=true
merge-dirs=plugins;
[Extension org.freedesktop.Platform.Compat.i386]
version=25.08
[Extension org.freedesktop.Platform.GL32]
versions=25.08;1.4;
subdirectories=true
no-autodownload=true
download-if=active-gl-driver
[Extension org.example.Test.Debug]
[Extension org.example.Test.Sources]
[Extension org.example.Missing]
[Extension org.example.Disabled]
no-autodownload=true
""",
    "b": """[Runtime]
name=org.kde.Platform
[Extension org.freedesktop.Platform.GL]
versions=25.08;1.4;
subdirectories=true
no-autodownload=true
download-if=active-gl-driver
[Extension org.example.Codec]
version=25.08
autodownload=true
[Extension org.kde.Platform.Locale]
""",
    "e": "[Runtime]\n[ExtensionOf]\nref=app/org.example.Test/x86_64/stable\n",
}
for ref, pin in refs.items():
    metadata.setdefault(pin, "[Runtime]\n" + ("[ExtensionOf]\n" if ref not in (app, runtime, base_app) else ""))
refs["runtime/org.example.Test.Debug/x86_64/stable"] = "5"
refs["runtime/org.example.Test.Sources/x86_64/stable"] = "6"

for scenario in ("pulled-refs", "missing-runtime", "app-pin", "runtime-pin", "signature", "deploy", "large-list", "invalid-ref", "record-failure"):
    selected = dict(refs)
    if scenario == "missing-runtime": del selected[runtime]
    if scenario == "app-pin": selected[app] = "7"
    if scenario == "runtime-pin": selected[runtime] = "8"
    if scenario == "large-list":
        for i in range(300): selected[f"runtime/org.example.Extra{i}{"x" * 210}/x86_64/stable"] = "f"
    if scenario == "invalid-ref": selected["garbage"] = "f"
    with tempfile.TemporaryDirectory(dir=output) as temp:
        root = Path(temp)
        for name in ("R", "S", "U"): (root / name / "repo").mkdir(parents=True)
        (root / "U/repo/config").write_text('[remote "flathub"]\n')
        log = root / "calls.jsonl"
        env = dict(os.environ, MATONOS_FLATPAK_CLI=str(fake), MATONOS_OSTREE_CLI=str(fake),
                   GL_TEST_LOG=str(log), PUBLISH_REF_LOG=str(root / "ref-log"),
                   GL_TEST_REFS=json.dumps(selected),
                   GL_TEST_METADATA=json.dumps(metadata), GL_TEST_FAIL=scenario)
        if scenario == "record-failure": env["PUBLISH_RECORD_FAIL"] = "1"
        result = subprocess.run([str(driver), app, "a" * 64, runtime, "b" * 64,
                                 "flathub", str(root / "R"), str(root / "S"), str(root / "U")],
                                env=env, capture_output=True, text=True)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        installs = [c for c in calls if "install" in c["args"]]
        assert (result.returncode == 0) == (scenario in ("pulled-refs", "large-list")), (scenario, result.stderr)
        if scenario not in ("pulled-refs", "large-list", "deploy"): assert not installs
        if scenario in ("pulled-refs", "large-list"):
            if scenario == "large-list":
                assert len(selected) > 64
                assert sum(len("flathub:" + r + "\n") for r in selected) > 65536
            pulls = [c for c in calls if "pull-local" in c["args"]]
            expected = {r for r in selected if not any(x in r for x in (".Debug/", ".Sources/"))}
            assert {c["args"][-1] for c in pulls} == expected
            recorded = set((root / "ref-log").read_text().splitlines())
            assert recorded == {r for r in expected if r != app and not r.split("/")[1].startswith("org.example.Test.")}
            for c in pulls:
                user = c["args"][-1] == app or c["args"][-1].split("/")[1].startswith("org.example.Test.")
                assert c["args"][0] == "--repo=" + str(root / ("U" if user else "S") / "repo")
                assert "--gpg-verify" in c["args"] and "--untrusted" in c["args"]
            assert installs[-1]["args"][-1] == "org.example.Test//stable"
            assert {c["args"][-1] for c in installs[:2]} == {"org.kde.Platform//5.15-25.08", "org.example.Base//24.08"}
            assert all("--no-pull" in c["args"] for c in installs)
            assert all("--user" in c["args"] for c in installs if "org.example.Test." in c["args"][-1])
        assert not any("--no-deploy" in c["args"] or "remote-ls" in c["args"] for c in calls)
        print("PASS: staged dependency publication", scenario)

# Update an existing UID through the actual publication protection/completion hooks.
old = "runtime/org.example.Old/x86_64/stable"
for scenario in ("success", "other-owner", "deploy"):
    with tempfile.TemporaryDirectory(dir=output) as temp:
        root = Path(temp)
        for name in ("R", "S", "U"): (root / name / "repo").mkdir(parents=True)
        (root / "U/repo/config").write_text('[remote "flathub"]\n')
        database = root / "refs-0.json"
        database.write_text(json.dumps({old: [10120, 10122] if scenario == "other-owner" else [10120]}))
        database.chmod(0o600)
        log = root / "calls.jsonl"
        env = dict(os.environ, MATONOS_FLATPAK_CLI=str(fake), MATONOS_OSTREE_CLI=str(fake),
                   GL_TEST_LOG=str(log), PUBLISH_REF_LOG=str(root / "ref-log"), PUBLISH_DB_DIR=str(root), PUBLISH_OLD_REF=old, PUBLISH_NEW_REF=runtime,
                   GL_TEST_REFS=json.dumps(refs), GL_TEST_METADATA=json.dumps(metadata), GL_TEST_FAIL=scenario)
        result = subprocess.run([str(driver), app, "a" * 64, runtime, "b" * 64,
                                 "flathub", str(root / "R"), str(root / "S"), str(root / "U")],
                                env=env, capture_output=True, text=True)
        assert (result.returncode == 0) == (scenario != "deploy"), result.stderr
        state = json.loads(database.read_text())
        assert state[runtime] == [10120]
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        removed = [c["args"][-1] for c in calls if "uninstall" in c["args"]]
        if scenario == "success":
            assert old not in state and removed == [old]
            assert "install" in calls[-2]["args"] and "uninstall" in calls[-1]["args"]
        elif scenario == "other-owner": assert state[old] == [10122] and not removed
        else: assert state[old] == [10120] and not removed
        print("PASS: publication update ownership", scenario)
