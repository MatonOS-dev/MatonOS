#!/usr/bin/env python3
"""Actual public staged verifier with tiny genuinely GPG-signed OSTree commits.

Only verity ioctls and the statx verity-state bits are mocked. Positive begin needs CAP_CHOWN to another UID
and is not claimed here: registration's private trust repository is prepared
by the harness. No fake OSTree, Flatpak, checksum or signature subprocesses.
Requires host UID1000, GnuPG, GLib and host-compatible OSTree; no network.
"""
import ctypes
import ctypes.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
OSTREE = os.environ.get('MATON_TEST_OSTREE', '/mnt/data/aosp/out/pc-logs/musl-324/apex/com.matonos.flatpak/bin/ostree')
APP = 'app/org.matonos.VerifierTest/x86_64/stable'
RUNTIME = 'runtime/org.matonos.TestPlatform/x86_64/stable'


def run(*args, **kwargs):
    result = subprocess.run(args, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f'{args}: {result.stdout!r}\n{result.stderr!r}')
    return result.stdout


def variant_bytes(text):
    glib = ctypes.CDLL(ctypes.util.find_library('glib-2.0'))
    glib.g_variant_parse.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    glib.g_variant_parse.restype = ctypes.c_void_p
    glib.g_variant_get_size.argtypes = [ctypes.c_void_p]; glib.g_variant_get_size.restype = ctypes.c_size_t
    glib.g_variant_get_data.argtypes = [ctypes.c_void_p]; glib.g_variant_get_data.restype = ctypes.c_void_p
    glib.g_variant_unref.argtypes = [ctypes.c_void_p]
    value = glib.g_variant_parse(None, text.encode(), None, None, None)
    assert value, 'invalid GVariant fixture'
    result = ctypes.string_at(glib.g_variant_get_data(value), glib.g_variant_get_size(value))
    glib.g_variant_unref(value)
    return result


def main():
    os.umask(0o022)
    if os.getuid() != 1000 or not Path(OSTREE).is_file() or not shutil.which('gpg') or not ctypes.util.find_library('glib-2.0'):
        print('SKIP: requires UID1000, host OSTree, GnuPG and GLib')
        return 77
    with tempfile.TemporaryDirectory(prefix='staged-publish-') as temp:
        work = Path(temp); home = work / 'gpg'; home.mkdir(mode=0o700)
        def gpg(*args):
            return run('gpg', '--homedir', str(home), '--batch', '--pinentry-mode', 'loopback', '--passphrase', '', *args)
        gpg('--quick-generate-key', 'MatonOS local verifier fixture', 'rsa2048', 'sign', '0')
        key = next(line.split(':')[9] for line in gpg('--with-colons', '--list-keys').decode().splitlines() if line.startswith('fpr:'))
        pubkey = gpg('--export', key)
        source = work / 'source'
        run(OSTREE, f'--repo={source}', 'init', '--mode=bare-user-only')
        commits = []
        for ref, metadata in [(APP, '[Application]\nname=org.matonos.VerifierTest\nruntime=org.matonos.TestPlatform/x86_64/stable\n'), (RUNTIME, '[Runtime]\nname=org.matonos.TestPlatform\n')]:
            tree = work / ('app-tree' if ref == APP else 'runtime-tree')
            (tree / 'files/bin').mkdir(parents=True)
            (tree / 'metadata').write_text(metadata)
            (tree / 'files/bin/payload').write_text('authenticated fixture payload\n')
            (tree / 'files/bin/payload').chmod(0o755)
            commit = run(OSTREE, f'--repo={source}', 'commit', f'--branch={ref}', '--subject=fixture', '--no-xattrs', f'--tree=dir={tree}').decode().strip()
            obj = source / 'objects' / commit[:2] / (commit[2:] + '.commit')
            signature = work / 'signature'
            gpg('--yes', '--digest-algo', 'SHA256', '--local-user', key, '--detach-sign', '--output', str(signature), str(obj))
            sig = ','.join(f'0x{x:02x}' for x in signature.read_bytes())
            obj.with_name(commit[2:] + '.commitmeta').write_bytes(variant_bytes("{'ostree.gpgsigs': <@aay [[" + sig + "]]>}"))
            commits.append(commit)
        store = work / 'linux-app'; store.mkdir()
        driver = work / 'driver.c'
        driver.write_text(r'''
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <linux/fsverity.h>
#include <stdarg.h>
#include <linux/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
static int test_ioctl(int fd,unsigned long command,...) {
    (void)fd; va_list ap;va_start(ap,command);void* arg=va_arg(ap,void*);va_end(ap);
    if(command==FS_IOC_MEASURE_VERITY){struct fsverity_digest* d=arg;d->digest_algorithm=FS_VERITY_HASH_ALG_SHA256;d->digest_size=32;memset(d->digest,42,32);}return 0;
}
static long test_statx(long number,int fd,const char* path,int flags,unsigned mask,struct statx* result) {
    long rc=syscall(number,fd,path,flags,mask,result);
    if(!rc){result->stx_attributes_mask|=STATX_ATTR_VERITY;result->stx_attributes|=STATX_ATTR_VERITY;}
    return rc;
}
#define MATON_STAGED_STATX test_statx
#define MATON_VERITY_IOCTL test_ioctl
#include "install/linuxd/FlatpakPublish.c"
static int record_ref(const char* ref,void* context){(void)context;printf("RECORD %s\n",ref);return getenv("FAIL_RECORD")?1:0;}
static int complete_refs(const char*const* refs,size_t count,void* context){(void)context;for(size_t i=0;i<count;i++)printf("COMPLETE %s\n",refs[i]);return 0;}
int main(int argc,char** argv){
    if(argc!=6)return 2;
    struct FlatpakStagedManifest* m=calloc(1,sizeof(*m));if(!m)return 2;
    char registration_error[256]={0};
    if(!flatpak_begin_staged_install(1000,"invalid","fixture",registration_error,sizeof(registration_error)) ||
            !flatpak_begin_staged_install(10000,"../invalid","fixture",registration_error,sizeof(registration_error)))return 2;
    m->version=1;strcpy(m->operation_id,argv[1]);strcpy(m->remote,"fixture");
    strcpy(m->app_ref,"app/org.matonos.VerifierTest/x86_64/stable");strcpy(m->app_commit,argv[2]);
    m->deployment_count=2;strcpy(m->deployments[0].ref,m->app_ref);strcpy(m->deployments[0].commit,argv[2]);
    strcpy(m->deployments[1].ref,"runtime/org.matonos.TestPlatform/x86_64/stable");strcpy(m->deployments[1].commit,argv[3]);
    if(!strcmp(argv[5],"missing-runtime"))m->deployment_count=1;
    if(!strcmp(argv[5],"bad-binding"))strcpy(m->deployments[1].ref,"runtime/org.matonos.Wrong/x86_64/stable");
    char staging[4096],pending[4096],error[1024]={0};
    snprintf(staging,sizeof(staging),MATON_STAGING_ROOT "/10000/%s",argv[1]);
    snprintf(pending,sizeof(pending),MATON_INSTALL_ROOT "/%s",argv[1]);
    int rc=flatpak_verify_publish_staged_tracked(m,staging,argv[4],pending,error,sizeof(error),record_ref,complete_refs,NULL);
    if(rc)fprintf(stderr,"%s\n",error);
    free(m);return rc?1:0;
}
''')
        binary = work / 'driver'
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('MATON_PUBLISH_TEST_SANITIZE') == '1' else []
        run('cc', *flags, '-DMATONOS_PUBLISH_HOST_TEST', f'-DMATON_LINUX_ROOT="{store}"', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', f'-I{ROOT}', str(driver), '-o', str(binary))
        def trusted(repo):
            run(OSTREE, f'--repo={repo}', 'init', '--mode=bare-user-only')
            (repo / 'fixture.trustedkeys.gpg').write_bytes(pubkey)
            for keyname, value in [('url', f'file://{source}'), ('gpg-verify', 'true'), ('gpg-verify-summary', 'true')]:
                run(OSTREE, f'--repo={repo}', '--group=remote "fixture"', 'config', 'set', keyname, value)
        shared = store / 'runtime'; shared.mkdir()
        trusted(shared / 'repo')
        env = dict(os.environ, MATONOS_OSTREE_CLI=OSTREE)
        runtime_inode = None
        for case in ['good', 'runtime-reuse', 'content', 'signature', 'missing-runtime', 'bad-binding', 'staging-keyring', 'repo-control-links', 'record-fail', 'existing-runtime-corrupt']:
            pending = store / 'install' / case; pending.mkdir(parents=True)
            op = store / 'staging/10000' / case; installation = op / 'installation'; installation.mkdir(parents=True)
            trust = op.parent / ('.trust-' + case); trust.mkdir(mode=0o700)
            trusted(trust / 'repo')
            shutil.copytree(source, installation / 'repo', symlinks=True)
            for ref, commit in zip([APP, RUNTIME], commits):
                path = installation / ref / commit; path.parent.mkdir(parents=True)
                run(OSTREE, f'--repo={source}', 'checkout', '--user-mode', '--force-copy', commit, str(path))
                (path / 'deploy').write_bytes(b'attacker supplied deployment bookkeeping')
            if case == 'staging-keyring': (installation / 'repo/fixture.trustedkeys.gpg').write_bytes(b'attacker keyring')
            if case == 'repo-control-links':
                repo = installation / 'repo'
                for name in ('tmp', 'state', 'refs'):
                    path = repo / name
                    if path.is_dir(): shutil.rmtree(path)
                    elif path.exists() or path.is_symlink(): path.unlink()
                    path.symlink_to('/outside-producer-scratch')
                (repo / 'lock').symlink_to('/outside-producer-scratch')
            if case == 'content': (installation / APP / commits[0] / 'files/bin/payload').write_bytes(b'corrupt deployed file')
            if case == 'signature':
                (installation / 'repo/objects' / commits[0][:2] / (commits[0][2:] + '.commitmeta')).write_bytes(b'corrupt signature')
            runtime_file = shared / RUNTIME / commits[1] / 'files/bin/payload'
            if case == 'existing-runtime-corrupt': runtime_file.write_bytes(b'corrupt shared runtime')
            result = subprocess.run([str(binary), case, *commits, str(shared), case], env=dict(env, **({'FAIL_RECORD': '1'} if case == 'record-fail' else {})), capture_output=True, text=True)
            success = case in ('good', 'runtime-reuse', 'staging-keyring', 'repo-control-links')
            assert result.returncode == (0 if success else 1), (case, result.stdout, result.stderr)
            pending = store / 'install' / case
            if success:
                assert 'RECORD ' + RUNTIME in result.stdout
                assert 'COMPLETE ' + RUNTIME in result.stdout
                assert result.stdout.index('RECORD ') < result.stdout.index('COMPLETE ')
                assert run(OSTREE, f'--repo={shared}/repo', 'rev-parse', 'fixture:' + RUNTIME).decode().strip() == commits[1]
                assert run(OSTREE, f'--repo={pending}/repo', 'rev-parse', 'fixture:' + APP).decode().strip() == commits[0]
                published = pending / APP / commits[0]
                assert (published / 'files/bin/payload').read_text() == 'authenticated fixture payload\n'
                assert (published / 'deploy').read_bytes() != b'attacker supplied deployment bookkeeping'
                if runtime_inode is None: runtime_inode = runtime_file.stat().st_ino
                else: assert runtime_file.stat().st_ino == runtime_inode, 'runtime reuse replaced verified inode'
            else:
                assert not (pending / 'app').exists(), (case, 'failed verify published app')
                assert 'COMPLETE ' not in result.stdout
            print('PASS public staged verifier:', case, '(verity ioctl/statx mocked)', flush=True)
        run('gpgconf', '--homedir', str(home), '--kill', 'gpg-agent')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
