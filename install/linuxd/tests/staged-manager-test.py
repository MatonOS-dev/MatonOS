#!/usr/bin/env python3
"""Execute production manager state/binding with mock staging/verification only.

Real RuntimeRefs and pending/deployment storage run on a temporary store. This
covers orchestration, not backend signature verification, verity, or Binder.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix="staged-manager-") as temp:
    work = Path(temp)
    (work / "android").mkdir()
    (work / "android/log.h").write_text(
        '#define ANDROID_LOG_WARN 5\n'
        'static inline int __android_log_print(int l,const char*t,const char*f,...)'
        '{(void)l;(void)t;(void)f;return 0;}\n')
    source = work / "manager-test.c"
    source.write_text(r'''
#include "install/linuxd/FlatpakManager.c"
#include <assert.h>
static const char* app="app/org.example.App/x86_64/stable";
static const char* runtime="runtime/org.example.Platform/x86_64/stable";
static int begin_calls,verify_calls,remove_calls,startup_failure,begin_failure,verify_failure,cleanup_failure;
static int concurrent_probe;
int stub_uid_has_package(int uid){(void)uid;return 1;}
int stub_user_has_package(int uid){(void)uid;return 1;}
int flatpak_cleanup_stale_staging(void){return startup_failure?-1:0;}
int flatpak_remote_verified(const char* remote){return !strcmp(remote,"flathub");}
int flatpak_staging_dir(int uid,const char* op,char* path,unsigned long size) {
    return snprintf(path,size,MATON_STAGING_ROOT "/%d/%s",uid,op)<(int)size?0:-1;
}
static void make_dir(const char* path){assert(!mkdir(path,0700)||errno==EEXIST);}
int flatpak_begin_staged_install(int uid,const char* op,const char* remote,char* error,unsigned long size) {
    (void)remote;(void)error;(void)size;begin_calls++;
    char parent[PATH_MAX],path[PATH_MAX];snprintf(parent,sizeof(parent),MATON_STAGING_ROOT "/%d",uid);make_dir(parent);
    assert(!flatpak_staging_dir(uid,op,path,sizeof(path)));make_dir(path);
    snprintf(path,sizeof(path),"%s/.trust-%s",parent,op);make_dir(path);
    snprintf(path,sizeof(path),"%s/.capture-%s",parent,op);make_dir(path);
    return begin_failure?-1:0;
}
int flatpak_remove_staging(int uid,const char* op) {
    remove_calls++;if(cleanup_failure)return -1;
    char parent[PATH_MAX],name[96];snprintf(parent,sizeof(parent),MATON_STAGING_ROOT "/%d",uid);
    int fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return errno==ENOENT?0:-1;
    const char* prefixes[]={"",".trust-",".capture-"};int rc=0;
    for(size_t i=0;i<3;i++) {
        snprintf(name,sizeof(name),"%s%s",prefixes[i],op);
        int child=openat(fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(child<0){if(errno!=ENOENT)rc=-1;continue;}
        struct stat st;assert(!fstat(child,&st));if(remove_contents(child,st.st_dev))rc=-1;
        close(child);if(unlinkat(fd,name,AT_REMOVEDIR))rc=-1;
    }
    close(fd);return rc;
}
int flatpak_verify_publish_staged_tracked(const struct FlatpakStagedManifest* manifest,
        const char* staging,const char* system,const char* pending,char* error,unsigned long size,
        int(*record)(const char*,void*),int(*complete)(const char*const*,size_t,void*),void* context) {
    (void)staging;(void)system;verify_calls++;
    if(concurrent_probe) {
        FlatpakResult probe;
        flatpak_manager_staged("abort_staged_install",manifest->operation_id,NULL,NULL,-1,NULL,&probe);
        assert(!probe.ok);flatpak_manager_result_clear(&probe);
        flatpak_manager_staged("verify_staged_install",manifest->operation_id,NULL,NULL,-1,manifest,&probe);
        assert(!probe.ok);flatpak_manager_result_clear(&probe);
    }
    assert(!record(runtime,context));const char* refs[]={runtime};assert(!complete(refs,1,context));
    char path[PATH_MAX];snprintf(path,sizeof(path),"%s/verified-marker",pending);
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);assert(fd>=0);assert(write(fd,"verified",8)==8);close(fd);
    if(verify_failure){snprintf(error,size,"injected verification failure");return -1;}
    return 0;
}
/* Legacy work must remain unreachable from production manager dispatch. */
int flatpak_publish_tracked(const char* a,const char* b,const char* c,const char* d,
        const char* e,const char* f,const char* g,const char* h,char* error,unsigned long size,
        int(*record)(const char*,void*),int(*complete)(const char*const*,size_t,void*),void* context) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)error;(void)size;
    (void)record;(void)complete;(void)context;assert(!"legacy publish invoked");return -1;
}
int flatpak_available_commit(const char* a,const char* b,const char* c,char* d,unsigned long e,char* f,unsigned long g) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;assert(!"unexpected network lookup");return -1;
}
static struct FlatpakStagedManifest manifest;
static void setup_manifest(const char* op) {
    memset(&manifest,0,sizeof(manifest));manifest.version=1;
    strcpy(manifest.operation_id,op);strcpy(manifest.app_ref,app);strcpy(manifest.remote,"flathub");
    memset(manifest.app_commit,'a',64);
}
static void staged(const char* command,const char* op,int uid,int expected) {
    FlatpakResult r;flatpak_manager_staged(command,op,app,"flathub",uid,&manifest,&r);
    if(!!r.ok!=expected)fprintf(stderr,"%s %s expected %d, error=%s\n",command,op,expected,r.error?r.error:"");
    assert(!!r.ok==expected);flatpak_manager_result_clear(&r);
}
static void handoff(const char* command,const char* op,int uid,int expected) {
    FlatpakResult r;flatpak_manager_handoff(command,app,"flathub",op,uid,&r);
    assert(!!r.ok==expected);flatpak_manager_result_clear(&r);
}
static void pointer_target(char* target) {
    char pointer[PATH_MAX];assert(!maton_install_dir(pointer,sizeof(pointer),110120));assert(realpath(pointer,target));
}
static void assert_scratch_absent(int uid,const char* op) {
    char path[PATH_MAX];assert(!flatpak_staging_dir(uid,op,path,sizeof(path)));assert(access(path,F_OK));
    snprintf(path,sizeof(path),MATON_STAGING_ROOT "/%d/.trust-%s",uid,op);assert(access(path,F_OK));
    snprintf(path,sizeof(path),MATON_STAGING_ROOT "/%d/.capture-%s",uid,op);assert(access(path,F_OK));
}
int main(void) {
    make_dir(MATON_LINUX_ROOT);flatpak_manager_init();assert(g_staging_ready);
    FlatpakResult legacy;flatpak_manager_call("install",app,0,NULL,NULL,NULL,NULL,"flathub",110120,&legacy);
    assert(!legacy.ok);flatpak_manager_result_clear(&legacy);handoff("deploy_pending","old",-1,0);
    setup_manifest("first");staged("begin_staged_install","first",2902,0);
    staged("begin_staged_install","first",20000,0);assert(begin_calls==0);
    staged("begin_staged_install","first",110000,1);assert(begin_calls==1);
    staged("begin_staged_install","first",110000,0);staged("begin_staged_install","other",110000,0);
    staged("verify_staged_install","unknown",-1,0);assert(verify_calls==0);
    strcpy(manifest.remote,"other");staged("verify_staged_install","first",-1,0);strcpy(manifest.remote,"flathub");
    handoff("pending_metadata","first",-1,0);assert(flatpak_manager_complete_handoff("first",app,"flathub"));
    concurrent_probe=1;staged("verify_staged_install","first",-1,1);assert(verify_calls==1);
    assert_scratch_absent(110000,"first");staged("verify_staged_install","first",-1,0);
    char pointer[PATH_MAX];assert(!maton_install_dir(pointer,sizeof(pointer),110120));assert(access(pointer,F_OK));
    handoff("bind_pending","first",10120,0);handoff("bind_pending","first",110120,1);
    assert(flatpak_manager_complete_handoff("first",app,"wrong"));
    assert(flatpak_manager_complete_handoff("first","app/org.other.App/x86_64/stable","flathub"));
    assert(!flatpak_manager_complete_handoff("first",app,"flathub"));assert(flatpak_manager_complete_handoff("first",app,"flathub"));
    char original[PATH_MAX],selected[PATH_MAX];pointer_target(original);
    staged("begin_staged_install","first",110000,0);handoff("bind_pending","first",110121,0);
    puts("PASS registration, replay, concurrent completion/abort, verified bind, same-user UID, completion receipt");
    setup_manifest("bad-update");staged("begin_staged_install","bad-update",110000,1);
    verify_failure=1;staged("verify_staged_install","bad-update",-1,0);verify_failure=0;
    pointer_target(selected);assert(!strcmp(selected,original));assert(!pending_install.operation[0]);assert_scratch_absent(110000,"bad-update");
    puts("PASS failed verification keeps committed pointer and collects scratch");
    setup_manifest("abort");staged("begin_staged_install","abort",110000,1);staged("abort_staged_install","abort",-1,1);
    assert_scratch_absent(110000,"abort");staged("abort_staged_install","abort",-1,0);staged("begin_staged_install","abort",110000,0);
    setup_manifest("dead-provider");staged("begin_staged_install","dead-provider",110000,1);
    staged("verify_staged_install","dead-provider",-1,1);cleanup_failure=1;flatpak_manager_abort_handoffs();
    assert(pending_install.state==HANDOFF_FAILED);handoff("bind_pending","dead-provider",110120,0);
    cleanup_failure=0;staged("abort_staged_install","dead-provider",-1,1);pointer_target(selected);assert(!strcmp(selected,original));
    puts("PASS abort and provider death collect siblings; failed cleanup cannot bind");
    setup_manifest("bad-begin");begin_failure=1;cleanup_failure=1;staged("begin_staged_install","bad-begin",110000,0);
    assert(pending_install.state==HANDOFF_FAILED);begin_failure=0;cleanup_failure=0;staged("abort_staged_install","bad-begin",-1,1);
    assert_scratch_absent(110000,"bad-begin");
    setup_manifest("failed-cleanup");staged("begin_staged_install","failed-cleanup",110000,1);
    cleanup_failure=1;staged("verify_staged_install","failed-cleanup",-1,0);handoff("bind_pending","failed-cleanup",110120,0);
    cleanup_failure=0;staged("abort_staged_install","failed-cleanup",-1,1);
    startup_failure=1;flatpak_manager_init();assert(!g_staging_ready);setup_manifest("blocked");staged("begin_staged_install","blocked",110000,0);
    startup_failure=0;flatpak_manager_init();setup_manifest("allowed");staged("begin_staged_install","allowed",110000,1);staged("abort_staged_install","allowed",-1,1);
    puts("PASS failed begin/cleanup retained for abort; failed startup cleanup blocks registration");
    puts("PASS production manager lifecycle with mock verifier (no crypto/verity/Binder claim)");
}
''')
    binary = work / "manager-test"
    subprocess.run([
        "cc", "-Wall", "-Wextra", "-Werror", "-Wno-format-truncation",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-pthread",
        f"-I{work}", f"-I{root}", f'-DMATON_LINUX_ROOT="{work}/store"',
        "-DMATONOS_PUBLISH_HOST_TEST", str(source),
        str(root / "install/linuxd/RuntimeRefs.c"), "-o", str(binary)
    ], check=True)
    subprocess.run([str(binary)], check=True, env=os.environ)
