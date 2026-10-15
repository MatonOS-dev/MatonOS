#!/usr/bin/env python3
"""Adversarial deployed-tree tests with real signed OSTree fixture.

Only FS_IOC_* syscalls are mocked, at compile time, to test the namespace and
signed-tree comparator on hosts without fs-verity. This is NOT a sealing,
retained-writer, enforcing-policy, or end-to-end transaction success claim.
No network is used. Missing cached fixtures produce SKIP (77).
"""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
OSTREE = os.environ.get('MATON_TEST_OSTREE', '/mnt/data/aosp/out/pc-logs/musl-324/apex/com.matonos.flatpak/bin/ostree')
CACHE = Path(os.environ.get('MATONOS_FLATPAK_TEST_REPO', '/mnt/data/aosp/out/pc-logs/musl-324/layout-test-final3/R'))
REF = 'app/org.gnome.Calculator/x86_64/stable'
COMMIT = '0032886e3396c2e0cb107d453d600301c8090241d3fef31ca3f688510dba0e43'


def run(*args):
    result = subprocess.run(args, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f'{args}: {result.stdout}\n{result.stderr}')
    return result.stdout


def main():
    if not Path(OSTREE).is_file() or not (CACHE / 'flathub.trustedkeys.gpg').is_file():
        print('SKIP: real OSTree binary or signed fixture unavailable')
        return 77
    with tempfile.TemporaryDirectory(prefix='staged-tree-') as scratch:
        work = Path(scratch)
        repo = work / 'repo'
        run(OSTREE, f'--repo={repo}', 'init', '--mode=bare-user-only')
        shutil.copyfile(CACHE / 'flathub.trustedkeys.gpg', repo / 'flathub.trustedkeys.gpg')
        for key, value in [('url', f'file://{CACHE}'), ('gpg-verify', 'true')]:
            run(OSTREE, f'--repo={repo}', '--group=remote "flathub"', 'config', 'set', key, value)
        run(OSTREE, f'--repo={repo}', 'pull-local', '--untrusted', '--gpg-verify', '--remote=flathub', str(CACHE), REF)
        expected = work / 'expected'
        run(OSTREE, f'--repo={repo}', 'checkout', '--user-mode', COMMIT, str(expected))
        # Checkout is never used as a producer-owned source: each case gets
        # separate inodes, preserving the trusted expectation throughout.
        driver = work / 'driver.c'
        driver.write_text(r'''
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <linux/fsverity.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
static int test_verity_ioctl(int fd, unsigned long command, ...)
{
    (void)fd;
    va_list ap; va_start(ap, command); void *arg = va_arg(ap, void *); va_end(ap);
    if (command == FS_IOC_MEASURE_VERITY) {
        struct fsverity_digest *digest = arg;
        digest->digest_algorithm = FS_VERITY_HASH_ALG_SHA256;
        digest->digest_size = 32;
        memset(digest->digest, 0x42, 32);
    }
    return 0;
}
#define MATON_VERITY_IOCTL test_verity_ioctl
#include "install/linuxd/StagedTree.h"
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    int source = open(argv[1], O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    int target = open(argv[2], O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    int expected = open(argv[3], O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    struct stat a,b; char error[512] = {0};
    if (source < 0 || target < 0 || expected < 0 || fstat(source,&a) || fstat(expected,&b)) return 2;
    int rc = maton_staged_capture(source,target,a.st_dev,getuid(),0,error,sizeof(error));
    if (!rc) rc = maton_staged_compare(target,expected,a.st_dev,b.st_dev,0,atoi(argv[4]),error,sizeof(error));
    if (rc) fprintf(stderr,"%s\n",error);
    close(source); close(target); close(expected);
    return rc ? 1 : 0;
}
''')
        binary = work / 'driver'
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('MATON_TREE_TEST_SANITIZE') == '1' else []
        run('cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', *flags, f'-I{ROOT}', str(driver), '-o', str(binary))
        payload = Path('files/bin/gnome-calculator')
        cases = ['good', 'content', 'mode', 'mode-exec-preserved', 'directory-mode', 'type', 'missing', 'unexpected', 'metadata',
                 'metadata-type', 'path', 'hardlink', 'external-hardlink', 'setuid', 'fifo', 'socket',
                 'symlink-escape', 'symlink-chain', 'signed-link-target',
                 'extra-good', 'extra-chain-good', 'extra-chain-escape', 'extra-absolute',
                 'extra-hardlink', 'extra-fifo', 'extra-setgid', 'unauthorized-extra']
        selected = os.environ.get('MATON_TREE_TEST_CASES')
        if selected:
            requested = selected.split(',')
            assert set(requested).issubset(cases), 'unknown test case'
            cases = requested
        for name in cases:
            source, target = work / name / 'source', work / name / 'target'
            shutil.copytree(expected, source, symlinks=True)
            target.mkdir()
            item = source / payload
            inode = item.stat().st_ino
            held_socket = None
            if name.startswith('extra-') or name == 'unauthorized-extra':
                extra = source / 'files/extra'
                extra.mkdir(mode=0o700)
                data = extra / 'payload'; data.write_bytes(b'unsigned extra output'); data.chmod(0o600)
                os.setxattr(data, 'user.attacker', b'not trusted')
                if name == 'extra-chain-good':
                    (extra / 'middle').symlink_to('payload'); (extra / 'first').symlink_to('middle')
                elif name == 'extra-chain-escape':
                    (extra / 'first').symlink_to('middle/passwd'); (extra / 'middle').symlink_to('/etc')
                elif name == 'extra-absolute': (extra / 'escape').symlink_to('/etc/passwd')
                elif name == 'extra-hardlink': os.link(data, extra / 'alias')
                elif name == 'extra-fifo': os.mkfifo(extra / 'pipe')
                elif name == 'extra-setgid': data.chmod(0o2644)
            elif name == 'content':
                item.chmod(0o755); item.write_bytes(b'corrupted deployed signed payload')
            elif name == 'mode': item.chmod(0o644)
            elif name == 'mode-exec-preserved': item.chmod(0o711)
            elif name == 'directory-mode': (source / 'files/bin').chmod(0o700)
            elif name == 'type': item.unlink(); item.mkdir()
            elif name == 'missing': item.unlink()
            elif name == 'unexpected': (source / 'files/unsigned-injection').write_bytes(b'exec')
            elif name == 'metadata': (source / 'metadata').write_text('[Application]\nname=attacker\n')
            elif name == 'metadata-type': (source / 'metadata').unlink(); (source / 'metadata').symlink_to('files/bin/gnome-calculator')
            elif name == 'path': item.rename(item.with_name('renamed-calculator'))
            elif name == 'hardlink': os.link(item, source / 'files/alias')
            elif name == 'external-hardlink': os.link(item, source.parent / 'outside-alias')
            elif name == 'setuid': item.chmod(0o4755)
            elif name == 'fifo': os.mkfifo(source / 'files/injected-fifo')
            elif name == 'socket':
                held_socket = socket.socket(socket.AF_UNIX); held_socket.bind(str(source / 'files/injected-socket'))
            elif name == 'symlink-escape': (source / 'files/escape').symlink_to('../../outside')
            elif name == 'symlink-chain':
                (source / 'files/chain').symlink_to('middle/payload')
                (source / 'files/middle').symlink_to('../../outside')
            elif name == 'signed-link-target':
                links = [p for p in (source / 'files').rglob('*') if p.is_symlink()]
                if not links:
                    raise RuntimeError('fixture has no signed symlink to corrupt')
                links[0].unlink(); links[0].symlink_to('attacker-target')
            result = subprocess.run([str(binary), str(source), str(target), str(expected), '1' if name.startswith('extra-') else '0'], text=True, capture_output=True)
            if held_socket: held_socket.close()
            assert result.returncode == (0 if name in ('good', 'extra-good', 'extra-chain-good') else 1), (name, result.returncode, result.stderr)
            if name in ('good', 'extra-good', 'extra-chain-good'):
                assert (target / payload).stat().st_ino == inode, 'capture copied signed file'
            if name in ('extra-good', 'extra-chain-good'):
                assert (target / 'files/extra').stat().st_mode & 0o7777 == 0o755
                assert (target / 'files/extra/payload').stat().st_mode & 0o7777 == 0o644
                assert 'user.attacker' not in os.listxattr(target / 'files/extra/payload')
            print(f'PASS signed deployed-tree {name} (verity syscall mocked)', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
