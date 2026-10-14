#!/usr/bin/env python3
"""Exercise the actual offline trust gate against a cached signed app.

Requires host-compatible OSTree and a cached signed Flathub repository. No
network/key generation: copy only the selected app's objects, not its runtime.
Override MATON_TEST_OSTREE and MATONOS_FLATPAK_TEST_REPO on another machine.
"""
import os, shutil, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[3]
ostree=os.environ.get('MATON_TEST_OSTREE','/mnt/data/aosp/out/pc-logs/musl-324/output/ostree')
cache=Path(os.environ.get('MATONOS_FLATPAK_TEST_REPO','/mnt/data/aosp/out/pc-logs/musl-324/layout-test-final3/R'))
app='app/org.gnome.Calculator/x86_64/stable'
commit='0032886e3396c2e0cb107d453d600301c8090241d3fef31ca3f688510dba0e43'
with tempfile.TemporaryDirectory(prefix='fpinstall-signature-',dir='/tmp') as temp:
    work=Path(temp);env=dict(os.environ,MATONOS_OSTREE_CLI=ostree)
    def run(*args):
        result=subprocess.run(args,env=env,text=True,capture_output=True)
        if result.returncode: print(result.stderr,flush=True)
        result.check_returncode();return result.stdout.strip()
    def trusted_repo(repo):
        repo.mkdir(parents=True)
        run(ostree,f'--repo={repo}','init','--mode=bare-user-only')
        shutil.copyfile(cache/'flathub.trustedkeys.gpg',repo/'flathub.trustedkeys.gpg')
        for key,value in (('url',f'file://{cache}'),('gpg-verify','true'),('gpg-verify-summary','true')):
            run(ostree,f'--repo={repo}','--group=remote "flathub"','config','set',key,value)
    source=work/'source';trusted_repo(source)
    run(ostree,f'--repo={source}','pull-local','--untrusted','--gpg-verify','--remote=flathub',str(cache),app)
    run(ostree,f'--repo={source}','refs','--force',f'--create=flathub:{app}',commit)
    run(ostree,f'--repo={source}','refs','--force',f'--create={app}',commit)
    driver=work/'driver.c'
    driver.write_text('#include "install/linuxd/FlatpakPublish.c"\nint main(int argc,char**argv){if(argc!=6)return 2;return transfer_and_verify(argv[1],argv[2],"flathub",argv[3],argv[4])?1:0;}\n')
    binary=work/'driver'
    run('cc','-DMATONOS_PUBLISH_HOST_TEST','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',f'-I{root}',str(driver),'-o',str(binary))
    for scenario in ('good','tampered-signature','tampered-content','staging-keyring','wrong-linuxd-keyring'):
        staging=work/scenario/'R';target=work/scenario/'S'
        shutil.copytree(source,staging/'repo',symlinks=True);trusted_repo(target/'repo')
        if scenario=='tampered-signature':
            obj=staging/'repo/objects'/commit[:2]/(commit[2:]+'.commitmeta');obj.chmod(0o644);obj.write_bytes(b'not a valid signature')
        elif scenario=='tampered-content':
            line=next(line for line in run(ostree,f'--repo={staging}/repo','ls','-R','-C',commit).splitlines() if line.endswith('/files/bin/gnome-calculator'))
            checksum=line.split()[-2]
            obj=staging/'repo/objects'/checksum[:2]/(checksum[2:]+'.file');obj.chmod(0o644);obj.write_bytes(b'tampered payload')
        elif scenario=='staging-keyring':(staging/'repo/flathub.trustedkeys.gpg').write_bytes(b'attacker keyring')
        elif scenario=='wrong-linuxd-keyring':(target/'repo/flathub.trustedkeys.gpg').write_bytes(b'untrusted linuxd keyring')
        result=subprocess.run([str(binary),str(staging),str(target),app,commit,'unused'],env=env,capture_output=True,text=True)
        if scenario in ('good','staging-keyring'):assert result.returncode==0,result.stderr
        else:
            assert result.returncode!=0,result.stdout
            assert not (target/'app').exists()
        print('PASS real GPG/checksum trust gate:',scenario,flush=True)
