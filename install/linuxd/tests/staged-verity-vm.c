#define _GNU_SOURCE
/* Disposable initramfs test. Never run as PID1 on the host. Compile static,
 * boot with a scratch ext4 -O verity disk as virtio vda; no Android required. */
#include "../StagedVerity.h"
#include <assert.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <stdlib.h>

static void regression(void) {
    int dir=open("/data/test",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(dir>=0);
    struct stat st;assert(!fstat(dir,&st));char error[128];
    int writer=openat(dir,"payload",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0644);
    assert(writer>=0 && write(writer,"verified code\n",14)==14);
    int sealed=maton_verity_openat(dir,"payload",st.st_dev,getuid(),error,sizeof(error));
    assert(sealed<0 && errno==ETXTBSY);
    assert(!strcmp(error,"staged file still open for writing"));
    char* mapping=mmap(NULL,14,PROT_READ|PROT_WRITE,MAP_SHARED,writer,0);
    assert(mapping!=MAP_FAILED);assert(!close(writer));
    assert(!fchmodat(dir,"payload",0444,0));
    sealed=maton_verity_openat(dir,"payload",st.st_dev,getuid(),error,sizeof(error));
    assert(sealed<0 && errno==ETXTBSY);
    assert(!strcmp(error,"staged file still open for writing"));
    struct stat absent;assert(fstatat(dir,"published",&absent,0)<0 && errno==ENOENT);
    memcpy(mapping,"modified code\n",14);assert(!msync(mapping,14,MS_SYNC));
    assert(!munmap(mapping,14));
    sealed=maton_verity_openat(dir,"payload",st.st_dev,getuid(),error,sizeof(error));
    if(sealed<0)fprintf(stderr,"seal after release: %d %s\n",errno,error);
    assert(sealed>=0);
    char bytes[14];assert(read(sealed,bytes,14)==14 && !memcmp(bytes,"modified code\n",14));close(sealed);
    assert(!fstatat(dir,"payload",&st,0) && (st.st_mode&0777)==0444);
    assert(!fchmodat(dir,"payload",0644,0));
    errno=0;assert(openat(dir,"payload",O_WRONLY)<0 && errno==EPERM);
    /* Existing seals must still measure successfully. */
    sealed=maton_verity_openat(dir,"payload",st.st_dev,getuid(),error,sizeof(error));
    assert(sealed>=0);close(sealed);close(dir);
    puts("PASS REAL VERITY: retained writable fd and mapping rejected ETXTBSY");
    puts("PASS REAL VERITY: release permits measured seal; reopening writer denied EPERM");
    fflush(stdout);
}
int main(void) {
    if(getpid()!=1){fprintf(stderr,"initramfs PID1 test only\n");return 2;}
    mkdir("/dev",0755);mkdir("/proc",0755);mkdir("/sys",0755);mkdir("/data",0755);
    mount("devtmpfs","/dev","devtmpfs",0,NULL);
    mount("proc","/proc","proc",0,NULL);mount("sysfs","/sys","sysfs",0,NULL);
    int mounted=-1;
    for(int attempt=0;attempt<100;attempt++) {
        mounted=mount("/dev/vda","/data","ext4",MS_NODEV|MS_NOSUID,NULL);
        if(!mounted)break;
        usleep(100000);
    }
    if(mounted){perror("mount scratch verity disk");reboot(RB_POWER_OFF);return 1;}
    assert(!mkdir("/data/test",0755));assert(!chown("/data/test",1000,1000));
    pid_t child=fork();assert(child>=0);
    if(!child){assert(!setgid(1000));assert(!setuid(1000));regression();_exit(0);}
    int status=0;assert(waitpid(child,&status,0)==child);
    puts(WIFEXITED(status)&&WEXITSTATUS(status)==0?"VERITY VM RESULT PASS":"VERITY VM RESULT FAIL");
    fflush(stdout);sync();reboot(RB_POWER_OFF);return 0;
}
