#!/usr/bin/env python3
"""Offline real Flatpak import/default verification and remote-key isolation.

Disabled fixture remotes avoid all network I/O. Production remotes stay enabled;
network/summary fetches are fresh-image tests. Requires a host CLI/cached key.
"""
from pathlib import Path
import base64,os,subprocess,tempfile
flatpak=os.environ.get('MATON_TEST_FLATPAK','/mnt/data/aosp/out/pc-logs/musl-324/output/flatpak')
key=Path(os.environ.get('MATON_TEST_GPG_KEY','/mnt/data/aosp/out/pc-logs/musl-324/layout-test-final3/R/flathub.trustedkeys.gpg'))
with tempfile.TemporaryDirectory(prefix='repo-keyring-',dir='/tmp') as d:
    p=Path(d);file=p/'repo.flatpakrepo'
    file.write_text('[Flatpak Repo]\nTitle=Test\nUrl=https://example.org/\nGPGKey='+base64.b64encode(key.read_bytes()).decode()+'\n')
    env=dict(os.environ,HOME=d,XDG_CONFIG_HOME=d+'/config',XDG_DATA_HOME=d+'/data',XDG_CACHE_HOME=d+'/cache',FLATPAK_SYSTEM_DIR=d+'/runtime',FLATPAK_USER_DIR=d+'/user')
    def run(*args,ok=True):
        result=subprocess.run([flatpak,*args],env=env,capture_output=True,text=True,timeout=15)
        assert (result.returncode==0)==ok,result.stderr
        return result.stdout
    run('remote-add','--system','--from','--disable','first',str(file))
    run('remote-add','--system','--from','--disable','first',str(file),ok=False)
    run('remote-add','--system','--from','--disable','second',str(file))
    repo=p/'runtime/repo';first=repo/'first.trustedkeys.gpg';second=repo/'second.trustedkeys.gpg'
    assert first.stat().st_size and second.stat().st_size
    import configparser
    config=configparser.ConfigParser();config.read(repo/'config')
    for name in ('first','second'):
        section=config[f'remote "{name}"']
        assert section.getboolean('gpg-verify') and section.getboolean('gpg-verify-summary')
    preserved=second.read_bytes();run('remote-delete','--system','--force','first')
    assert not first.exists() and second.read_bytes()==preserved
    print('PASS actual Flatpak .flatpakrepo GPG import, default commit/summary verification, duplicate rejection and separate remote keyrings (offline disabled fixtures)')
