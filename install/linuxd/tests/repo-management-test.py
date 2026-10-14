#!/usr/bin/env python3
"""Production linuxd repository commands with isolated CLI/keyring doubles."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='repo-management-',dir='/tmp') as temp:
    work=Path(temp);(work/'android').mkdir()
    (work/'android/log.h').write_text('#define ANDROID_LOG_WARN 5\nstatic inline int __android_log_print(int l,const char*t,const char*f,...) {(void)l;(void)t;(void)f;return 0;}\n')
    fake=work/'cli'
    fake.write_text(r'''#!/usr/bin/env python3
import json,os,sys
from pathlib import Path
args=sys.argv[1:];root=Path(os.environ['FLATPAK_SYSTEM_DIR']) if 'FLATPAK_SYSTEM_DIR' in os.environ else Path(os.environ['ROOT'])/'runtime'
repo=root/'repo';repo.mkdir(parents=True,exist_ok=True)
state=repo/'state.json';remotes=json.loads(state.read_text()) if state.exists() else {}
if 'remotes' in args:
    for name,url in remotes.items():print(name+'\t'+url)
elif 'remote-add' in args:
    assert '--no-gpg-verify' not in args and '--from' in args
    name=args[-2];text=Path(args[-1]).read_text();fields=dict(line.split('=',1) for line in text.splitlines()[1:])
    assert fields['GPGKey'];remotes[name]=fields['Url'];(repo/(name+'.trustedkeys.gpg')).write_text(fields['GPGKey'])
    state.write_text(json.dumps(remotes))
elif 'remote-delete' in args:
    name=args[-1]
    if name not in remotes:sys.exit(1)
    del remotes[name];state.write_text(json.dumps(remotes));(repo/(name+'.trustedkeys.gpg')).unlink(missing_ok=True)
elif 'list' in args:
    app=Path(os.environ['FLATPAK_USER_DIR'])/'apps'
    if app.exists():print(app.read_text(),end='')
elif 'get' in args:
    print('false' if os.environ.get('BAD_SUMMARY') else 'true')
''');fake.chmod(0o755)
    source=work/'test.c'
    source.write_text(r'''
#include "install/linuxd/FlatpakManager.c"
#include <assert.h>
int stub_uid_has_package(int uid){(void)uid;return 0;}
int stub_user_has_package(int uid){(void)uid;return 0;}
static const char* file="[Flatpak Repo]\nTitle=Example\nUrl=https://example.org/repo/\nGPGKey=YWJjZA==\n";
static void call(const char* command,const char* data,const char* remote,int success) {
    FlatpakResult r;flatpak_manager_call(command,data,0,NULL,NULL,NULL,NULL,remote,-1,&r);
    if(r.ok!=success)fprintf(stderr,"unexpected %s: %s\n",command,r.error?r.error:"");
    assert(r.ok==success);flatpak_manager_result_clear(&r);
}
int main(void) {
    assert(!mkdir(MATON_LINUX_ROOT,0755));flatpak_manager_init();
    call("install_repo",file,"example",1);
    call("install_repo",file,"example",0); // name and URL duplicates
    call("install_repo",file,"other",0); // URL duplicate under different name
    call("install_repo","[Flatpak Repo]\nTitle=X\nUrl=https://unsigned.org/\nGPGKey=\n","unsigned",0);
    call("install_repo","[Flatpak Repo]\nTitle=X\nUrl=http://plain.org/\nGPGKey=YWJjZA==\n","plain",0);
    call("remove_repo",NULL,"flathub",0);
    call("install_repo","[Flatpak Repo]\nTitle=Other\nUrl=https://other.org/\nGPGKey=ZWZnaA==\n","other",1);
    assert(!setenv("BAD_SUMMARY","1",1));
    call("install_repo","[Flatpak Repo]\nTitle=X\nUrl=https://bad.org/\nGPGKey=YWJjZA==\n","bad",0);
    assert(!unsetenv("BAD_SUMMARY"));
    int manager=open(MATON_LINUX_ROOT "/manager",O_RDONLY|O_DIRECTORY);assert(manager>=0);
    assert(!remote_pin_commit(manager,"app/org.example.App/x86_64/stable","example"));
    assert(!remote_pin_commit(manager,"app/org.other.App/x86_64/stable","other"));
    assert(!mkdir(MATON_INSTALL_ROOT "/10120-abcdef",0755));
    assert(!symlink("10120-abcdef",MATON_INSTALL_ROOT "/10120"));
    FILE* apps=fopen(MATON_INSTALL_ROOT "/10120-abcdef/apps","w");assert(apps);fputs("org.example.App\texample\n",apps);fclose(apps);
    call("remove_repo",NULL,"example",0); // bridge must remove stubs and deployments first
    assert(!remote_pin_check(manager,"app/org.example.App/x86_64/stable","example"));
    assert(!unlink(MATON_INSTALL_ROOT "/10120")); // simulated completed existing uninstall/GC
    call("remove_repo",NULL,"example",1);
    assert(faccessat(manager,"org.example.App",F_OK,0) && errno==ENOENT);
    assert(!faccessat(manager,"org.other.App",F_OK,0));close(manager);
    assert(access(MATON_RUNTIME_DIR "/repo/example.trustedkeys.gpg",F_OK));
    assert(!access(MATON_RUNTIME_DIR "/repo/other.trustedkeys.gpg",F_OK));
    puts("PASS repo add/remove, duplicate/unsigned/HTTP/summary rejection, installed-app guard, isolated key/pin cleanup, protected flathub");
}
''')
    binary=work/'test'
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-Wno-format-truncation','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread',f'-I{work}',f'-I{root}',f'-DMATON_LINUX_ROOT="{work}/store"','-DMATONOS_PUBLISH_HOST_TEST',str(source),str(root/'install/linuxd/FlatpakPublish.c'),str(root/'install/linuxd/RuntimeRefs.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],env=dict(os.environ,CLI=str(fake),ROOT=str(work/'store'),MATONOS_FLATPAK_CLI=str(fake),MATONOS_OSTREE_CLI=str(fake)),check=True)
