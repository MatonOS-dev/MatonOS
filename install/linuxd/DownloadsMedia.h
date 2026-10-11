#ifndef MATON_DOWNLOADS_MEDIA_H
#define MATON_DOWNLOADS_MEDIA_H
#include "DownloadsGrant.h"
#include <dirent.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

static inline int maton_media_project(int directory,unsigned user) {
    struct fsxattr fsx;
    if(ioctl(directory,FS_IOC_FSGETXATTR,&fsx))
        return errno==ENOTTY || errno==EOPNOTSUPP?0:-1;
    uint64_t project=(uint64_t)user*100000+1000;
    if(project>UINT32_MAX){errno=EOVERFLOW;return -1;}
    if(fsx.fsx_projid==(uint32_t)project && (fsx.fsx_xflags&FS_XFLAG_PROJINHERIT))return 0;
    fsx.fsx_projid=(uint32_t)project;fsx.fsx_xflags|=FS_XFLAG_PROJINHERIT;
    return ioctl(directory,FS_IOC_FSSETXATTR,&fsx);
}
static inline int maton_media_created_directory(int path_fd) {
    char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",path_fd);
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return -1;
    unsigned writer=(unsigned)syscall(SYS_setfsuid,-1);
    int rc=fchown(fd,1023,1023) || fchmod(fd,02775) || maton_media_project(fd,writer/100000);
    int saved=errno;close(fd);errno=saved;return rc?-1:0;
}

/* Use a thread-local filesystem GID, never setgroups() in Binder's shared
 * process. CAP_SETGID is already needed by the existing session launcher. */
static inline int maton_media_directory_at(int root,const char* path,int create) {
    if(!maton_folder_subpath(path)){errno=EINVAL;return -1;}
    int fd=fcntl(root,F_DUPFD_CLOEXEC,200);if(fd<0)return -1;
    char* copy=strdup(path);if(!copy){close(fd);return -1;}
    char* part=copy;
    while(*part) {
        char* slash=strchr(part,'/');if(slash)*slash=0;
        int created=0;
        int next=openat(fd,part,O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(next<0 && errno==ENOENT && create) {
            if(!mkdirat(fd,part,02775))created=1;
            else if(errno!=EEXIST){int saved=errno;close(fd);fd=-1;errno=saved;break;}
            next=openat(fd,part,O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        }
        if(next>=0 && created && maton_media_created_directory(next)) {
            int saved=errno;close(next);next=-1;errno=saved;
        }
        int saved=errno;close(fd);fd=next;errno=saved;
        if(fd<0 || !slash)break;
        part=slash+1;
    }
    free(copy);
    if(fd>=0 && fd<200){int pinned=fcntl(fd,F_DUPFD_CLOEXEC,200);close(fd);fd=pinned;}
    return fd;
}

/* One directory: media_rw group with setgid so new entries
 * inherit it, and the per-user media project. Shared storage never uses the
 * stub's internal app-data project; MediaProvider refines audio/video/image
 * attribution during its scan. */
static inline int maton_media_prepare_dir(int dir,unsigned user,unsigned writer) {
    struct stat owner;
    if(fstat(dir,&owner))return -1;
    if(owner.st_uid==writer)return fchown(dir,1023,1023) || fchmod(dir,02775) ? -1 :
        maton_media_project(dir,user);
    return fchown(dir,(uid_t)-1,1023) || fchmod(dir,(owner.st_mode&07777)|S_ISGID) ? -1 :
        maton_media_project(dir,user);
}

/* Teardown: normalize pinned Download descendants, not the Android user's other
 * media. No path-based recursion, symlink traversal, or depth/count cap. */
typedef struct MatonMediaWalk {
    DIR* directory;
    struct MatonMediaWalk* parent;
    int prepared;
} MatonMediaWalk;
static inline int maton_media_walk(int source,unsigned user,unsigned writer) {
    char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",source);
    int fd=open(proc,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat root;if(fstat(fd,&root)){close(fd);return -1;}
    MatonMediaWalk* stack=calloc(1,sizeof(*stack));
    if(!stack){close(fd);return -1;}
    stack->directory=fdopendir(fd);
    if(!stack->directory){free(stack);close(fd);return -1;}
    int rc=0;
    while(stack) {
        int dir=dirfd(stack->directory);
        if(!stack->prepared) {
            if(maton_media_prepare_dir(dir,user,writer)){rc=-1;break;}
            stack->prepared=1;
        }
        errno=0;struct dirent* entry=readdir(stack->directory);
        if(!entry) {
            if(errno){rc=-1;break;}
            MatonMediaWalk* parent=stack->parent;
            closedir(stack->directory);free(stack);stack=parent;continue;
        }
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        struct stat st;
        if(fstatat(dir,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)){rc=-1;break;}
        if(st.st_dev!=root.st_dev){errno=EXDEV;rc=-1;break;}
        if(S_ISDIR(st.st_mode)) {
            int next=openat(dir,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(next<0){rc=-1;break;}
            MatonMediaWalk* child=calloc(1,sizeof(*child));
            if(!child){close(next);rc=-1;break;}
            child->directory=fdopendir(next);child->parent=stack;
            if(!child->directory){close(next);free(child);rc=-1;break;}
            stack=child;continue;
        }
        if(S_ISREG(st.st_mode) && st.st_uid==writer) {
            int file=openat(dir,entry->d_name,O_PATH|O_NOFOLLOW|O_CLOEXEC);
            struct stat pinned;
            char capability[64];snprintf(capability,sizeof(capability),"/proc/self/fd/%d",file);
            if(file<0 || fstat(file,&pinned) || !S_ISREG(pinned.st_mode) || pinned.st_uid!=writer ||
                    chown(capability,1023,1023) || chmod(capability,0664))rc=-1;
            if(file>=0)close(file);
            if(rc)break;
        }
    }
    int saved=errno;
    while(stack){MatonMediaWalk* parent=stack->parent;closedir(stack->directory);free(stack);stack=parent;}
    errno=saved;return rc;
}
static inline int maton_media_open(unsigned uid,MatonFolderRequests* folders) {
    /* All path components, including /data and media, are opened separately. */
    long original=syscall(SYS_setfsgid,1023);
    long original_uid=syscall(SYS_setfsuid,uid);
    if(syscall(SYS_setfsgid,-1)!=1023 || syscall(SYS_setfsuid,-1)!=(long)uid) {
        syscall(SYS_setfsuid,original_uid);syscall(SYS_setfsgid,original);errno=EPERM;return -1;
    }
    int rc=0,root=open("/",O_PATH|O_DIRECTORY|O_CLOEXEC);
    char user_path[64];snprintf(user_path,sizeof(user_path),"data/media/%u",uid/100000);
    int user=root<0?-1:maton_media_directory_at(root,user_path,0);
    int download=user<0?-1:maton_media_directory_at(user,maton_shared_folders[0].android,1);
    if(user>=0)close(user);
    if(root>=0)close(root);
    if(download<0)rc=-1;
    for(size_t i=0;!rc && i<folders->count;i++) {
        /* Only the granted directory itself: its contents already carry the
         * Download label from file_contexts/inheritance, and new entries
         * inherit group and project from it. Files the app creates are
         * normalized by the teardown walk, so launch time does not grow with
         * the size of the user's Downloads. */
        int fd=maton_media_directory_at(download,folders->items[i].subpath,1);
        int prepared=-1;
        if(fd>=0) {
            char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
            int dir=open(proc,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
            if(dir>=0){prepared=maton_media_prepare_dir(dir,uid/100000,uid);int saved=errno;close(dir);errno=saved;}
        }
        if(fd<0 || prepared) {
            if(fd>=0)close(fd);
            rc=-1;break;
        }
        folders->items[i].fd=fd;
    }
    if(download>=0)close(download);
    int saved=errno;syscall(SYS_setfsuid,original_uid);syscall(SYS_setfsgid,original);errno=saved;
    return rc;
}
#endif
