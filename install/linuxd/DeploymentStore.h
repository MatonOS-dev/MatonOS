#ifndef MATON_DEPLOYMENT_STORE_H
#define MATON_DEPLOYMENT_STORE_H
#include "LinuxStorage.h"
/* All mutations hold linuxd's package-operation mutex. A Java shared FileLock
 * on .lease pins a version for the entire stub process lifetime. POSIX locks
 * are released by process death, including force-stop and crash. */
static inline int deployment_create(unsigned uid,char* path,size_t size) {
    if(!maton_path_uid_valid(uid) || maton_store_prepare())return -1;
    int n=snprintf(path,size,MATON_INSTALL_ROOT "/%u-XXXXXX",maton_app_id(uid));
    if(n<0||(size_t)n>=size)return -1;
    if(maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0"))return -1;
    char* made=mkdtemp(path);int saved=errno;
    int clear=maton_storage_context(NULL);errno=saved;
    if(!made || clear)return -1;
    if(chmod(path,0755))return -1;
    char lease[PATH_MAX];snprintf(lease,sizeof(lease),"%s/.lease",path);
    if(maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0"))return -1;
    int fd=open(lease,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    clear=maton_storage_context(NULL);
    if(fd<0)return -1;
    int rc=fchmod(fd,0644)||fsync(fd)||clear;close(fd);return rc?-1:0;
}
static inline int deployment_commit(unsigned uid,const char* version) {
    unsigned appid;
    if(!maton_path_uid_valid(uid)||!maton_parse_install_path(version,&appid)||appid!=maton_app_id(uid)||!strchr(strrchr(version,'/')+1,'-'))return -1;
    char current[PATH_MAX],temporary[PATH_MAX+5];
    maton_install_dir(current,sizeof(current),uid);
    snprintf(temporary,sizeof(temporary),"%s.new",current);
    const char* name=strrchr(version,'/')+1;
    if(unlink(temporary) && errno!=ENOENT)return -1;
    if(maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0"))return -1;
    int rc=symlink(name,temporary),saved=errno;
    int clear=maton_storage_context(NULL);errno=saved;
    if(rc||clear)return -1;
    if(rename(temporary,current)){unlink(temporary);return -1;}
    int fd=open(MATON_INSTALL_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    rc=fsync(fd);close(fd);return rc;
}
/* Open read/write only as linuxd. Never delete a version with a shared lease.
 * A pending launcher locks then rechecks current, preventing the swap race. */
static inline int deployment_exclusive(int directory) {
    int fd=openat(directory,".lease",O_RDWR|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    struct flock lock={0};lock.l_type=F_WRLCK;lock.l_whence=SEEK_SET;
    if(fcntl(fd,F_SETLK,&lock)){close(fd);return -1;}return fd;
}
#endif
