#!/usr/bin/env python3
"""Validate rebuilt deploy bookkeeping against GLib and atomic layout rules.

No crypto or seal is mocked here: inputs represent an already-verified tree;
this test exercises only the trusted bookkeeping/publication helper.
"""
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
COMMIT = "a" * 64
REF = "app/org.example.App/x86_64/stable"
lib = c.CDLL("libglib-2.0.so.0")
lib.g_variant_parse.restype = c.c_void_p
lib.g_variant_parse.argtypes = [c.c_void_p, c.c_char_p, c.c_void_p, c.c_void_p,
                               c.POINTER(c.c_void_p)]
lib.g_variant_get_size.argtypes = [c.c_void_p]
lib.g_variant_get_size.restype = c.c_size_t
lib.g_variant_get_data.argtypes = [c.c_void_p]
lib.g_variant_get_data.restype = c.c_void_p
lib.g_variant_unref.argtypes = [c.c_void_p]


def variant(origin, commit, paths, runtime, extension, size):
    values = ["'deploy-version': <4>"]
    if runtime:
        values.append(f"'runtime': <{json.dumps(runtime)}>")
    if extension:
        values.append(f"'extension-of': <{json.dumps(extension)}>")
    be_size = int.from_bytes(size.to_bytes(8, "big"), "little")
    text = (f"({json.dumps(origin)}, {json.dumps(commit)}, @as {json.dumps(paths)}, "
            f"uint64 {be_size}, {{{', '.join(values)}}})")
    error = c.c_void_p()
    value = lib.g_variant_parse(None, text.encode(), None, None, c.byref(error))
    assert value, text
    try:
        return c.string_at(lib.g_variant_get_data(value), lib.g_variant_get_size(value))
    finally:
        lib.g_variant_unref(value)


with tempfile.TemporaryDirectory(prefix="maton-staged-deployment-") as temp:
    work = Path(temp)
    driver = work / "driver.c"
    driver.write_text(r'''
#define _GNU_SOURCE
#define MATONOS_PUBLISH_HOST_TEST
#include "install/linuxd/StagedDeployment.h"
int main(int argc,char**argv) {
    if(argc<2)return 2;
    if(!strcmp(argv[1],"variant")) {
        if(argc<8)return 2;
        struct sd_bytes* b=calloc(1,sizeof(*b));
        if(!b)return 2;
        int rc=sd_deploy_bytes(b,argv[2],argv[3],(const char* const*)argv+8,
                (size_t)(argc-8),argv[4],argv[5],strtoull(argv[6],NULL,10));
        if(!rc && fwrite(b->data,1,b->size,stdout)!=b->size)rc=1;
        free(b);return rc;
    }
    if(argc<7)return 2;
    char error[512]={0};errno=0;
    int rc=maton_staged_deploy(argv[2],argv[3],argv[4],argv[5],argv[6],
            (const char* const*)argv+7,(size_t)(argc-7),error,sizeof(error));
    if(rc)fprintf(stderr,"%d %s\n",errno,error);
    return rc?1:0;
}
''')
    binary = work / "driver"
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", f"-I{ROOT}",
                    str(driver), "-o", str(binary)], check=True)
    cases = [([], "", "", 0), (["/en", "/fr"], "org.example.Platform/x86_64/stable", "", 123456),
             ([], "", "runtime/org.example.Platform/x86_64/stable", 2**40),
             (["/" + "a" * 250 + str(i) for i in range(256)], "r" * 500, "e" * 500, 65536)]
    for paths, runtime, extension, size in cases:
        result = subprocess.run([str(binary), "variant", "flathub", COMMIT,
                                 runtime, extension, str(size), "reserved", *paths],
                                capture_output=True, check=True)
        assert result.stdout == variant("flathub", COMMIT, paths, runtime, extension, size)
    print("PASS: generated deployment bytes equal real GLib normal form (all offset widths)")

    def source(name):
        root = work / name
        root.mkdir(mode=0o755)
        (root / "files").mkdir(mode=0o755)
        (root / "files/payload").write_bytes(b"verified payload")
        (root / "metadata").write_text("[Application]\nname=org.example.App\nruntime=org.example.Platform/x86_64/stable\n")
        (root / "deploy").write_bytes(b"untrusted bookkeeping")
        return root

    target = work / "target"
    target.mkdir(mode=0o755)
    src = source("source")
    command = lambda s, t=target: [str(binary), "publish", str(s), str(t), REF, COMMIT, "flathub"]
    subprocess.run(command(src), check=True)
    deployed = target / REF / COMMIT
    assert not src.exists()
    assert (target / REF / "active").readlink() == Path(COMMIT)
    assert (target / "app/org.example.App/current").readlink() == Path("x86_64/stable")
    expected_size = len(b"verified payload") + (deployed / "metadata").stat().st_size
    assert (deployed / "deploy").read_bytes() == variant("flathub", COMMIT, [],
                       "org.example.Platform/x86_64/stable", "", expected_size)
    again = source("again")
    result = subprocess.run(command(again), capture_output=True)
    assert result.returncode != 0 and again.exists()
    assert (deployed / "files/payload").read_bytes() == b"verified payload"
    print("PASS: rebuild untrusted bookkeeping, publish layout, reject existing destination")

    outside = work / "outside"
    outside.mkdir(mode=0o755)
    hostile = work / "hostile"
    hostile.mkdir(mode=0o755)
    (hostile / "app").symlink_to(outside, target_is_directory=True)
    src = source("symlink-source")
    result = subprocess.run(command(src, hostile), capture_output=True)
    assert result.returncode != 0 and src.exists() and not list(outside.iterdir())
    print("PASS: target directory symlink substitution is rejected")
