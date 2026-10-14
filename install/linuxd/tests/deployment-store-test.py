#!/usr/bin/env python3
"""Exercise production atomic pointers, POSIX leases and collector across users."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[3]
manager=(root/'install/linuxd/FlatpakManager.c').read_text()
remove=manager[manager.index('static int remove_contents('):manager.index('/* Return nonzero while any retired version')]
collect=manager[manager.index('static int collect_deployments(void) {'):manager.index('/* Reference counts include current')]
snapshot=manager[manager.index('static int retained_refs(RuntimeRefs* db) {'):manager.index('/* Return only an installed')]
with tempfile.TemporaryDirectory(prefix='linux-app-store-') as tmp:
    work=Path(tmp);store=work/'store';store.mkdir()
    source=work/'test.c'
    source.write_text(r'''
#define _GNU_SOURCE
#define MATONOS_PUBLISH_HOST_TEST
#include "install/linuxd/LinuxStorage.h"
#include <stdarg.h>
static int fail_lease;
static int test_open(const char* path,int flags,...) {
    mode_t mode=0;
    if(flags&O_CREAT){va_list args;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);}
    if(fail_lease && strstr(path,"/.lease")){errno=EIO;return -1;}
    return openat(AT_FDCWD,path,flags,mode);
}
#define open test_open
#include "install/linuxd/DeploymentStore.h"
#include "install/linuxd/RuntimeRefs.h"
#include <assert.h>
#include <sys/wait.h>
static int live=1;
static int stub_uid_has_package(int uid){assert(uid==10120);return live;}
static int pruned;
static int prune(const char* ref,int* extension,void* opaque){(void)ref;(void)opaque;if(extension)*extension=0;else pruned++;return 0;}
''' +remove+collect+snapshot+r'''
int main(void) {
    char old[PATH_MAX],next[PATH_MAX],pointer[PATH_MAX];
    // A failure after mkdtemp must remove the partially created version.
    fail_lease=1;assert(deployment_create(10120,old,sizeof(old)));
    assert(access(old,F_OK));fail_lease=0;
    // Startup collects interrupted creation (no lease) even if PM is unknown.
    assert(!deployment_create(10120,old,sizeof(old)));
    char lease_path[PATH_MAX+8];snprintf(lease_path,sizeof(lease_path),"%s/.lease",old);
    assert(!unlink(lease_path));
    int sentinel=open(MATON_LINUX_ROOT "/../sentinel",O_CREAT|O_WRONLY|O_EXCL,0600);assert(sentinel>=0);close(sentinel);
    char nested[PATH_MAX+16];snprintf(nested,sizeof(nested),"%s/outside",old);
    assert(!symlink(MATON_LINUX_ROOT "/../sentinel",nested));live=-1;
    assert(!collect_deployments() && access(old,F_OK));
    assert(!access(MATON_LINUX_ROOT "/../sentinel",F_OK));live=1;
    // Exact six-character suffix only; never traverse a directory symlink.
    assert(!mkdir(MATON_INSTALL_ROOT "/10120-short",0755));
    assert(!symlink("/tmp",MATON_INSTALL_ROOT "/10120-ABC123"));
    assert(collect_deployments());
    assert(!access(MATON_INSTALL_ROOT "/10120-short",F_OK));
    assert(!unlink(MATON_INSTALL_ROOT "/10120-ABC123"));
    assert(!rmdir(MATON_INSTALL_ROOT "/10120-short"));
    assert(!deployment_create(10120,old,sizeof(old)));
    assert(!deployment_commit(10120,old));
    assert(!deployment_discard(old) && !access(old,F_OK));
    const char* base="runtime/org.example.Old/x86_64/1";
    const char* updated="runtime/org.example.New/x86_64/2";
    RuntimeRefs* global=runtime_refs_open(MATON_RUNTIME_DIR,0);assert(global);
    RuntimeRefs* version=runtime_refs_open(old,0);assert(version);
    assert(!runtime_refs_add(version,base,10120)&&!runtime_refs_save(version));runtime_refs_close(version);
    assert(!runtime_refs_add(global,base,10120)&&!runtime_refs_save(global));
    assert(!maton_install_dir(pointer,sizeof(pointer),210120));
    char* resolved=realpath(pointer,NULL);assert(resolved&&!strcmp(resolved,old));free(resolved);
    int ready[2],release[2];assert(!pipe(ready)&&!pipe(release));
    pid_t child=fork();assert(child>=0);
    if(!child){
        close(ready[0]);close(release[1]);
        char path[PATH_MAX+8];snprintf(path,sizeof(path),"%s/.lease",old);
        int fd=open(path,O_RDONLY);assert(fd>=0);
        struct flock lock={0};lock.l_type=F_RDLCK;lock.l_whence=SEEK_SET;
        assert(!fcntl(fd,F_SETLK,&lock));assert(write(ready[1],"x",1)==1);
        char byte;assert(read(release[0],&byte,1)==1);close(fd);_exit(0);
    }
    close(ready[1]);close(release[0]);char byte;assert(read(ready[0],&byte,1)==1);
    assert(!deployment_create(210120,next,sizeof(next)));
    assert(!deployment_commit(210120,next));
    version=runtime_refs_open(next,0);assert(version);
    assert(!runtime_refs_add(version,updated,10120)&&!runtime_refs_save(version));runtime_refs_close(version);
    assert(!runtime_refs_add(global,updated,10120)&&!runtime_refs_save(global));
    assert(!retained_refs(global)&&!runtime_refs_prune(global,prune,NULL)&&pruned==0);
    assert(collect_deployments()==1 && !access(old,F_OK) && !access(next,F_OK));
    // Unknown PackageManager state must not delete either deployment.
    live=-1;assert(collect_deployments()==1 && !access(old,F_OK) && !access(next,F_OK));
    live=1;assert(write(release[1],"x",1)==1);int status;assert(waitpid(child,&status,0)==child&&status==0);
    assert(!collect_deployments() && access(old,F_OK) && !access(next,F_OK));
    assert(!retained_refs(global)&&!runtime_refs_prune(global,prune,NULL)&&pruned==1);
    // Another user's installed stub keeps current. All-users removal deletes it.
    assert(!collect_deployments()&&!access(next,F_OK));
    live=0;assert(!collect_deployments() && access(next,F_OK) && access(pointer,F_OK));
    assert(!retained_refs(global)&&!runtime_refs_prune(global,prune,NULL)&&pruned==2);
    runtime_refs_close(global);
    // A directory outside the store cannot be committed as a pointer.
    assert(deployment_commit(10120,"/tmp/10120-evil"));
    puts("PASS atomic cross-user pointers, live-version leases, unknown PM, all-users removal and retained-version runtime refs");
}
''')
    binary=work/'test'
    subprocess.run(['cc','-Wall','-Wextra','-Werror',f'-I{root}',f'-DMATON_LINUX_ROOT="{store}"',str(source),str(root/'install/linuxd/RuntimeRefs.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
