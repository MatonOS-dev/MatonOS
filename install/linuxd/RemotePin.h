#ifndef MATON_REMOTE_PIN_H
#define MATON_REMOTE_PIN_H
/* linuxd's private metadata; caller serializes publication. A durable pin is
 * retained after uninstall so another user cannot silently change provenance. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static inline int remote_pin_name(const char* ref, char* name, size_t size) {
    if (!ref || strncmp(ref,"app/",4)) return -1;
    const char* end=strchr(ref+4,'/');
    if (!end || end==ref+4 || (size_t)(end-ref-4)>255 || ref[4]=='.' || end[-1]=='.') return -1;
    for(const char* p=ref+4;p<end;p++)
        if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='_'||*p=='-'))return -1;
    return snprintf(name,size,"%.*s",(int)(end-ref-4),ref+4)<(int)size?0:-1;
}
/* 0 = absent/matching, -1 = corrupt/I/O, -2 = another remote. */
static inline int remote_pin_check(int directory,const char* ref,const char* remote) {
    char name[272],value[130];
    if(remote_pin_name(ref,name,sizeof(name)) || !remote || !*remote || strlen(remote)>128)return -1;
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?0:-1;
    struct stat st;int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>128;
    ssize_t n=bad?-1:read(fd,value,sizeof(value));close(fd);
    if(n<1 || n>128 || n!=st.st_size || memchr(value,0,(size_t)n))return -1;
    value[n]=0;return strcmp(value,remote)?-2:0;
}
static inline int remote_pin_commit(int directory,const char* ref,const char* remote) {
    char name[272];int rc=remote_pin_check(directory,ref,remote);
    if(rc || remote_pin_name(ref,name,sizeof(name)))return rc?rc:-1;
    /* Write+fsync before link: readers never observe an incomplete pin. The
     * fixed temp name is safe under linuxd's operation mutex; startup/retry
     * removes leftovers from a crash before publication. */
    if(unlinkat(directory,".origin-new",0)&&errno!=ENOENT)return -1;
    int fd=openat(directory,".origin-new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    size_t n=strlen(remote);
    rc=write(fd,remote,n)!=(ssize_t)n || fsync(fd);close(fd);
    if(!rc && linkat(directory,".origin-new",directory,name,0) && errno!=EEXIST)rc=-1;
    if(unlinkat(directory,".origin-new",0))rc=-1;
    if(!rc && fsync(directory))rc=-1;
    return rc?-1:remote_pin_check(directory,ref,remote);
}
#endif
