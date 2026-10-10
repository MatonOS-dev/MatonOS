/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <sys/sem.h>
int main(void) {
    int id=semget(IPC_PRIVATE,1,0600);
    if(id<0)return 1;
    struct sembuf op={0,-1,0};
    if(semop(id,&op,1)!=-1||errno!=EINTR)return 1;
    puts("PASS: concurrent stop interrupts a pending semaphore waiter");
    return 0;
}
