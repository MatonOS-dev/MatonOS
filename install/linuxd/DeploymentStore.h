#ifndef MATON_DEPLOYMENT_STORE_H
#define MATON_DEPLOYMENT_STORE_H
#include "LinuxStorage.h"
/* All mutations hold linuxd's package-operation mutex. A Java shared FileLock
 * on .lease pins a version for the entire stub process lifetime. POSIX locks
 * are released by process death, including force-stop and crash. */
/* Only canonical mkdtemp versions belong to the unpublished collector. */
static inline int deployment_version_name(const char* name) {
    char path[PATH_MAX]; unsigned appid;
    snprintf(path,sizeof(path),MATON_INSTALL_ROOT "/%s",name);
    const char* dash=strchr(name,'-');
    return dash && strlen(dash+1)==6 && maton_parse_install_path(path,&appid);
}
static inline int deployment_empty(int fd,dev_t device) {
    DIR* entries=fdopendir(dup(fd));if(!entries)return -1;
    struct dirent* e;int rc=0;
    while((e=readdir(entries))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;
        if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){rc=-1;break;}
        if(S_ISDIR(st.st_mode)) {
            int child=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(child<0){rc=-1;break;}
            struct stat pinned;
            rc=fstat(child,&pinned)||pinned.st_dev!=device||deployment_empty(child,device);
            close(child);
            if(rc||unlinkat(fd,e->d_name,AT_REMOVEDIR)){rc=-1;break;}
        } else if(unlinkat(fd,e->d_name,0)){rc=-1;break;}
    }
    closedir(entries);return rc;
}
/* Caller holds the operation mutex, or runs before accepting requests. Never
 * remove current, even when commit's final fsync reported failure. Missing
 * leases occur when creation itself was interrupted. */
static inline int deployment_discard(const char* path) {
    const char* name=strrchr(path,'/');unsigned appid;
    if(!name || !maton_parse_install_path(path,&appid)||!deployment_version_name(++name))return -1;
    int parent=open(MATON_INSTALL_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(parent<0)return -1;
    char pointer[32],target[NAME_MAX+1];snprintf(pointer,sizeof(pointer),"%u",appid);
    ssize_t n=readlinkat(parent,pointer,target,sizeof(target)-1);
    if(n>=0){target[n]=0;if(!strcmp(target,name)){close(parent);return 0;}}
    else if(errno!=ENOENT && errno!=EINVAL){close(parent);return -1;}
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0){int rc=errno==ENOENT?0:-1;close(parent);return rc;}
    struct stat st,parent_st;
    int rc=fstat(fd,&st)||fstat(parent,&parent_st)||st.st_dev!=parent_st.st_dev||st.st_uid!=1000;
    int lease=openat(fd,".lease",O_RDWR|O_NOFOLLOW|O_CLOEXEC);
    if(lease<0 && errno!=ENOENT)rc=-1;
    if(lease>=0){struct flock lock={0};lock.l_type=F_WRLCK;lock.l_whence=SEEK_SET;if(fcntl(lease,F_SETLK,&lock))rc=-1;}
    if(!rc)rc=deployment_empty(fd,st.st_dev);
    if(!rc)rc=unlinkat(parent,name,AT_REMOVEDIR);
    if(lease>=0)close(lease);
    close(fd);close(parent);return rc;
}
static inline int deployment_create(unsigned uid,char* path,size_t size) {
    if(!maton_path_uid_valid(uid) || maton_store_prepare())return -1;
    int n=snprintf(path,size,MATON_INSTALL_ROOT "/%u-XXXXXX",maton_app_id(uid));
    if(n<0||(size_t)n>=size)return -1;
    if(maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0"))return -1;
    char* made=mkdtemp(path);int saved=errno;
    int clear=maton_storage_context(NULL);errno=saved;
    if(!made)return -1;
    if(clear)goto bad;
    if(chmod(path,0755))goto bad;
    char lease[PATH_MAX];snprintf(lease,sizeof(lease),"%s/.lease",path);
    if(maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0"))goto bad;
    int fd=open(lease,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    clear=maton_storage_context(NULL);
    if(fd<0)goto bad;
    int rc=fchmod(fd,0644)||fsync(fd)||clear;close(fd);if(!rc)return 0;
bad:
    deployment_discard(path);return -1;
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
    if(rc||clear){unlink(temporary);return -1;}
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
