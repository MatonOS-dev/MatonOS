#ifndef MATON_HOME_LABELS_H
#define MATON_HOME_LABELS_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
/* Stock vold restorecon on misc_ce resets custom MLS categories to s0.
 * Repair only our own data type, through pinned inodes, as the verified UID.
 * This is label maintenance, not storage migration. Root is labelled last so
 * an interrupted walk retries on the next launch. Never follow user symlinks. */
static inline int maton_home_label(int fd,const char* label,int apply) {
    char path[64],current[256]={0};snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
    if(getxattr(path,"security.selinux",current,sizeof(current)-1)<=0)return -1;
    if(strcmp(current,label) && strcmp(current,"u:object_r:matonos_linux_data_file:s0")){errno=EPERM;return -1;}
    return apply && strcmp(current,label) ? setxattr(path,"security.selinux",label,strlen(label)+1,0) : 0;
}
static inline int maton_home_repair_labels(int home,unsigned uid,const char* label) {
    long previous=syscall(SYS_setfsuid,uid);
    if(syscall(SYS_setfsuid,-1)!=(long)uid){syscall(SYS_setfsuid,previous);errno=EPERM;return -1;}
    char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",home);
    int root=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    struct stat origin;
    DIR** stack=NULL;size_t depth=0,capacity=0;int rc=-1;
    if(root<0 || fstat(root,&origin))goto done;
    capacity=1;stack=malloc(sizeof(*stack));if(!stack)goto done;
    stack[0]=fdopendir(root);if(!stack[0])goto done;root=-1;depth=1;
    while(depth) {
        int parent=dirfd(stack[depth-1]);errno=0;
        struct dirent* entry=readdir(stack[depth-1]);
        if(!entry) {
            if(errno || maton_home_label(parent,label,1))goto done;
            closedir(stack[--depth]);continue;
        }
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        int child=openat(parent,entry->d_name,O_PATH|O_NOFOLLOW|O_CLOEXEC);
        struct stat st;
        if(child<0)goto done;
        if(fstat(child,&st)||st.st_dev!=origin.st_dev||st.st_uid!=(uid_t)uid){close(child);errno=EPERM;goto done;}
        if(S_ISLNK(st.st_mode)) {
            char* link=NULL;char current[256]={0};
            if(asprintf(&link,"/proc/self/fd/%d/%s",parent,entry->d_name)<0){close(child);goto done;}
            ssize_t n=lgetxattr(link,"security.selinux",current,sizeof(current)-1);
            int bad=n<=0 || (strcmp(current,label)&&strcmp(current,"u:object_r:matonos_linux_data_file:s0"));
            if(!bad && strcmp(current,label))bad=lsetxattr(link,"security.selinux",label,strlen(label)+1,0);
            free(link);close(child);if(bad)goto done;
        } else if(S_ISDIR(st.st_mode)) {
            if(maton_home_label(child,label,0)){close(child);goto done;}
            snprintf(path,sizeof(path),"/proc/self/fd/%d",child);
            int directory=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);close(child);
            if(directory<0)goto done;
            if(depth==capacity) {
                if(capacity>SIZE_MAX/2/sizeof(*stack)){close(directory);errno=EOVERFLOW;goto done;}
                size_t next=capacity*2;DIR** grown=realloc(stack,next*sizeof(*stack));
                if(!grown){close(directory);goto done;}stack=grown;capacity=next;
            }
            DIR* dir=fdopendir(directory);if(!dir){close(directory);goto done;}
            stack[depth++]=dir;
        } else {
            int bad=maton_home_label(child,label,1);close(child);if(bad)goto done;
        }
    }
    rc=0;
done:
    {int saved=errno;
    while(depth)closedir(stack[--depth]);
    free(stack);if(root>=0)close(root);
    syscall(SYS_setfsuid,previous);errno=saved;}
    return rc;
}
#endif
