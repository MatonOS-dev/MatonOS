/* SPDX-License-Identifier: Apache-2.0 */
/* Two processes under separate seccomp filters (separate listeners) share one
 * supervisor: "create" makes a keyed semaphore and blocks on it, "post" finds
 * the same key and releases it. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sem.h>
#include <time.h>
#define KEY 0x5151
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    struct sembuf op = { 0, 0, 0 };
    if (!strcmp(argv[1], "create")) {
        int id = semget(KEY, 1, IPC_CREAT | IPC_EXCL | 0600);
        if (id < 0) { perror("semget create"); return 1; }
        op.sem_op = -1;
        if (semop(id, &op, 1)) { perror("semop wait"); return 1; }
        return semctl(id, 0, IPC_RMID) ? 1 : 0;
    }
    struct timespec pause = { 0, 10000000 };
    for (int tries = 0; tries < 300; tries++) {
        int id = semget(KEY, 1, 0);
        if (id >= 0) { op.sem_op = 1; return semop(id, &op, 1) ? 1 : 0; }
        if (errno != ENOENT) { perror("semget open"); return 1; }
        nanosleep(&pause, NULL);
    }
    fputs("post: key never appeared\n", stderr);
    return 1;
}
