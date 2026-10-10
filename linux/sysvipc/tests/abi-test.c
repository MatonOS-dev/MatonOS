/* SPDX-License-Identifier: Apache-2.0 */
/* Wire-level tests complement real ELF32 tests when an outer sandbox kills
 * AUDIT_ARCH_I386. The supervisor itself is included to test private decoding. */
#define _GNU_SOURCE
#include "../sysvipc.c"
#include <assert.h>
#include <stdarg.h>
#include <sys/mman.h>
int __wrap_ioctl(int fd, unsigned long cmd, ...) {
    (void)fd;
    va_list ap;va_start(ap,cmd);void *arg=va_arg(ap,void *);va_end(ap);
    if(cmd==SECCOMP_IOCTL_NOTIF_ID_VALID)return 0;
    if(cmd==SECCOMP_IOCTL_NOTIF_ADDFD){struct seccomp_notif_addfd *a=arg;return dup((int)a->srcfd);}
    return 0;
}
ssize_t __wrap_process_vm_readv(pid_t pid,const struct iovec *l,unsigned long nl,const struct iovec *r,unsigned long nr,unsigned long flags){
    (void)pid;(void)nl;(void)nr;(void)flags;memcpy(l->iov_base,r->iov_base,l->iov_len);return (ssize_t)l->iov_len;
}
ssize_t __wrap_process_vm_writev(pid_t pid,const struct iovec *l,unsigned long nl,const struct iovec *r,unsigned long nr,unsigned long flags){
    (void)pid;(void)nl;(void)nr;(void)flags;memcpy(r->iov_base,l->iov_base,l->iov_len);return (ssize_t)l->iov_len;
}
static struct request request(int nr){
    struct request r={0};r.notif.data.arch=AUDIT_ARCH_I386;r.notif.data.nr=nr;
    r.caller.pid=r.caller.tgid=getpid();r.caller.uid=getuid();r.caller.gid=getgid();return r;
}
int main(void){
    unsigned char *b=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);assert(b!=MAP_FAILED);
    struct maton_sysvipc *s=maton_sysvipc_new();assert(s);
    struct request r=request(393);r.notif.data.args[0]=123;r.notif.data.args[1]=2;r.notif.data.args[2]=IPC_CREAT|0600;
    assert(!decode(s,&r));int id=(int)dispatch(s,&r,false);assert(id>=0);
    struct object *o=find(s,KSEM,id);assert(o);o->atime=INT64_C(0x123456789);o->ctime=INT64_C(0x23456789a);
    r=request(394);r.notif.data.args[0]=id;r.notif.data.args[2]=IPC_STAT;r.notif.data.args[3]=(uintptr_t)b;assert(!decode(s,&r));assert(!dispatch(s,&r,false));
    assert(getnum(b,36,8)==(uint64_t)o->atime&&getnum(b,44,8)==(uint64_t)o->ctime&&getnum(b,52,4)==2);
    uint32_t *arg=(uint32_t *)(b+256);*arg=(uint32_t)(uintptr_t)b;
    r=request(117);r.notif.data.args[0]=SEMCTL;r.notif.data.args[1]=id;r.notif.data.args[3]=IPC_STAT;r.notif.data.args[4]=(uintptr_t)arg;
    assert(!decode(s,&r));assert(!dispatch(s,&r,false));assert(getnum(b,40,2)==2&&getnum(b,16,4)==(uint32_t)o->atime);
    r.notif.data.args[3]=IPC_STAT|IPC_64;assert(!decode(s,&r));assert(!dispatch(s,&r,false));assert(getnum(b,52,4)==2);
    r=request(420);r.notif.data.args[0]=id;r.notif.data.args[1]=(uintptr_t)(b+512);r.notif.data.args[2]=1;r.notif.data.args[3]=(uintptr_t)(b+768);
    struct sembuf op={0,-1,0};memcpy(b+512,&op,sizeof(op));int64_t ts[2]={0,0};memcpy(b+768,ts,sizeof(ts));
    assert(!decode(s,&r)&&r.time64);assert(dispatch(s,&r,false)==-EAGAIN);free(r.ops);
    r=request(117);r.notif.data.args[0]=SEMTIMEDOP;r.notif.data.args[1]=id;r.notif.data.args[2]=1;r.notif.data.args[4]=(uintptr_t)(b+512);r.notif.data.args[5]=(uintptr_t)(b+768);
    assert(!decode(s,&r)&&!r.time64);assert(dispatch(s,&r,false)==-EAGAIN);free(r.ops);
    for(int k=KSHM;k<=KMSG;k++){
        struct object obj={.kind=(enum kind)k,.mode=0600,.size=123,.count=5,.bytes=14,.qbytes=16384,
            .atime=INT64_C(0x123456789),.dtime=INT64_C(0x23456789a),.ctime=INT64_C(0x3456789ab),.cpid=42,.lpid=43};
        r=request(396);r.compat=true;r.old=false;assert(!stat_ds(s,&r,&obj,(uintptr_t)b));
        if(k==KSHM){assert(getnum(b,36,4)==123&&getnum(b,40,8)==(uint64_t)obj.atime&&getnum(b,72,4)==5);}
        else {assert(getnum(b,36,8)==(uint64_t)obj.atime&&getnum(b,60,4)==14&&getnum(b,64,4)==5&&getnum(b,68,4)==16384);}
        r.old=true;assert(!stat_ds(s,&r,&obj,(uintptr_t)b));assert(getnum(b,k==KSHM?36:46,2)==5);
    }
    r=request(117);r.notif.data.args[0]=MSGRCV;r.notif.data.args[1]=7;r.notif.data.args[2]=8;r.notif.data.args[4]=(uintptr_t)(b+256);
    arg[0]=(uint32_t)(uintptr_t)(b+512);arg[1]=(uint32_t)-7;assert(!decode(s,&r));assert(r.a[1]==arg[0]&&(int32_t)r.a[3]==-7);
    r.notif.data.args[0]=(1u<<16)|MSGRCV;r.notif.data.args[5]=9;assert(!decode(s,&r));assert(r.a[1]==(uintptr_t)arg&&r.a[3]==9);
    r=request(999);assert(decode(s,&r)==-EINVAL);r.notif.data.arch=AUDIT_ARCH_X86_64;r.notif.data.nr=0x4000001e;assert(decode(s,&r)==-EINVAL);
    r=request(394);r.notif.data.args[0]=id;r.notif.data.args[2]=IPC_RMID;
    assert(!decode(s,&r));assert(!dispatch(s,&r,false));s->nextid[KSEM]=32768;
    r=request(393);r.notif.data.args[1]=1;r.notif.data.args[2]=0600;
    assert(!decode(s,&r));int replacement=(int)dispatch(s,&r,false);assert(replacement==32768);
    r=request(394);r.notif.data.args[0]=id;r.notif.data.args[2]=GETVAL;
    assert(!decode(s,&r));assert(dispatch(s,&r,false)==-EINVAL);
    r.notif.data.args[2]=SEM_STAT;r.notif.data.args[3]=(uintptr_t)b;
    assert(!decode(s,&r));assert(dispatch(s,&r,false)==replacement);assert(getnum(b,24,2)==1);
    maton_sysvipc_free(s);munmap(b,4096);puts("PASS: i386 wire decoding, old/IPC_64 metadata, high time words, multiplexed arguments, time32/time64");return 0;
}
