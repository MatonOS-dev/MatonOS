#ifndef MATON_LINUX_STORAGE_H
#define MATON_LINUX_STORAGE_H
#include "../../linux/flatpak/linux-paths.h"
#include <sys/stat.h>
/* These roots are prepared by vold, never created by linuxd. */
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
static inline int maton_storage_root(unsigned uid,int ce) {
    if(!maton_path_uid_valid(uid)){errno=EINVAL;return -1;}
    int base=open(ce?MATON_MISC_CE:MATON_MISC_DE,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(base<0)return -1;
    char user[32];snprintf(user,sizeof(user),"%u",maton_user_of(uid));
    int parent=openat(base,user,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(base);
    if(parent<0)return -1;
    int root=openat(parent,"matonos-linux",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(parent);
    if(root<0)return -1;
    struct stat st;
    if(fstat(root,&st)||st.st_uid!=1000||(st.st_mode&0777)!=0711){close(root);errno=EPERM;return -1;}
    return root;
}
static inline int maton_prepare_de(unsigned uid) {
    int root=maton_storage_root(uid,0);if(root<0)return -1;
    const char* names[]={"install","runtime","store"};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        int child=maton_storage_child(root,names[i],i==2?"u:object_r:matonos_flatpak_store_file:s0":"u:object_r:matonos_flatpak_data_file:s0",i==2?0700:0711);
        if(child<0){close(root);return -1;}
        if(i==2) {
            const char* stores[]={"apps","runtime"};
            for(size_t j=0;j<2;j++) {
                int sub=maton_storage_child(child,stores[j],"u:object_r:matonos_flatpak_store_file:s0",0700);
                if(sub<0){close(child);close(root);return -1;}close(sub);
            }
        }
        close(child);
    }
    close(root);return 0;
}
static inline int maton_prepare_install(unsigned uid,int runtime) {
    if(maton_prepare_de(uid))return -1;
    int root=maton_storage_root(uid,0);if(root<0)return -1;
    int parent=openat(root,runtime?"runtime":"install",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(root);
    if(parent<0)return -1;
    char name[32];snprintf(name,sizeof(name),"%u",uid);
    int child=maton_storage_child(parent,name,"u:object_r:matonos_flatpak_deployment_file:s0",0711);
    close(parent);if(child<0)return -1;close(child);return 0;
}
static inline int maton_home_root(unsigned uid,int create) {
    int root=maton_storage_root(uid,1);if(root<0)return -1;
    int home=create ? maton_storage_child(root,"home","u:object_r:matonos_flatpak_data_file:s0",0711) :
        openat(root,"home",O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    close(root);return home;
}
#endif
