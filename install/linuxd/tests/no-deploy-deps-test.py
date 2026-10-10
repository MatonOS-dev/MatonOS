#!/usr/bin/env python3
"""Local, unsigned fixture testing dependency selection, not GPG verification."""
from pathlib import Path
import subprocess,os
import tempfile
workspace = tempfile.TemporaryDirectory(prefix='flatpak-no-deploy-')
r=Path(workspace.name)
def run(*a,env=None):
 p=subprocess.run(['flatpak',*a],env=env,text=True,capture_output=True);print(p.stdout,p.stderr);p.check_returncode()
for name,meta in [('Base','[Runtime]\nname=org.example.Base\n'),('Runtime','[Runtime]\nname=org.example.Runtime\n[Extension org.example.Codec]\nversion=stable\n[Extension org.freedesktop.Platform.GL]\nversion=stable\nsubdirectories=true\nno-autodownload=true\ndownload-if=active-gl-driver\n[Extension org.freedesktop.Platform.Compat.i386]\nversion=stable\n'),('freedesktop.Platform.GL.default','[Runtime]\nname=org.freedesktop.Platform.GL.default\n[ExtensionOf]\nref=runtime/org.example.Runtime/x86_64/stable\n'),('freedesktop.Platform.Compat.i386','[Runtime]\nname=org.freedesktop.Platform.Compat.i386\n[ExtensionOf]\nref=runtime/org.example.Runtime/x86_64/stable\n'),('Codec','[Runtime]\nname=org.example.Codec\n[ExtensionOf]\nref=runtime/org.example.Runtime/x86_64/stable\n'),('Test.Plugin','[Runtime]\nname=org.example.Test.Plugin\n[ExtensionOf]\nref=app/org.example.Test/x86_64/stable\n'),('Test','[Application]\nname=org.example.Test\nruntime=org.example.Runtime/x86_64/stable\nsdk=org.example.Runtime/x86_64/stable\nbase=org.example.Base\nbase-version=stable\n[Extension org.example.Test.Plugin]\nversion=stable\n')]:
 d=r/name;(d/'files').mkdir(parents=True,exist_ok=True);(d/'metadata').write_text(meta)
 if name=='Test': run('build-finish',str(d))
 run('build-export','--disable-sandbox','--files=files',*(['--runtime'] if name!='Test' else []),str(r/'repo'),str(d),'stable')
env=dict(os.environ,HOME=str(r/'home'),XDG_DATA_HOME=str(r/'data'),XDG_CACHE_HOME=str(r/'cache'))
run('--user','remote-add','--no-gpg-verify','fixture',str(r/'repo'),env=env)
run('install','--user','--no-deploy','--noninteractive','--assumeyes','fixture','org.example.Test',env=env)
pulled = {str(p.relative_to(r/'data/flatpak/repo/refs/remotes/fixture')) for p in (r/'data/flatpak/repo/refs/remotes/fixture').glob('*/*/*/*')}
expected = {'app/org.example.Test/x86_64/stable', 'runtime/org.example.Runtime/x86_64/stable',
            'runtime/org.example.Codec/x86_64/stable', 'runtime/org.example.Test.Plugin/x86_64/stable',
            'runtime/org.freedesktop.Platform.GL.default/x86_64/stable',
            'runtime/org.freedesktop.Platform.Compat.i386/x86_64/stable'}
assert pulled == expected, pulled
print('PASS: real --no-deploy pulls runtime, app/runtime extensions, GL and compat from app-only install')
print('NOTE: base= is build-time metadata; Flatpak does not pull the named base')
