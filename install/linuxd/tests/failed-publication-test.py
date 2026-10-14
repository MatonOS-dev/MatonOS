#!/usr/bin/env python3
"""Exercise the real publication worker and query guard on a failed install."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='linuxd-failed-publish-',dir='/tmp') as tmp:
    work=Path(tmp);store=work/'store';store.mkdir()
    (work/'android').mkdir()
    (work/'android/log.h').write_text('''#define ANDROID_LOG_WARN 5
static inline int __android_log_print(int level,const char* tag,const char* fmt,...) {(void)level;(void)tag;(void)fmt;return 0;}
''')
    source=work/'test.c'
    source.write_text(r'''
#include "install/linuxd/FlatpakManager.c"
#include <assert.h>
static int completions;
static void completed(const FlatpakResult* r,void* context) {
    (void)context;assert(!r->ok && r->error);completions++;
}
static void fail_publish(void) {
    PackageOperation* op=calloc(1,sizeof(*op));assert(op);
    op->app_uid=10120;
    op->ref=strdup("app/org.example.App/x86_64/stable");
    op->runtime_ref=strdup("runtime/org.example.Platform/x86_64/stable");
    op->app_commit=strdup("0000000000000000000000000000000000000000000000000000000000000000");
    op->runtime_commit=strdup(op->app_commit);op->remote=strdup("flathub");
    op->operation_id=strdup("missing-staging");
    package_operation_thread(op);
}
int main(void) {
    flatpak_manager_init();g_complete_callback=completed;
    char pointer[PATH_MAX],current[PATH_MAX];
    assert(!maton_install_dir(pointer,sizeof(pointer),10120));
    const char* args[]={"--user","info","app/org.example.App/x86_64/stable"};
    ChildResult query=run_cli_in(args,3,0,MATON_RUNTIME_DIR,pointer);
    assert(query.status==1 && access(pointer,F_OK));free(query.output);
    // A numeric entry is a pointer, never a Flatpak installation directory.
    assert(!mkdir(pointer,0755));
    query=run_cli_in(args,3,0,MATON_RUNTIME_DIR,pointer);
    assert(query.status==1);free(query.output);assert(!rmdir(pointer));
    fail_publish();assert(completions==1 && access(pointer,F_OK));
    DIR* dir=opendir(MATON_INSTALL_ROOT);assert(dir);struct dirent* e;
    while((e=readdir(dir)))assert(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."));
    closedir(dir);
    // An existing published version survives a failed replacement.
    assert(!deployment_create(10120,current,sizeof(current)) && !deployment_commit(10120,current));
    fail_publish();assert(completions==2 && !access(current,F_OK));
    char* resolved=realpath(pointer,NULL);assert(resolved&&!strcmp(resolved,current));free(resolved);
    puts("PASS failed publication removes version, leaves no UID pointer, preserves previous deployment; missing query creates nothing");
}
''')
    binary=work/'test'
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
                    '-Wl,--gc-sections','-pthread',f'-I{work}',f'-I{root}',
                    f'-DMATON_LINUX_ROOT="{store}"','-DMATONOS_PUBLISH_HOST_TEST',str(source),
                    str(root/'install/linuxd/FlatpakPublish.c'),str(root/'install/linuxd/RuntimeRefs.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
