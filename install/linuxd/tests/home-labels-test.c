#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
static ino_t root_inode,file_inode;
static int changed,root_last,foreign;
static const char expected[]="u:object_r:matonos_linux_data_file:s0:c1,c2";
static ssize_t fake_get(const char* path,const char* name,void* value,size_t size) {
    assert(!strcmp(name,"security.selinux"));struct stat st;assert(!stat(path,&st));
    const char* label=foreign && st.st_ino==file_inode ? "u:object_r:untrusted:s0" : "u:object_r:matonos_linux_data_file:s0";
    assert(size>strlen(label));strcpy(value,label);return (ssize_t)strlen(label)+1;
}
static int fake_set(const char* path,const char* name,const void* value,size_t size,int flags) {
    assert(!strcmp(name,"security.selinux")&&!strcmp(value,expected)&&size==sizeof(expected)&&flags==0);
    struct stat st;assert(!stat(path,&st));changed++;if(st.st_ino==root_inode)root_last=changed;return 0;
}
static ssize_t fake_lget(const char* path,const char* name,void* value,size_t size) {
    struct stat st;assert(!lstat(path,&st)&&S_ISLNK(st.st_mode));
    assert(!strcmp(name,"security.selinux")&&size>48);
    strcpy(value,"u:object_r:matonos_linux_data_file:s0");return (ssize_t)strlen(value)+1;
}
static int fake_lset(const char* path,const char* name,const void* value,size_t size,int flags) {
    struct stat st;assert(!lstat(path,&st)&&S_ISLNK(st.st_mode));
    assert(!strcmp(name,"security.selinux")&&!strcmp(value,expected)&&size==sizeof(expected)&&!flags);changed++;return 0;
}
#define getxattr fake_get
#define setxattr fake_set
#define lgetxattr fake_lget
#define lsetxattr fake_lset
#include "../HomeLabels.h"
int main(void) {
    char root[]="/tmp/maton-home-labels.XXXXXX";assert(mkdtemp(root));
    int fd=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(fd>=0);
    assert(!mkdirat(fd,"nested",0700));
    int file=openat(fd,"nested/file",O_WRONLY|O_CREAT|O_CLOEXEC,0600);assert(file>=0);
    struct stat st;assert(!fstat(file,&st));file_inode=st.st_ino;close(file);
    assert(!symlinkat("/",fd,"escape"));assert(!fstat(fd,&st));root_inode=st.st_ino;
    assert(!maton_home_repair_labels(fd,getuid(),expected));assert(changed==4&&root_last==4);
    changed=0;root_last=0;foreign=1;
    assert(maton_home_repair_labels(fd,getuid(),expected)==-1);assert(root_last==0);
    assert(!unlinkat(fd,"nested/file",0)&&!unlinkat(fd,"nested",AT_REMOVEDIR)&&!unlinkat(fd,"escape",0));
    close(fd);assert(!rmdir(root));puts("Home label repair: descendants, symlinks, root-last and foreign-label rejection passed");return 0;
}
