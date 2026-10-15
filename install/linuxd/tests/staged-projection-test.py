#!/usr/bin/env python3
"""Test exact private trusted-checkout subpath projection, no producer copies."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix="maton-staged-projection-") as tmp:
    work=Path(tmp)
    code=work/'driver.c'
    code.write_text(r'''
#define _GNU_SOURCE
#include "install/linuxd/StagedProjection.h"
int main(int argc,char**argv){if(argc<2)return 2;int fd=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 2;char error[256]={0};int rc=maton_staged_project(fd,(const char*const*)argv+2,(size_t)(argc-2),error,sizeof(error));close(fd);return rc?1:0;}
''')
    binary=work/'driver'
    subprocess.run(['cc','-Wall','-Wextra','-Werror',f'-I{ROOT}',str(code),'-o',str(binary)],check=True)
    def checkout(name):
        tree=work/name
        for path in ('files/share/locale/en/a','files/share/locale/fr/b','files/bin/execute','metadata','export/share/app.desktop'):
            f=tree/path;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(path)
        return tree
    tree=checkout('valid')
    subprocess.run([str(binary),str(tree),'/share/locale/en','/bin'],check=True)
    actual={str(p.relative_to(tree)) for p in tree.rglob('*') if p.is_file()}
    assert actual=={'metadata','files/share/locale/en/a','files/bin/execute'},actual
    print('PASS: exact subpath union, ancestor directories, metadata, partial export exclusion')
    for i,path in enumerate(('/','../escape','/share/../bin','/share//locale','/share/')):
        tree=checkout(f'bad{i}')
        assert subprocess.run([str(binary),str(tree),path]).returncode!=0
    tree=checkout('symlink');(tree/'files/alias').symlink_to('share',target_is_directory=True)
    assert subprocess.run([str(binary),str(tree),'/alias/locale/en']).returncode!=0
    print('PASS: reject traversal/empty components and signed symlink intermediates')
    tree=checkout('missing')
    subprocess.run([str(binary),str(tree),'/absent'],check=True)
    assert {str(p.relative_to(tree)) for p in tree.rglob('*') if p.is_file()}=={'metadata'}
    print('PASS: missing selected paths produce an exact empty file projection')
