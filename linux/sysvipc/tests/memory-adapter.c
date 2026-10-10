/* SPDX-License-Identifier: Apache-2.0 */
/* Test-only transport for environments denying process_vm_* in seccomp.
 * Never link this into the shipped supervisor. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>
static ssize_t copy(pid_t pid,const struct iovec *l,unsigned long nl,
        const struct iovec *r,unsigned long nr,unsigned long flags,int write) {
    if(nl!=1||nr!=1||flags||l[0].iov_len!=r[0].iov_len){errno=EINVAL;return -1;}
    char path[64];snprintf(path,sizeof(path),"/proc/%d/mem",pid);
    int fd=open(path,(write?O_WRONLY:O_RDONLY)|O_CLOEXEC);if(fd<0)return -1;
    ssize_t n=write?pwrite(fd,l[0].iov_base,l[0].iov_len,(off_t)r[0].iov_base):
        pread(fd,l[0].iov_base,l[0].iov_len,(off_t)r[0].iov_base);
    int e=errno;close(fd);errno=e;return n;
}
ssize_t __wrap_process_vm_readv(pid_t p,const struct iovec *l,unsigned long nl,const struct iovec *r,unsigned long nr,unsigned long f){return copy(p,l,nl,r,nr,f,0);}
ssize_t __wrap_process_vm_writev(pid_t p,const struct iovec *l,unsigned long nl,const struct iovec *r,unsigned long nr,unsigned long f){return copy(p,l,nl,r,nr,f,1);}
