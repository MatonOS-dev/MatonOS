#define _GNU_SOURCE
#include "mountd_core.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/mount.h>
#include <linux/openat2.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_setns
#define SYS_setns 308
#endif
#ifndef SYS_openat2
#define SYS_openat2 437
#endif

#define REQUEST_TIMEOUT_MS 5000

static void *reap_worker(void *opaque) {
    pid_t pid = (pid_t)(intptr_t)opaque;
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return NULL;
}

static int fail(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return -1;
}

static int safe_component(const char *s, size_t max, int package) {
    size_t n;
    if (!s || !(n = strnlen(s, max)) || n >= max) return 0;
    if (s[0] == '.' || s[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              (package && c == '.'))) return 0;
        if (c == '.' && (i + 1 == n || s[i + 1] == '.')) return 0;
    }
    return 1;
}

int mountd_validate_request(const MountdRequest *r) {
    if (!r || r->pid <= 1 || r->uid < 10000 || !safe_component(r->package, sizeof(r->package), 1) ||
        !safe_component(r->deployment, sizeof(r->deployment), 0)) return -1;
    if (strnlen(r->signer, sizeof(r->signer)) != 64 ||
        strnlen(r->request_nonce, sizeof(r->request_nonce)) != 36) return -1;
    for (size_t i = 0; i < 64; ++i)
        if (!((r->signer[i] >= '0' && r->signer[i] <= '9') ||
              (r->signer[i] >= 'a' && r->signer[i] <= 'f'))) return -1;
    for (size_t i = 0; i < 36; ++i) {
        char c = r->request_nonce[i];
        if (!(c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
    }
    if (r->request_nonce[8] != '-' || r->request_nonce[13] != '-' ||
        r->request_nonce[18] != '-' || r->request_nonce[23] != '-') return -1;
    return 0;
}

int mountd_derive_paths(const MountdRequest *r, char *app, size_t app_size,
                        char *runtime, size_t runtime_size, char *home,
                        size_t home_size, char *target_home,
                        size_t target_home_size) {
    if (mountd_validate_request(r) != 0 || !app || !runtime || !home || !target_home) return -1;
    unsigned int user = r->uid / 100000U;
    int a = snprintf(app, app_size, "/data/misc_ce/%u/matonos-linux/deployments/%s/app", user, r->deployment);
    int u = snprintf(runtime, runtime_size, "/data/misc_ce/%u/matonos-linux/deployments/%s/usr", user, r->deployment);
    int h = snprintf(home, home_size, "/data/misc_ce/%u/matonos-linux/home/%u", user, r->uid);
    int t = snprintf(target_home, target_home_size, "/home/%u", r->uid);
    return a < 0 || (size_t)a >= app_size || u < 0 || (size_t)u >= runtime_size ||
           h < 0 || (size_t)h >= home_size || t < 0 || (size_t)t >= target_home_size ? -1 : 0;
}

static unsigned long long process_start_time(pid_t pid) {
    char path[64], buf[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    p += 2;
    for (int field = 3; field < 22; ++field) {
        p = strchr(p, ' ');
        if (!p) return 0;
        ++p;
    }
    return strtoull(p, NULL, 10);
}

static int process_uid(pid_t pid, uid_t *uid) {
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0) return -1;
    line[n] = '\0';
    char *p = strstr(line, "Uid:");
    if (!p || sscanf(p + 4, "%u", uid) != 1) return -1;
    return 0;
}

static int process_name_matches(pid_t pid, const char *package) {
    char path[64], name[MOUNTD_PACKAGE_MAX + 2];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t n = read(fd, name, sizeof(name) - 1);
    close(fd);
    if (n <= 0) return 0;
    name[n] = '\0';
    size_t len = strlen(package);
    return strcmp(name, package) == 0 || (strncmp(name, package, len) == 0 && name[len] == ':');
}

static int open_source(const char *path, uid_t owner) {
    char parent[512];
    if (strlen(path) >= sizeof(parent)) return -1;
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return -1;
    *slash = '\0';
    const char *leaf = slash + 1;
    int root = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) return -1;
    struct open_how how = { .flags = O_PATH | O_DIRECTORY | O_CLOEXEC,
                            .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS };
    int pfd = syscall(SYS_openat2, root, parent + 1, &how, sizeof(how));
    close(root);
    if (pfd < 0) return -1;
    int fd = syscall(SYS_openat2, pfd, leaf, &how, sizeof(how));
    close(pfd);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != owner) { close(fd); return -1; }
    return fd;
}

static int mount_fd(int source_fd, const char *target, int writable, int *created) {
    char source[64];
    struct stat source_stat, target_stat;
    struct statvfs target_fs;
    *created = 0;
    if (fstat(source_fd, &source_stat) == 0 && stat(target, &target_stat) == 0 &&
        statvfs(target, &target_fs) == 0 && source_stat.st_dev == target_stat.st_dev &&
        source_stat.st_ino == target_stat.st_ino &&
        (!!(target_fs.f_flag & ST_RDONLY) == !writable)) return 0;
    snprintf(source, sizeof(source), "/proc/self/fd/%d", source_fd);
    if (mount(source, target, NULL, MS_BIND | MS_REC, NULL)) return -1;
    *created = 1;
    if (!writable && mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL)) return -1;
    if (writable && mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NODEV, NULL)) return -1;
    return 0;
}

static int identity_alive(int pidfd, pid_t pid, uid_t uid, unsigned long long start) {
    uid_t actual;
    if (syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0) != 0 ||
        process_uid(pid, &actual) != 0 || actual != uid || process_start_time(pid) != start) return 0;
    return 1;
}

int mountd_mount_request(const MountdRequest *r, char *error, size_t error_size) {
    char app[512], runtime[512], home[512], target_home[64];
    int pidfd = -1, nsfd = -1, appfd = -1, runtimefd = -1, homefd = -1;
    unsigned long long started;
    uid_t uid;
    int result = -1;
    if (mountd_derive_paths(r, app, sizeof(app), runtime, sizeof(runtime), home, sizeof(home),
                            target_home, sizeof(target_home))) return fail(error, error_size, "invalid request");
    if (process_uid(r->pid, &uid) || uid != r->uid ||
        !process_name_matches(r->pid, r->package))
        return fail(error, error_size, "pid/package/uid identity mismatch");
    started = process_start_time(r->pid);
    if (!started) return fail(error, error_size, "cannot pin process start identity");
    pidfd = syscall(SYS_pidfd_open, r->pid, 0);
    if (pidfd < 0) return fail(error, error_size, "pidfd_open failed");
    if (!identity_alive(pidfd, r->pid, r->uid, started)) { fail(error, error_size, "process identity changed"); goto done; }
    char nspath[64];
    snprintf(nspath, sizeof(nspath), "/proc/%d/ns/mnt", r->pid);
    nsfd = open(nspath, O_RDONLY | O_CLOEXEC);
    if (nsfd < 0) { fail(error, error_size, "cannot open mount namespace"); goto done; }
    appfd = open_source(app, r->uid);
    runtimefd = open_source(runtime, r->uid);
    homefd = open_source(home, r->uid);
    if (appfd < 0 || runtimefd < 0 || homefd < 0) { fail(error, error_size, "verified deployment/home source unavailable"); goto done; }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) { fail(error, error_size, "cannot create worker status pipe"); goto done; }
    pid_t worker = fork();
    if (worker < 0) {
        close(pipefd[0]); close(pipefd[1]);
        fail(error, error_size, "cannot start bounded mount worker"); goto done;
    }
    if (worker == 0) {
        close(pipefd[0]);
        int child_result = -1;
        int app_mounted = 0, runtime_mounted = 0, home_mounted = 0;
        if (syscall(SYS_setns, nsfd, CLONE_NEWNS) == 0 &&
            mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0 &&
            identity_alive(pidfd, r->pid, r->uid, started) &&
            mount_fd(appfd, "/app", 0, &app_mounted) == 0 &&
            mount_fd(runtimefd, "/usr", 0, &runtime_mounted) == 0 &&
            mount_fd(homefd, target_home, 1, &home_mounted) == 0 &&
            identity_alive(pidfd, r->pid, r->uid, started)) child_result = 0;
        if (child_result != 0) {
            if (home_mounted) umount2(target_home, MNT_DETACH);
            if (runtime_mounted) umount2("/usr", MNT_DETACH);
            if (app_mounted) umount2("/app", MNT_DETACH);
            const char msg[] = "setns, identity check, or bind mount failed";
            ssize_t ignored = write(pipefd[1], msg, sizeof(msg));
            (void)ignored;
        }
        close(pipefd[1]);
        _exit(child_result == 0 ? 0 : 1);
    }
    close(pipefd[1]);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long deadline = (unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL + REQUEST_TIMEOUT_MS;
    int status = 0, worker_done = 0;
    for (;;) {
        pid_t waited = waitpid(worker, &status, WNOHANG);
        if (waited == worker) { worker_done = 1; break; }
        if (waited < 0 && errno != EINTR) { fail(error, error_size, "mount worker wait failed"); break; }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        unsigned long long now = (unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
        if (now >= deadline) {
            kill(worker, SIGKILL);
            pid_t waited = waitpid(worker, &status, WNOHANG);
            if (waited == 0) {
                pthread_t reaper;
                if (pthread_create(&reaper, NULL, reap_worker, (void *)(intptr_t)worker) == 0)
                    pthread_detach(reaper);
            }
            fail(error, error_size, "mount request timed out");
            worker_done = waited == worker;
            break;
        }
        usleep(10000);
    }
    if (worker_done && WIFEXITED(status) && WEXITSTATUS(status) == 0) result = 0;
    else if (!error || !error[0]) {
        char message[160] = {};
        ssize_t n = read(pipefd[0], message, sizeof(message) - 1);
        if (n > 0) fail(error, error_size, message);
        else fail(error, error_size, "mount worker failed");
    }
    close(pipefd[0]);
done:
    if (homefd >= 0) close(homefd);
    if (runtimefd >= 0) close(runtimefd);
    if (appfd >= 0) close(appfd);
    if (nsfd >= 0) close(nsfd);
    if (pidfd >= 0) close(pidfd);
    return result;
}
