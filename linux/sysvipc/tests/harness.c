/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "../sysvipc.h"
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { perror(#x); exit(1); } } while (0)
static int filter(void) {
    const int native[] = {29,30,31,64,65,66,67,68,69,70,71,220};
    const int compat[] = {117,393,394,395,396,397,398,399,400,401,402,420};
    struct sock_filter code[64]; size_t n = 0;
#define ADD(x) do { code[n++] = (struct sock_filter)x; } while (0)
    ADD(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)));
    ADD(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 0, 2 + 2*sizeof(native)/sizeof(native[0])));
    ADD(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)));
    for (size_t i = 0; i < sizeof(native)/sizeof(native[0]); i++) {
        ADD(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, native[i], 0, 1));
        ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_USER_NOTIF));
    }
    ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW));
    ADD(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_I386, 0, 2 + 2*sizeof(compat)/sizeof(compat[0])));
    ADD(BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)));
    for (size_t i = 0; i < sizeof(compat)/sizeof(compat[0]); i++) {
        ADD(BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, compat[i], 0, 1));
        ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_USER_NOTIF));
    }
    ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW));
    ADD(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS));
    struct sock_fprog p = { .len = (unsigned short)n, .filter = code };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
    return (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &p);
}
static void sendfd(int sock, int fd) {
    char c = 0, control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io = { &c, 1 };
    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(fd)); CHECK(sendmsg(sock, &msg, 0) == 1);
}
static int recvfd(int sock) {
    char c, control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io = { &c, 1 };
    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control) };
    CHECK(recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) == 1);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); CHECK(cm && cm->cmsg_type == SCM_RIGHTS);
    int fd; memcpy(&fd, CMSG_DATA(cm), sizeof(fd)); return fd;
}
struct session { pid_t pid; int fd, result; struct maton_sysvipc *s; };
static void *serve(void *arg) {
    struct session *s = arg; s->result = maton_sysvipc_serve(s->s, s->fd); return NULL;
}
#ifndef HARNESS_NO_MAIN
static struct session start(char **argv) {
    int pair[2]; CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET|SOCK_CLOEXEC, 0, pair));
    struct session s = { .pid = fork() }; CHECK(s.pid >= 0);
    if (!s.pid) {
        close(pair[0]); int fd = filter(); CHECK(fd >= 0); sendfd(pair[1], fd); close(fd); close(pair[1]);
        if (argv[2]) CHECK(!setenv("LD_PRELOAD", argv[2], 1));
        execl(argv[1], argv[1], (char *)NULL); perror("execl"); _exit(1);
    }
    close(pair[1]); s.fd = recvfd(pair[0]); close(pair[0]); s.s = maton_sysvipc_new(); CHECK(s.s); return s;
}
int main(int argc, char **argv) {
    CHECK(argc >= 2); alarm(45);
    /* Both live supervisors create IPC_EXCL objects with identical keys. */
    struct session s[2] = { start(argv), start(argv) };
    pthread_t threads[2];
    for (int i = 0; i < 2; i++) CHECK(!pthread_create(&threads[i], NULL, serve, &s[i]));
    if (argc > 3 && !strcmp(argv[3], "--stop")) {
        struct timespec delay = {0, 100000000}; nanosleep(&delay, NULL);
        for (int i = 0; i < 2; i++) maton_sysvipc_stop(s[i].s);
    }
    int failed = 0;
    for (int i = 0; i < 2; i++) {
        int status; CHECK(waitpid(s[i].pid, &status, 0) == s[i].pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr,"session %d status=%d exit=%d\n",i,status,WIFEXITED(status)?WEXITSTATUS(status):-1); failed = 1; }
        /* Stop is safe concurrently, even if tracee-exit HUP already ended serve. */
        maton_sysvipc_stop(s[i].s); CHECK(!pthread_join(threads[i], NULL));
        if (s[i].result) { perror("serve"); failed = 1; }
        close(s[i].fd); maton_sysvipc_free(s[i].s);
    }
    if (!failed) puts("PASS: two isolated supervisors; listener transport and concurrent stop");
    return failed;
}
#endif
