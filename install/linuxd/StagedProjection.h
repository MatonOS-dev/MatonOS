#ifndef MATON_STAGED_PROJECTION_H
#define MATON_STAGED_PROJECTION_H
/* Project only a trusted, private OSTree comparison checkout. Every actual
 * staged entry is subsequently compared against this exact projection. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int sp_remove(int parent,const char* name,dev_t device,unsigned depth) {
    struct stat st;
    if(depth>128||fstatat(parent,name,&st,AT_SYMLINK_NOFOLLOW)||st.st_dev!=device)return -1;
    if(!S_ISDIR(st.st_mode))return unlinkat(parent,name,0);
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    DIR* d=fdopendir(fd);if(!d){close(fd);return -1;}
    struct dirent* e;int rc=0;
    while(!rc&&(e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,".."))
        rc=sp_remove(fd,e->d_name,device,depth+1);
    closedir(d);return rc?rc:unlinkat(parent,name,AT_REMOVEDIR);
}
static inline int sp_valid(const char* path) {
    if(!path||!*path||strlen(path)>=256)return 0;
    if(*path=='/')path++;
    if(!*path)return 0;
    while(*path) {
        const char* slash=strchr(path,'/');size_t n=slash?(size_t)(slash-path):strlen(path);
        if(!n||(n==1&&path[0]=='.')||(n==2&&path[0]=='.'&&path[1]=='.'))return 0;
        for(size_t i=0;i<n;i++)if((unsigned char)path[i]<32||(unsigned char)path[i]==127)return 0;
        if(!slash)break;
        path=slash+1;if(!*path)return 0;
    }
    return 1;
}
/* 2=selected entire subtree, 1=ancestor of selection, 0=not selected. */
static inline int sp_selected(const char* path,const char*const* paths,size_t count) {
    int keep=0;size_t n=strlen(path);
    for(size_t i=0;i<count;i++) {
        const char* select=paths[i]+(paths[i][0]=='/');size_t m=strlen(select);
        if(n==m&&!strcmp(path,select))return 2;
        if(n>m&&!strncmp(path,select,m)&&path[m]=='/')return 2;
        if(m>n&&!strncmp(select,path,n)&&select[n]=='/')keep=1;
    }
    return keep;
}
static inline int sp_prune(int dir,const char* prefix,const char*const* paths,size_t count,
        dev_t device,unsigned depth) {
    if(depth>128)return -1;
    int scan=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(scan<0)return -1;
    DIR* d=fdopendir(scan);if(!d){close(scan);return -1;}
    struct dirent* e;int rc=0;
    while(!rc&&(e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char relative[4096];struct stat st;
        if(snprintf(relative,sizeof(relative),"%s%s%s",prefix,*prefix?"/":"",e->d_name)>=(int)sizeof(relative)||
                fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)||st.st_dev!=device){rc=-1;break;}
        int selected=sp_selected(relative,paths,count);
        if(!selected)rc=sp_remove(dir,e->d_name,device,depth);
        else if(selected==1) {
            if(!S_ISDIR(st.st_mode)){rc=-1;break;}
            int child=openat(dir,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(child<0){rc=-1;break;}rc=sp_prune(child,relative,paths,count,device,depth+1);close(child);
        }
    }
    closedir(d);return rc;
}
static inline int maton_staged_project(int expected,const char*const* paths,size_t count,
        char* error,size_t size) {
    if(!count)return 0;
    int rc=-1,files=-1;struct stat st;
    if(count>32||!paths||fstat(expected,&st))goto done;
    for(size_t i=0;i<count;i++)if(!sp_valid(paths[i]))goto done;
    files=openat(expected,"files",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(files<0)goto done;
    /* Reject selections traversing a signed symlink. Missing selected paths
     * are allowed by stock Flatpak (e.g. locale not present in this commit). */
    for(size_t i=0;i<count;i++) {
        char copy[256];strcpy(copy,paths[i]+(paths[i][0]=='/'));
        int at=dup(files);if(at<0)goto done;
        char* save=NULL;char* token=strtok_r(copy,"/",&save);
        while(token) {
            char* next=strtok_r(NULL,"/",&save);
            struct stat node;
            if(fstatat(at,token,&node,AT_SYMLINK_NOFOLLOW)) {
                if(errno!=ENOENT){close(at);goto done;}
                break;
            }
            if(node.st_dev!=st.st_dev){close(at);goto done;}
            if(next) {
                if(!S_ISDIR(node.st_mode)){close(at);goto done;}
                int child=openat(at,token,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                close(at);at=child;if(at<0)goto done;
            }
            token=next;
        }
        close(at);
    }
    if(sp_prune(files,"",paths,count,st.st_dev,0))goto done;
    /* A partial Flatpak checkout consists only of metadata and files/. */
    int scan=openat(expected,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(scan<0)goto done;
    DIR* d=fdopendir(scan);if(!d){close(scan);goto done;}
    struct dirent* e;rc=0;
    while(!rc&&(e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")&&
            strcmp(e->d_name,"metadata")&&strcmp(e->d_name,"files"))
        rc=sp_remove(expected,e->d_name,st.st_dev,0);
    closedir(d);
done:
    if(files>=0)close(files);
    if(rc&&error&&size)snprintf(error,size,"invalid authenticated extension subpath projection");
    return rc;
}
#endif
