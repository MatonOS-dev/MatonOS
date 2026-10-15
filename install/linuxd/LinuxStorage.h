#ifndef MATON_LINUX_STORAGE_H
#define MATON_LINUX_STORAGE_H
#include "../../linux/flatpak/linux-paths.h"
#include <sys/stat.h>
/* Init prepares the encrypted root; linuxd prepares fixed children only. */
static inline int maton_storage_context(const char* label) {
#ifdef MATONOS_PUBLISH_HOST_TEST
    (void)label;return 0;
#else
    int fd=open("/proc/thread-self/attr/fscreate",O_WRONLY|O_CLOEXEC);
    if(fd<0)return -1;
    size_t n=label?strlen(label):0;
    int rc=write(fd,label?label:"",n)==(ssize_t)n?0:-1;close(fd);return rc;
#endif
}
static inline int maton_storage_child(int parent,const char* name,const char* label,mode_t mode) {
    if(maton_storage_context(label))return -1;
    int rc=mkdirat(parent,name,mode),saved=errno;
    int clear=maton_storage_context(NULL);
    if(clear || (rc && saved!=EEXIST)){errno=saved;return -1;}
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if(fd<0)return -1;
    if(fstat(fd,&st)||st.st_uid!=1000||fchmod(fd,mode)){close(fd);errno=EPERM;return -1;}
    return fd;
}
/* Init owns the encrypted root. linuxd only prepares fixed children. */
static inline int maton_storage_prepare(void) {
    int root=open(MATON_LINUX_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0)return -1;
    struct stat st;
    if(fstat(root,&st)||st.st_uid!=1000){close(root);errno=EPERM;return -1;}
    const char* names[]={"install","runtime","staging"};
    for(size_t i=0;i<3;i++) {
        /* Staging is sticky and write/traverse-only for the subsystem app,
         * which creates its own UID child without listing other users. */
        int child=maton_storage_child(root,names[i],i==2?"u:object_r:matonos_flatpak_staging_file:s0":"u:object_r:matonos_flatpak_deployment_file:s0",i==2?01733:0755);
        if(child<0){close(root);return -1;}close(child);
    }
    int manager=maton_storage_child(root,"manager","u:object_r:matonos_flatpak_deployment_file:s0",0700);
    close(root);
    if(manager<0)return -1;
    const char* scratch[]={"home","cache","run","tmp"};
    for(size_t i=0;i<4;i++) {
        int child=maton_storage_child(manager,scratch[i],"u:object_r:matonos_flatpak_deployment_file:s0",0700);
        if(child<0){close(manager);return -1;}close(child);
    }
    close(manager);return 0;
}
#endif
