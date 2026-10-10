/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>
int main(void) {
    char a=1,b=2;struct iovec l={&a,1},r={&b,1};
    ssize_t w=process_vm_writev(getpid(),&l,1,&r,1,0);
    printf("self process_vm_writev=%zd errno=%d\n",w,errno);errno=0;
    ssize_t n=process_vm_readv(getpid(),&l,1,&r,1,0);
    printf("self process_vm_readv=%zd errno=%d\n",n,errno);
    return w==1&&n==1?0:77;
}
