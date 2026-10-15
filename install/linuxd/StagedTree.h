#ifndef MATON_STAGED_TREE_H
#define MATON_STAGED_TREE_H
/* No payload copies: rebuild directories/symlinks, account all regular-inode
 * aliases, and seal regular inodes before consuming any producer bytes. */
#include "StagedVerity.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/xattr.h>
#include <sys/syscall.h>
#include <linux/openat2.h>
#include <linux/stat.h>
#ifndef MATON_STAGED_STATX
#define MATON_STAGED_STATX syscall
#endif

static inline int maton_staged_bad(char* error,size_t size,const char* text) {
    if(error&&size)snprintf(error,size,"%s",text);
    errno=EPERM;return -1;
}
struct maton_staged_inode {
    ino_t inode;dev_t device;nlink_t links;size_t seen;int zone;
    struct maton_staged_inode* next;
};
#define MATON_STAGED_INODE_BUCKETS 65536
struct maton_staged_inodes {struct maton_staged_inode* buckets[MATON_STAGED_INODE_BUCKETS];};
static inline struct maton_staged_inode* maton_staged_inode(struct maton_staged_inodes* all,const struct stat* st) {
    size_t bucket=((size_t)st->st_ino^(size_t)st->st_dev)%MATON_STAGED_INODE_BUCKETS;
    struct maton_staged_inode* item=all->buckets[bucket];
    while(item&&(item->inode!=st->st_ino||item->device!=st->st_dev))item=item->next;
    if(!item){item=calloc(1,sizeof(*item));if(!item)return NULL;item->inode=st->st_ino;item->device=st->st_dev;item->links=st->st_nlink;item->next=all->buckets[bucket];all->buckets[bucket]=item;}
    return item;
}
static inline void maton_staged_inodes_free(struct maton_staged_inodes* all) {
    if(!all)return;
    for(size_t i=0;i<MATON_STAGED_INODE_BUCKETS;i++){
        struct maton_staged_inode* item=all->buckets[i];
        while(item){struct maton_staged_inode* next=item->next;free(item);item=next;}
    }
    free(all);
}
static inline int maton_staged_capture_inner(struct maton_staged_inodes* all,int source,int destination,dev_t device,
        uid_t owner,unsigned depth,char* error,size_t size) {
    struct stat root,dst;
    if(depth>128 || fstat(source,&root)||fstat(destination,&dst)||
            root.st_dev!=device||dst.st_dev!=device||dst.st_uid!=owner||fchmod(destination,0700)||
            fchown(source,owner,owner)||fchmod(source,0700))
        return maton_staged_bad(error,size,"cannot capture staging directory namespace");
    DIR* entries=fdopendir(dup(source));if(!entries)return -1;rewinddir(entries);
    struct dirent* e;int rc=0;
    while(!rc&&(e=readdir(entries))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;
        if(fstatat(source,e->d_name,&st,AT_SYMLINK_NOFOLLOW)||st.st_dev!=device||
                (st.st_mode&06000)){rc=maton_staged_bad(error,size,"unsafe staged inode");break;}
        if(S_ISDIR(st.st_mode)) {
            if(fchownat(source,e->d_name,owner,owner,AT_SYMLINK_NOFOLLOW)||fchmodat(source,e->d_name,0700,0)){rc=-1;break;}
            int in=openat(source,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(in<0||mkdirat(destination,e->d_name,0700)){if(in>=0)close(in);rc=-1;break;}
            int out=openat(destination,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(out<0){close(in);rc=-1;break;}
            rc=maton_staged_capture_inner(all,in,out,device,owner,depth+1,error,size);
            if(!rc)rc=fchmod(out,st.st_mode&0777)||fsync(out);
            close(in);close(out);
            if(!rc)rc=unlinkat(source,e->d_name,AT_REMOVEDIR);
        } else if(S_ISREG(st.st_mode)) {
            struct maton_staged_inode* item=maton_staged_inode(all,&st);
            if(!item||item->links!=st.st_nlink){rc=-1;break;}
            item->seen++;
            if(fchownat(source,e->d_name,owner,owner,AT_SYMLINK_NOFOLLOW)||
                    fchmodat(source,e->d_name,(st.st_mode&0777)|0600,0)||
                    renameat(source,e->d_name,destination,e->d_name)){rc=-1;break;}
            int fd=openat(destination,e->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            struct stat now;
            if(fd<0){rc=-1;break;}
            if(fstat(fd,&now)||now.st_ino!=st.st_ino||now.st_dev!=device||now.st_nlink!=st.st_nlink||
                    fchmod(fd,(st.st_mode&0777)|0600))rc=-1;
            char attrs[65536];ssize_t n=rc?-1:flistxattr(fd,attrs,sizeof(attrs));
            if(n<0)rc=-1;
            for(ssize_t i=0;!rc&&i<n;) {
                size_t len=strnlen(attrs+i,(size_t)(n-i));
                if(len==(size_t)(n-i)){rc=-1;break;}
                if(strcmp(attrs+i,"security.selinux")&&fremovexattr(fd,attrs+i))rc=-1;
                i+=(ssize_t)len+1;
            }
            close(fd);
            if(!rc) {
                fd=maton_verity_openat(destination,e->d_name,device,owner,error,size);
                if(fd<0)rc=-1;else {rc=fchmod(fd,st.st_mode&0777)||fsync(fd);close(fd);}
            }
        } else if(S_ISLNK(st.st_mode)) {
            char text[4096];ssize_t n=readlinkat(source,e->d_name,text,sizeof(text)-1);
            if(n<=0||n==(ssize_t)sizeof(text)-1){rc=-1;break;}
            text[n]=0;
            rc=symlinkat(text,destination,e->d_name)||unlinkat(source,e->d_name,0);
        } else rc=maton_staged_bad(error,size,"staged special file rejected");
    }
    closedir(entries);
    if(!rc)rc=fsync(destination);
    return rc;
}
/* Account every alias in the captured installation before accepting it. */
static inline int maton_staged_capture(int source,int destination,dev_t device,
        uid_t owner,unsigned depth,char* error,size_t size) {
    struct maton_staged_inodes* all=calloc(1,sizeof(*all));if(!all)return -1;
    int rc=maton_staged_capture_inner(all,source,destination,device,owner,depth,error,size);
    for(size_t i=0;!rc&&i<MATON_STAGED_INODE_BUCKETS;i++)
        for(struct maton_staged_inode* item=all->buckets[i];item;item=item->next)
            if(item->seen!=item->links){rc=maton_staged_bad(error,size,"staged hardlink alias outside captured installation");break;}
    maton_staged_inodes_free(all);return rc;
}
static inline int maton_staged_equal_bytes(int a,int b) {
    char left[65536],right[65536];
    for(;;) {
        ssize_t x=read(a,left,sizeof(left));if(x<0){if(errno==EINTR)continue;return -1;}
        size_t got=0;
        while(got<(size_t)x){ssize_t y=read(b,right+got,(size_t)x-got);if(y<0&&errno==EINTR)continue;if(y<=0)return -1;got+=(size_t)y;}
        if(memcmp(left,right,(size_t)x))return -1;
        if(!x){char c;ssize_t y;do{y=read(b,&c,1);}while(y<0&&errno==EINTR);return y==0?0:-1;}
    }
}
/* Validate unsigned output against the kernel's complete symlink resolution,
 * including intermediate links. Dangling links are conservatively rejected. */
static inline int maton_staged_extra(int root,int dir,dev_t device,unsigned depth,
        const char* prefix,char* error,size_t size) {
    if(depth>128)return -1;
    DIR* entries=fdopendir(dup(dir));if(!entries)return -1;rewinddir(entries);
    struct dirent* e;int rc=0;
    while(!rc&&(e=readdir(entries))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;char path[4096];
        if(snprintf(path,sizeof(path),"%s%s%s",prefix,*prefix?"/":"",e->d_name)>=(int)sizeof(path)||
                fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)||st.st_dev!=device||
                (st.st_mode&06000)){rc=-1;break;}
        if(S_ISLNK(st.st_mode)) {
            struct open_how how={.flags=O_PATH|O_CLOEXEC,.resolve=RESOLVE_BENEATH|RESOLVE_NO_MAGICLINKS|RESOLVE_NO_XDEV};
            int fd=(int)syscall(SYS_openat2,root,path,&how,sizeof(how));
            if(fd<0)rc=-1;else close(fd);
        }else if(S_ISDIR(st.st_mode)){
            if(fchmodat(dir,e->d_name,0755,0)){rc=-1;break;}
            int fd=openat(dir,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(fd<0)rc=-1;else{rc=fchmod(fd,0755)||maton_staged_extra(root,fd,device,depth+1,path,error,size);close(fd);}
        }else if(!S_ISREG(st.st_mode)||st.st_nlink!=1)rc=-1;
        else {
            rc=fchmodat(dir,e->d_name,(st.st_mode&0111)?0755:0644,0);
        }
    }
    closedir(entries);
    return rc?maton_staged_bad(error,size,"unsafe unsigned extra-data tree"):0;
}
/* Existing code is already outside the staging ioctl policy zone. Require
 * statx to attest immutable verity state; never enable a seal on live code. */
static inline int maton_staged_open_existing(int dir,const char* name,dev_t device,
        uid_t owner,char* error,size_t size) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0)return -1;
    struct stat st;struct statx sx;memset(&sx,0,sizeof(sx));
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_dev!=device||st.st_uid!=owner||
            (st.st_mode&06022)||MATON_STAGED_STATX(SYS_statx,fd,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_BASIC_STATS,&sx)||
            !(sx.stx_attributes_mask&STATX_ATTR_VERITY)||!(sx.stx_attributes&STATX_ATTR_VERITY)){
        close(fd);maton_staged_bad(error,size,"existing code is not fs-verity sealed");return -1;
    }
    return fd;
}
/* Exact signed checkout comparison. Only the designated files/extra output
 * may be absent from the signed commit; the rest of a deployment is exact. */
static inline int maton_staged_compare_inner(int actual,int expected,dev_t actual_device,
        dev_t expected_device,unsigned depth,int allow_extra,int enable,char* error,size_t size) {
    if(depth>128)return -1;
    DIR* entries=fdopendir(dup(actual));if(!entries)return -1;rewinddir(entries);
    struct dirent* e;int rc=0;
    while(!rc&&(e=readdir(entries))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat a,b;
        if(fstatat(actual,e->d_name,&a,AT_SYMLINK_NOFOLLOW)||a.st_dev!=actual_device||
                (!enable&&(a.st_uid!=1000||(!S_ISLNK(a.st_mode)&&(a.st_mode&0022))))){rc=-1;break;}
        if(allow_extra==2&&!strcmp(e->d_name,"extra")) {
            if(!S_ISDIR(a.st_mode)){rc=-1;break;}
            if(fchmodat(actual,e->d_name,0755,0)){rc=-1;break;}
            int fd=openat(actual,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(fd<0)rc=-1;else{rc=fchmod(fd,0755)||maton_staged_extra(fd,fd,actual_device,0,"",error,size);close(fd);}
            /* A signed extra/ subtree must still compare; unsigned children
             * may only occupy an absent or empty signed directory. */
            if(!rc&&!fstatat(expected,e->d_name,&b,AT_SYMLINK_NOFOLLOW)) {
                int ef=openat(expected,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                DIR* ed=ef<0?NULL:fdopendir(ef);struct dirent* ee;
                if(!ed)rc=-1;else{while((ee=readdir(ed)))if(strcmp(ee->d_name,".")&&strcmp(ee->d_name,"..")){rc=-1;break;}closedir(ed);}
            }else if(!rc&&errno!=ENOENT)rc=-1;
            continue;
        }
        if(fstatat(expected,e->d_name,&b,AT_SYMLINK_NOFOLLOW)||b.st_dev!=expected_device||
                (a.st_mode&S_IFMT)!=(b.st_mode&S_IFMT)||(a.st_mode&0777)!=(b.st_mode&0777)){rc=-1;break;}
        if(S_ISREG(a.st_mode)) {
            int af=enable?maton_verity_openat(actual,e->d_name,actual_device,a.st_uid,error,size):
                maton_staged_open_existing(actual,e->d_name,actual_device,1000,error,size);
            int bf=openat(expected,e->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
            if(af<0||bf<0||a.st_size!=b.st_size||maton_staged_equal_bytes(af,bf))rc=-1;
            if(af>=0)close(af);
            if(bf>=0)close(bf);
        }else if(S_ISLNK(a.st_mode)){
            char at[4096],bt[4096];ssize_t an=readlinkat(actual,e->d_name,at,sizeof(at)),bn=readlinkat(expected,e->d_name,bt,sizeof(bt));
            if(an<0||an==(ssize_t)sizeof(at)||an!=bn||memcmp(at,bt,(size_t)an))rc=-1;
        }else if(S_ISDIR(a.st_mode)){
            int af=openat(actual,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),bf=openat(expected,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            int next=allow_extra==1&&!strcmp(e->d_name,"files")?2:0;
            if(af<0||bf<0||maton_staged_compare_inner(af,bf,actual_device,expected_device,depth+1,next,enable,error,size))rc=-1;
            if(af>=0)close(af);
            if(bf>=0)close(bf);
        }else rc=-1;
    }
    closedir(entries);if(rc)return maton_staged_bad(error,size,"staged deployment differs from signed commit");
    entries=fdopendir(dup(expected));if(!entries)return -1;rewinddir(entries);
    while((e=readdir(entries))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;if(fstatat(actual,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){rc=-1;break;}
    }
    closedir(entries);return rc?maton_staged_bad(error,size,"missing signed deployment entry"):0;
}
static inline int maton_staged_compare(int actual,int expected,dev_t actual_device,
        dev_t expected_device,unsigned depth,int allow_extra,char* error,size_t size) {
    return maton_staged_compare_inner(actual,expected,actual_device,expected_device,depth,allow_extra,1,error,size);
}
#endif
