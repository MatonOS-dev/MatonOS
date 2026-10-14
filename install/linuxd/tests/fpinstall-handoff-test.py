#!/usr/bin/env python3
"""Real C staging/publication/binding worker with deterministic CLI failures.

The CLI double models signed refs; real crypto/checksum regressions live in
flatpak-publish-host-test.sh. Android UID entry/PM require a fresh image.
"""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='fpinstall-handoff-', dir='/tmp') as temp:
    work=Path(temp);(work/'android').mkdir()
    (work/'android/log.h').write_text('#define ANDROID_LOG_WARN 5\nstatic inline int __android_log_print(int l,const char*t,const char*f,...) {(void)l;(void)t;(void)f;return 0;}\n')
    fake=work/'cli'
    fake.write_text(r'''#!/usr/bin/env python3
from pathlib import Path
import os,sys
args=sys.argv[1:]; scenario=os.environ['SCENARIO']
app='app/org.example.App/x86_64/stable'; runtime='runtime/org.example.Platform/x86_64/stable'
repo=next((a[7:] for a in args if a.startswith('--repo=')),None)
if 'init' in args:
    Path(repo).mkdir(parents=True,exist_ok=True);(Path(repo)/'config').write_text('[core]\n')
elif 'get' in args and 'config' in args: print('true')
elif 'rev-parse' in args: print(('a' if 'app/' in args[-1] else 'b')*64)
elif 'refs' in args and not any(a.startswith('--create=') for a in args):
    for ref in (app,runtime): print('flathub:'+ref)
elif 'cat' in args:
    if scenario=='resolve': sys.exit(1)
    print('[Application]\nname=org.example.App\nruntime=org.example.Platform/x86_64/stable' if args[args.index('cat')+1][0]=='a' else '[Runtime]\nname=org.example.Platform')
elif 'pull-local' in args:
    if scenario=='signature': sys.exit(1)
elif 'install' in args:
    if '--no-deploy' in args:
        if scenario=='stage': sys.exit(1)
    elif scenario=='deploy': sys.exit(1)
elif '--show-location' in args:
    isapp='--user' in args
    location=Path(os.environ['FLATPAK_USER_DIR' if isapp else 'FLATPAK_SYSTEM_DIR'])/('app' if isapp else 'runtime')/('org.example.App' if isapp else 'org.example.Platform')/'x86_64'/'stable'/('a' if isapp else 'b')
    location.mkdir(parents=True,exist_ok=True)
    (location/'files').mkdir(exist_ok=True)
    if isapp:
        desktop=location/'export/share/applications/org.example.App.desktop'
        desktop.parent.mkdir(parents=True,exist_ok=True)
        desktop.write_text('[Desktop Entry]\nName=Verified name\nType=Application\n')
        if scenario=='sanitize-link' and not (location/'escape').is_symlink(): (location/'escape').symlink_to('/tmp')
        if scenario=='sanitize-relative' and not (location/'escape').is_symlink(): (location/'escape').symlink_to('../../../../tmp')
        if scenario=='sanitize-device' and not (location/'pipe').exists(): os.mkfifo(location/'pipe')
        if scenario=='sanitize-mode': (location/'files').chmod(0o4755)
        if scenario=='sanitize-xattr': os.setxattr(location/'files','user.bad',b'bad')
    print(location)
elif '--show-metadata' in args:
    if scenario=='metadata' and args[-1]==app: sys.exit(1)
    print('[Application]\nname=org.example.App\nruntime=org.example.Platform/x86_64/stable\n[Context]\ndevices=all;')
''')
    fake.chmod(0o755)
    source=work/'test.c'
    source.write_text(r'''
#include "install/linuxd/FlatpakManager.c"
#include <assert.h>
static const char* app="app/org.example.App/x86_64/stable";
int stub_uid_has_package(int uid){(void)uid;return 1;}
int stub_user_has_package(int uid){(void)uid;return 1;}
static void call(const char* command,const char* operation,int uid,FlatpakResult* out) {
    flatpak_manager_handoff(command,app,"flathub",operation,uid,out);
}
static void clear(FlatpakResult* r){flatpak_manager_result_clear(r);}
static void empty_install(void) {
    DIR* dir=opendir(MATON_INSTALL_ROOT);assert(dir);struct dirent* e;
    while((e=readdir(dir)))assert(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."));
    closedir(dir);
}
int main(int argc,char** argv) {
    assert(argc==2);const char* scenario=argv[1];
    assert(!mkdir(MATON_LINUX_ROOT,0755));flatpak_manager_init();
    char repo[PATH_MAX];snprintf(repo,sizeof(repo),MATON_RUNTIME_DIR "/repo");assert(!mkdir(repo,0755));
    char path[PATH_MAX];snprintf(path,sizeof(path),"%s/config",repo);
    FILE* config=fopen(path,"w");assert(config);fputs("[remote \"flathub\"]\ngpg-verify=true\ngpg-verify-summary=true\n",config);fclose(config);
    snprintf(path,sizeof(path),"%s/flathub.trustedkeys.gpg",repo);
    FILE* key=fopen(path,"w");assert(key);fputs("linuxd keyring",key);fclose(key);
    FlatpakResult r;call("deploy_pending","op",-1,&r);
    int early=!strcmp(scenario,"stage")||!strcmp(scenario,"resolve")||!strcmp(scenario,"signature")||!strcmp(scenario,"deploy")||!strncmp(scenario,"sanitize-",9);
    if(early){assert(!r.ok);clear(&r);assert(!pending_install.operation[0]);empty_install();return 0;}
    assert(r.ok);clear(&r);
    assert(pending_deployment_path(pending_install.path));
    char pointer[PATH_MAX];assert(!maton_install_dir(pointer,sizeof(pointer),10120));assert(access(pointer,F_OK));
    // Operation/ref authority is required even before a UID exists.
    call("abort_pending","wrong-op",-1,&r);assert(!r.ok);clear(&r);
    call("pending_metadata","op",-1,&r);
    if(!strcmp(scenario,"metadata"))assert(!r.ok);else assert(r.ok&&strstr(r.output,"devices=all"));clear(&r);
    call("pending_desktop","op",-1,&r);assert(r.ok&&strstr(r.output,"Verified name"));clear(&r);
    if(!strcmp(scenario,"bridge-crash")) {flatpak_manager_abort_handoffs();assert(!pending_install.operation[0]);empty_install();return 0;}
    if(!strcmp(scenario,"crash")) {pending_clear();flatpak_manager_init();empty_install();return 0;}
    if(!strcmp(scenario,"pm")||!strcmp(scenario,"metadata")||!strcmp(scenario,"bind")) {
        if(!strcmp(scenario,"bind")){call("bind_pending","op",2902,&r);assert(!r.ok);clear(&r);}
        call("abort_pending","op",-1,&r);assert(r.ok);clear(&r);empty_install();return 0;
    }
    call("bind_pending","op",10120,&r);if(!r.ok)fprintf(stderr,"bind error: %s\n",r.error);assert(r.ok);clear(&r);
    assert(!pending_install.operation[0]);char original[PATH_MAX];assert(realpath(pointer,original));
    assert(!access(original,F_OK));
    // Update failure keeps the previous pointer and deployment.
    assert(!setenv("SCENARIO","signature",1));
    call("deploy_pending","update",-1,&r);assert(!r.ok);clear(&r);
    char current[PATH_MAX];assert(realpath(pointer,current)&&!strcmp(current,original));
    // Duplicate binding cannot switch another UID to this deployment.
    call("bind_pending","op",10121,&r);assert(!r.ok);clear(&r);
    puts("PASS verified deployment binds once; failed update preserves current");
}
''')
    binary=work/'test'
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-Wno-format-truncation','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-pthread',f'-I{work}',f'-I{root}',f'-DMATON_LINUX_ROOT="{work}/store"','-DMATONOS_PUBLISH_HOST_TEST',str(source),str(root/'install/linuxd/FlatpakPublish.c'),str(root/'install/linuxd/RuntimeRefs.c'),'-o',str(binary)],check=True)
    import shutil
    for scenario in ('stage','resolve','signature','deploy','sanitize-link','sanitize-relative','sanitize-device','sanitize-mode','sanitize-xattr','metadata','pm','bind','crash','bridge-crash','success'):
        env=dict(os.environ,SCENARIO=scenario,MATONOS_FLATPAK_CLI=str(fake),MATONOS_OSTREE_CLI=str(fake))
        subprocess.run([str(binary),scenario],env=env,check=True)
        shutil.rmtree(work/'store')
        print('PASS handoff:',scenario,flush=True)
