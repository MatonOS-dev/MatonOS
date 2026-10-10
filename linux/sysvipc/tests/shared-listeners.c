/* SPDX-License-Identifier: Apache-2.0 */
/* One supervisor, two sessions with their own filters: the second listener is
 * handed over with maton_sysvipc_add_listener while serve() runs, as the stub
 * does for the portal and the app payload. */
#define HARNESS_NO_MAIN
#include "harness.c"
static pid_t spawn(const char *program, const char *role, int *listener) {
    int pair[2]; CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET|SOCK_CLOEXEC, 0, pair));
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        close(pair[0]); int fd = filter(); CHECK(fd >= 0); sendfd(pair[1], fd); close(fd); close(pair[1]);
        execl(program, program, role, (char *)NULL); perror("execl"); _exit(1);
    }
    close(pair[1]); *listener = recvfd(pair[0]); close(pair[0]); return pid;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); alarm(30);
    struct session s = { .fd = -1 };
    s.s = maton_sysvipc_new(); CHECK(s.s);
    pthread_t thread; CHECK(!pthread_create(&thread, NULL, serve, &s));
    int first, second;
    pid_t creator = spawn(argv[1], "create", &first);
    CHECK(!maton_sysvipc_add_listener(s.s, first));
    pid_t poster = spawn(argv[1], "post", &second);
    CHECK(!maton_sysvipc_add_listener(s.s, second));
    int failed = 0;
    pid_t pids[2] = { creator, poster };
    for (int i = 0; i < 2; i++) {
        int status; CHECK(waitpid(pids[i], &status, 0) == pids[i]);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) { fprintf(stderr, "child %d status=%d\n", i, status); failed = 1; }
    }
    maton_sysvipc_stop(s.s); CHECK(!pthread_join(thread, NULL));
    if (s.result) { perror("serve"); failed = 1; }
    maton_sysvipc_free(s.s);
    if (!failed) puts("PASS: two filters, one supervisor: keyed semaphore shared across listeners");
    return failed;
}
