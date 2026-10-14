#!/usr/bin/env python3
"""Shell harness regression against a local double; never invokes real adb."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='repo-harness-',dir='/tmp') as d:
    work=Path(d);fake=work/'device-double';repo=work/'repo.flatpakrepo'
    repo.write_text('[Flatpak Repo]\nTitle=X\nUrl=https://example.org/\nGPGKey=xgYEAQIDBAU=\n')
    fake.write_text(r'''#!/usr/bin/env python3
import base64,json,sys
from pathlib import Path
args=sys.argv[1:]
if 'content' not in args:sys.exit(0)
command=args[args.index('--arg')+1];encoded=args[args.index('--extra')+1].split(':',2)[2]
payload=json.loads(base64.b64decode(encoded))
if command=='install_repo':
    assert base64.b64decode(payload['file_b64'])==Path(sys.argv[0]).with_name('repo.flatpakrepo').read_bytes()
    reply={'ok':True,'accepted':True,'operationId':'op','remote':'example'}
elif command=='remove_repo':
    assert payload['remote']=='example';reply={'ok':True,'accepted':True,'operationId':'op'}
elif command=='repo_status':
    assert payload=={'operationId':'op'};reply={'ok':True,'complete':True}
elif command=='list_remotes':reply={'ok':True,'output':''}
else:raise AssertionError(command)
print('Bundle[{result='+json.dumps(reply)+'}]')
''');fake.chmod(0o755)
    env=dict(os.environ,MATON_ADB=str(fake))
    for mode,arg in (('--add-repo',str(repo)),('--remove-repo','example')):
        subprocess.run(['bash',str(root/'install/tools/test-flatpak.sh'),mode,arg],env=env,check=True,capture_output=True,text=True)
    print('PASS add/remove harness file/base64 payload, operation polling and error-free JSON quoting (local double only)')
