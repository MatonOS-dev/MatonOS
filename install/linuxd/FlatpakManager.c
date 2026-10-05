#define _GNU_SOURCE
#include <android/log.h>
#include "FlatpakManager.h"
#include "FlatpakPublish.h"
#include "UdevDatabase.h"
#include "SessionPads.h"
#include "SafePath.h"
#include "MatonMls.h"
#include <sys/xattr.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <spawn.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

/* r24: linuxd stays in system_ext; the Flatpak stack ships in the updatable
 * com.matonos.flatpak APEX, mounted at /apex/com.matonos.flatpak. This is the
 * launcher (platform-signed Flatpak env wrapper); it execs the CLI in the
 * same APEX. */
static const char k_flatpak[] = "/apex/com.matonos.flatpak/bin/flatpak-env-wrapper";
static const size_t k_output_limit = 24 * 1024;
static pthread_mutex_t g_operation_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_package_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_package_operation_active;
static FlatpakProgressCallback g_progress_callback;
static FlatpakCompleteCallback g_complete_callback;
static void* g_callback_context;

typedef struct ChildResult {
    int status;
    int truncated;
    char* output;
} ChildResult;

static int ascii_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int ascii_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

static int app_component_char(unsigned char c) {
    return ascii_alpha(c) || ascii_digit(c) || c == '_';
}

int flatpak_manager_valid_app_id(const char* value) {
    size_t length;
    size_t component = 0;
    const char* last_dot;
    int saw_dot = 0;
    if (!value) return 0;
    length = strlen(value);
    if (length < 3 || length > 255 || value[0] == '.' || value[length - 1] == '.') return 0;
    last_dot = strrchr(value, '.');
    if (!last_dot) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c == '.') {
            if (component == 0 || component > 63 || value[i - 1] == '-') return 0;
            component = 0;
            saw_dot = 1;
        } else if (app_component_char(c)) {
            if (component == 0 && !ascii_alpha(c)) return 0;
            ++component;
        } else if (c == '-' && value + i > last_dot && component > 0 &&
                i + 1 < length && value[i + 1] != '.') {
            ++component;
        } else {
            return 0;
        }
    }
    return saw_dot && component > 0 && component <= 63;
}

static int valid_ref_component(const char* begin, size_t length) {
    if (length == 0 || length > 96) return 0;
    if ((length == 1 && begin[0] == '.') ||
            (length == 2 && begin[0] == '.' && begin[1] == '.')) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)begin[i];
        if (!ascii_alpha(c) && !ascii_digit(c) && c != '_' && c != '-' && c != '.') return 0;
    }
    return 1;
}

int flatpak_manager_valid_ref(const char* value) {
    const char* first;
    const char* second;
    const char* third;
    size_t length;
    size_t id_length;
    char id[256];
    if (!value) return 0;
    length = strlen(value);
    if (length < 12 || length > 512) return 0;
    first = strchr(value, '/');
    second = first ? strchr(first + 1, '/') : NULL;
    third = second ? strchr(second + 1, '/') : NULL;
    if (!first || !second || !third || strchr(third + 1, '/')) return 0;
    if (!((size_t)(first - value) == 3 && memcmp(value, "app", 3) == 0) &&
            !((size_t)(first - value) == 7 && memcmp(value, "runtime", 7) == 0)) return 0;
    id_length = (size_t)(second - first - 1);
    if (id_length >= sizeof(id)) return 0;
    memcpy(id, first + 1, id_length);
    id[id_length] = '\0';
    return flatpak_manager_valid_app_id(id) &&
            valid_ref_component(second + 1, (size_t)(third - second - 1)) &&
            valid_ref_component(third + 1, strlen(third + 1));
}

static void set_error(FlatpakResult* result, const char* message) {
    result->ok = 0;
    free(result->error);
    result->error = strdup(message ? message : "Flatpak operation failed");
}

void flatpak_manager_result_clear(FlatpakResult* result) {
    if (!result) return;
    free(result->output);
    free(result->error);
    free(result->operation_id);
    memset(result, 0, sizeof(*result));
}

void flatpak_manager_set_callbacks(FlatpakProgressCallback progress,
        FlatpakCompleteCallback complete, void* context) {
    g_progress_callback = progress;
    g_complete_callback = complete;
    g_callback_context = context;
}

void flatpak_manager_init(void) {
    maton_udev_start();
    setenv("TMPDIR", "/data/matonos/linux/cache", 1);
    setenv("XDG_RUNTIME_DIR", "/data/matonos/linux/runtime", 1);
    setenv("FLATPAK_SYSTEM_DIR", "/data/matonos/linux/flatpak", 1);
    setenv("FLATPAK_SYSTEM_CACHE_DIR", "/data/matonos/linux/cache", 1);
    setenv("FLATPAK_USER_DIR", "/data/matonos/linux/flatpak-user", 1);
    setenv("HOME", "/data/matonos/linux/flatpak-data", 1);
    setenv("XDG_DATA_HOME", "/data/matonos/linux/flatpak-data/.local/share", 1);
}

static int valid_install_uid(int uid) {
    return uid >= 10000 && uid % 100000 >= 10000 && uid % 100000 <= 19999;
}

static int remember_runtime_install(int uid) {
    char temporary[160], path[160];
    char value[32];
    if (!valid_install_uid(uid)) { errno = EINVAL; return -1; }
    unsigned android_user = (unsigned)uid / 100000;
    snprintf(temporary, sizeof(temporary), "/data/matonos/linux/runtime/runtime-installation-%u.tmp", android_user);
    snprintf(path, sizeof(path), "/data/matonos/linux/runtime/runtime-installation-%u", android_user);
    int length = snprintf(value, sizeof(value), "%d\n", uid);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = fchmod(fd, 0600) == 0 && write(fd, value, (size_t)length) == length && fsync(fd) == 0;
    int saved = errno;
    close(fd);
    if (!ok || rename(temporary, path)) {
        if (ok) saved = errno;
        unlink(temporary);
        errno = saved;
        return -1;
    }
    return 0;
}

static int remembered_runtime_install(int app_uid, int* uid) {
    char path[160];
    char value[32];
    struct stat st;
    if (!valid_install_uid(app_uid)) { errno = EINVAL; return -1; }
    snprintf(path, sizeof(path), "/data/matonos/linux/runtime/runtime-installation-%u",
            (unsigned)app_uid / 100000);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t size = read(fd, value, sizeof(value) - 1);
    int bad = fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 1000 ||
            (st.st_mode & 0777) != 0600 || size <= 0 || size >= (ssize_t)sizeof(value) - 1;
    close(fd);
    if (bad) { errno = EINVAL; return -1; }
    value[size] = '\0';
    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || (*end != '\n' && *end != '\0') || parsed < 10000 || parsed > INT_MAX ||
            !valid_install_uid((int)parsed)) {
        errno = EINVAL;
        return -1;
    }
    *uid = (int)parsed;
    return 0;
}

static void publish_progress_line(const char* line) {
    if (g_progress_callback) g_progress_callback(line ? line : "", g_callback_context);
}

static char** flatpak_environment(const char* system_dir, const char* user_dir) {
    size_t count = 0, out = 0;
    while (environ[count]) ++count;
    char** env = calloc(count + 3, sizeof(char*));
    if (!env) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if ((system_dir && !strncmp(environ[i], "FLATPAK_SYSTEM_DIR=", 19)) ||
                (user_dir && !strncmp(environ[i], "FLATPAK_USER_DIR=", 17))) continue;
        env[out] = strdup(environ[i]);
        if (!env[out++]) goto fail;
    }
    if (system_dir && asprintf(&env[out++], "FLATPAK_SYSTEM_DIR=%s", system_dir) < 0) goto fail;
    if (user_dir && asprintf(&env[out++], "FLATPAK_USER_DIR=%s", user_dir) < 0) goto fail;
    return env;
fail:
    while (out) free(env[--out]);
    free(env);
    return NULL;
}

static void flatpak_environment_free(char** env) {
    if (!env) return;
    for (size_t i = 0; env[i]; ++i) free(env[i]);
    free(env);
}

static ChildResult run_cli_in(const char* const* args, size_t count, int progress,
        const char* system_dir, const char* user_dir) {
    ChildResult result;
    int pipes[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    char** argv;
    char** child_env = NULL;
    pid_t child = -1;
    int rc;
    int timed_out = 0;
    struct timespec deadline;
    size_t used = 0;
    char pending[4097];
    size_t pending_length = 0;
    result.status = 127;
    result.truncated = 0;
    result.output = calloc(k_output_limit + 1, 1);
    if (!result.output) return result;
    if (pipe2(pipes, O_CLOEXEC) != 0) {
        snprintf(result.output, k_output_limit + 1, "pipe failed: %s", strerror(errno));
        return result;
    }
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(pipes[0]); close(pipes[1]);
        snprintf(result.output, k_output_limit + 1, "spawn setup failed");
        return result;
    }
    rc = posix_spawnattr_init(&attributes);
    if (rc != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(pipes[0]); close(pipes[1]);
        snprintf(result.output, k_output_limit + 1, "spawn process group setup failed");
        return result;
    }
    if (posix_spawnattr_setpgroup(&attributes, 0) != 0 ||
            posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) != 0) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(pipes[0]); close(pipes[1]);
        snprintf(result.output, k_output_limit + 1, "spawn process group setup failed");
        return result;
    }
    (void)posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    (void)posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
    (void)posix_spawn_file_actions_addclose(&actions, pipes[0]);
    (void)posix_spawn_file_actions_addclose(&actions, pipes[1]);
    argv = calloc(count + 2, sizeof(char*));
    if (!argv) {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(pipes[0]); close(pipes[1]);
        snprintf(result.output, k_output_limit + 1, "spawn setup failed");
        return result;
    }
    argv[0] = (char*)k_flatpak;
    for (size_t i = 0; i < count; ++i) argv[i + 1] = (char*)args[i];
    child_env = flatpak_environment(system_dir, user_dir);
    if (!child_env) {
        free(argv);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(pipes[0]); close(pipes[1]);
        snprintf(result.output, k_output_limit + 1, "cannot prepare Flatpak environment");
        return result;
    }
    rc = posix_spawn(&child, k_flatpak, &actions, &attributes, argv, child_env);
    flatpak_environment_free(child_env);
    free(argv);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(pipes[1]);
    if (rc != 0) {
        close(pipes[0]);
        snprintf(result.output, k_output_limit + 1, "cannot start Flatpak CLI: %s", strerror(rc));
        return result;
    }
    {
        int read_flags = fcntl(pipes[0], F_GETFL);
        if (read_flags < 0 || fcntl(pipes[0], F_SETFL, read_flags | O_NONBLOCK) < 0) {
            (void)kill(-child, SIGKILL);
            close(pipes[0]);
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) { }
            snprintf(result.output, k_output_limit + 1, "cannot make Flatpak output pipe nonblocking");
            return result;
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 600;

    for (;;) {
        char buffer[4096];
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            timed_out = 1;
            (void)kill(-child, SIGKILL);
            break;
        }
        ssize_t n = read(pipes[0], buffer, sizeof(buffer));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = pipes[0], .events = POLLIN | POLLHUP };
            (void)poll(&pfd, 1, 1000);
            continue;
        }
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            result.truncated = 1;
            break;
        }
        if (used < k_output_limit) {
            size_t keep = (size_t)n;
            if (keep > k_output_limit - used) {
                keep = k_output_limit - used;
                result.truncated = 1;
            }
            memcpy(result.output + used, buffer, keep);
            used += keep;
            result.output[used] = '\0';
        } else {
            result.truncated = 1;
        }
        if (progress) {
            for (ssize_t i = 0; i < n; ++i) {
                char c = buffer[i];
                if (c == '\n' || c == '\r') {
                    pending[pending_length] = '\0';
                    publish_progress_line(pending);
                    pending_length = 0;
                } else if (pending_length < sizeof(pending) - 1) {
                    pending[pending_length++] = c;
                } else {
                    pending[pending_length] = '\0';
                    publish_progress_line(pending);
                    pending_length = 0;
                }
            }
        }
    }
    close(pipes[0]);
    if (progress && pending_length) {
        pending[pending_length] = '\0';
        publish_progress_line(pending);
    }
    {
        int status = 0;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0) result.status = 127;
        else if (timed_out) result.status = 124;
        else if (WIFEXITED(status)) result.status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) result.status = 128 + WTERMSIG(status);
        else result.status = 127;
    }
    if (timed_out) {
        const char* message = "Flatpak operation timed out after 10 minutes";
        strncpy(result.output, message, k_output_limit);
        result.truncated = 0;
    }
    return result;
}

static ChildResult run_cli(const char* const* args, size_t count, int progress) {
    return run_cli_in(args, count, progress, NULL, NULL);
}

static void result_from_child(FlatpakResult* result, ChildResult* child) {
    result->exit_code = child->status;
    result->ok = child->status == 0;
    result->output_truncated = child->truncated;
    result->output = child->output;
    child->output = NULL;
}

typedef struct PackageOperation {
    int uninstall;
    int delete_data;
    char* operation_id;
    char* ref;
    char* app_commit;
    char* runtime_ref;
    char* runtime_commit;
    char* remote;
    int app_uid;
    int runtime_uid;
} PackageOperation;

static void* package_operation_thread(void* data) {
    PackageOperation* operation = data;
    FlatpakResult result;
    ChildResult child;
    memset(&result, 0, sizeof(result));
    pthread_mutex_lock(&g_operation_mutex);
    if (operation->uninstall) {
        const char* args_with_delete[] = {"--system", "uninstall", "--delete-data", "--noninteractive", "--assumeyes", operation->ref};
        const char* args_keep_data[] = {"--system", "uninstall", "--noninteractive", "--assumeyes", operation->ref};
        child = operation->delete_data ? run_cli(args_with_delete, sizeof(args_with_delete) / sizeof(args_with_delete[0]), 1) :
                run_cli(args_keep_data, sizeof(args_keep_data) / sizeof(args_keep_data[0]), 1);
        result_from_child(&result, &child);
        free(child.output);
        if (result.ok) {
            const char* cleanup_args[] = {"--system", "uninstall", "--unused", "--noninteractive", "--assumeyes"};
            child = run_cli(cleanup_args, sizeof(cleanup_args) / sizeof(cleanup_args[0]), 1);
            result.has_unused_cleanup = 1;
            result.unused_cleanup_ok = child.status == 0;
            result.unused_cleanup_exit_code = child.status;
            if (!child.status) {
                // Keep the primary uninstall output; cleanup status is in fields above.
            } else {
                result.ok = 0;
                if (!result.error) result.error = strdup("App was removed, but unused runtime cleanup failed");
            }
            free(child.output);
        }
    } else {
        char system_dir[256], user_dir[256], error[512] = {0};
        snprintf(system_dir, sizeof(system_dir), "/data/matonos/linux/apps/%d", operation->runtime_uid);
        snprintf(user_dir, sizeof(user_dir), "/data/matonos/linux/apps/%d", operation->app_uid);
        publish_progress_line("Verifying pinned Flatpak commits and publishing runtime/app");
        int rc = flatpak_publish(operation->ref, operation->app_commit,
                operation->runtime_ref, operation->runtime_commit, operation->remote,
                "/data/matonos/linux/staging", system_dir, user_dir, error, sizeof(error));
        if (rc == 0 && remember_runtime_install(operation->runtime_uid)) {
            snprintf(error, sizeof(error), "published app but cannot record its shared runtime installation");
            rc = -1;
        }
        result.ok = rc == 0;
        result.exit_code = rc == 0 ? 0 : 1;
        if (rc != 0) result.error = strdup(error[0] ? error : "Flatpak publish pipeline failed");
        result.output = strdup(rc == 0 ? "Flatpak app and pinned runtime published" : error);
    }
    pthread_mutex_unlock(&g_operation_mutex);
    pthread_mutex_lock(&g_package_state_mutex);
    g_package_operation_active = 0;
    pthread_mutex_unlock(&g_package_state_mutex);
    result.operation_id = operation->operation_id ? strdup(operation->operation_id) : NULL;
    if (g_complete_callback) g_complete_callback(&result, g_callback_context);
    flatpak_manager_result_clear(&result);
    free(operation->ref);
    free(operation->app_commit);
    free(operation->runtime_ref);
    free(operation->runtime_commit);
    free(operation->remote);
    free(operation->operation_id);
    free(operation);
    return NULL;
}

static void start_package_operation(int uninstall, const char* ref, int delete_data,
        const char* operation_id, const char* app_commit, const char* runtime_ref,
        const char* runtime_commit, const char* remote, int app_uid, int runtime_uid,
        FlatpakResult* result) {
    PackageOperation* operation = calloc(1, sizeof(*operation));
    pthread_t thread;
    int error;
    pthread_mutex_lock(&g_package_state_mutex);
    if (g_package_operation_active) {
        pthread_mutex_unlock(&g_package_state_mutex);
        free(operation);
        set_error(result, "Another Flatpak package operation is already running");
        return;
    }
    g_package_operation_active = 1;
    pthread_mutex_unlock(&g_package_state_mutex);
    if (!operation) {
        pthread_mutex_lock(&g_package_state_mutex); g_package_operation_active = 0; pthread_mutex_unlock(&g_package_state_mutex);
        set_error(result, "cannot allocate Flatpak operation");
        return;
    }
    operation->uninstall = uninstall;
    operation->delete_data = delete_data;
    operation->app_uid = app_uid;
    operation->runtime_uid = runtime_uid;
    operation->operation_id = operation_id ? strdup(operation_id) : NULL;
    if (operation_id && !operation->operation_id) { free(operation); pthread_mutex_lock(&g_package_state_mutex); g_package_operation_active = 0; pthread_mutex_unlock(&g_package_state_mutex); set_error(result, "cannot allocate operation ID"); return; }
    operation->ref = strdup(ref);
    operation->app_commit = app_commit ? strdup(app_commit) : NULL;
    operation->runtime_ref = runtime_ref ? strdup(runtime_ref) : NULL;
    operation->runtime_commit = runtime_commit ? strdup(runtime_commit) : NULL;
    operation->remote = remote ? strdup(remote) : NULL;
    if (!operation->ref) {
        free(operation->operation_id); free(operation->app_commit); free(operation->runtime_ref);
        free(operation->runtime_commit); free(operation->remote);
        free(operation);
        pthread_mutex_lock(&g_package_state_mutex); g_package_operation_active = 0; pthread_mutex_unlock(&g_package_state_mutex);
        set_error(result, "cannot allocate Flatpak ref");
        return;
    }
    if (!uninstall && (!operation->app_commit || !operation->runtime_ref ||
            !operation->runtime_commit || !operation->remote)) {
        free(operation->ref); free(operation->operation_id); free(operation->app_commit);
        free(operation->runtime_ref); free(operation->runtime_commit); free(operation->remote); free(operation);
        pthread_mutex_lock(&g_package_state_mutex); g_package_operation_active = 0; pthread_mutex_unlock(&g_package_state_mutex);
        set_error(result, "cannot allocate pinned Flatpak publish data");
        return;
    }
    error = pthread_create(&thread, NULL, package_operation_thread, operation);
    if (error != 0) {
        free(operation->ref);
        free(operation->operation_id);
        free(operation->app_commit); free(operation->runtime_ref);
        free(operation->runtime_commit); free(operation->remote);
        free(operation);
        pthread_mutex_lock(&g_package_state_mutex); g_package_operation_active = 0; pthread_mutex_unlock(&g_package_state_mutex);
        set_error(result, "cannot start Flatpak operation thread");
        return;
    }
    (void)pthread_detach(thread);
    result->ok = 1;
    result->accepted = 1;
}

static void run_async(const char* app_id, const char* const* extra, size_t extra_count,
        FlatpakResult* result) {
    (void)app_id; (void)extra; (void)extra_count;
    set_error(result, "Launch through the signed Android stub");
}

typedef struct GraphicalChild { pid_t pid; int directory, listener, slot, x11_directory, x11_listener; MatonSessionPads *pads; char path[108], x11_path[108], x11_name[64]; } GraphicalChild;
static pthread_mutex_t g_launch_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct { char ref[512], log[512]; pid_t pid; int alive; } g_launches[128];
static int record_launch(const char* ref,const char* log,pid_t pid) {
    int slot=-1;pthread_mutex_lock(&g_launch_mutex);
    for(int i=0;i<128;i++)if(!strcmp(g_launches[i].ref,ref)){slot=i;break;}
    if(slot<0)for(int i=0;i<128;i++)if(!g_launches[i].alive){slot=i;break;}
    if(slot>=0){snprintf(g_launches[slot].ref,sizeof(g_launches[slot].ref),"%s",ref);
        snprintf(g_launches[slot].log,sizeof(g_launches[slot].log),"%s",log);
        g_launches[slot].pid=pid;g_launches[slot].alive=1;}
    pthread_mutex_unlock(&g_launch_mutex);return slot;
}
static void record_exit(int slot,pid_t pid) {
    pthread_mutex_lock(&g_launch_mutex);
    if(slot>=0&&g_launches[slot].pid==pid)g_launches[slot].alive=0;
    pthread_mutex_unlock(&g_launch_mutex);
}
static void read_launch_status(const char* ref,FlatpakResult* result) {
    if(!flatpak_manager_valid_ref(ref)){set_error(result,"Invalid application reference");return;}
    int alive=0;char path[512]={0};pthread_mutex_lock(&g_launch_mutex);
    for(int i=0;i<128;i++)if(!strcmp(g_launches[i].ref,ref)){
        alive=g_launches[i].alive;snprintf(path,sizeof(path),"%s",g_launches[i].log);break;}
    pthread_mutex_unlock(&g_launch_mutex);
    if(alive){result->ok=1;return;}
    char message[4096]="Application exited before opening a window";
    FILE* log=path[0]?fopen(path,"re"):NULL;
    if(log){size_t count=fread(message,1,sizeof(message)-1,log);message[count]=0;fclose(log);}
    set_error(result,message[0]?message:"Application exited before opening a window");
}
#include "../../linux/flatpak/socket-relay.h"

static void close_graphical_sockets(int listener,const char* path,int directory,
        int x11_listener,const char* x11_path,int x11_directory) {
    if(listener>=0)close(listener);if(path[0])unlink(path);if(directory>=0)close(directory);
    if(x11_listener>=0)close(x11_listener);if(x11_path[0])unlink(x11_path);if(x11_directory>=0)close(x11_directory);
}
static void* reap_graphical(void* argument) {
    GraphicalChild* child=argument;
    unsigned connections=0;
    for(;;) {
        int status;pid_t exited=waitpid(child->pid,&status,WNOHANG);
        if(exited==child->pid || (exited<0 && errno!=EINTR))break;
        struct pollfd listeners[2]={{.fd=child->listener,.events=POLLIN},{.fd=child->x11_listener,.events=POLLIN}};
        if(poll(listeners,2,1000)<=0)continue;
        for(int i=0;i<2;++i) {
            if(!(listeners[i].revents&POLLIN))continue;
            int client=accept4(listeners[i].fd,NULL,NULL,SOCK_CLOEXEC);
            if(client<0)continue;
            if(connections++>=128){close(client);continue;}
            int upstream=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
            struct sockaddr_un address={.sun_family=AF_UNIX};
            snprintf(address.sun_path,sizeof(address.sun_path),"/proc/self/fd/%d/%s",i?child->x11_directory:child->directory,i?child->x11_name:"wayland-0");
            if(upstream<0 || connect(upstream,(struct sockaddr*)&address,sizeof(address))) {
                if(upstream>=0)close(upstream);close(client);continue;
            }
            struct timeval timeout={.tv_sec=5};
            setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
            setsockopt(upstream,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
            WaylandRelay* relay=malloc(sizeof(*relay));pthread_t thread;
            if(relay){relay->client=client;relay->compositor=upstream;}
            pthread_attr_t attributes;pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,256*1024);
            int error=relay?pthread_create(&thread,&attributes,relay_wayland,relay):ENOMEM;
            pthread_attr_destroy(&attributes);
            if(error){free(relay);close(client);close(upstream);}else pthread_detach(thread);
        }
    }
    maton_pads_destroy(child->pads);
    record_exit(child->slot,child->pid);
    close_graphical_sockets(child->listener,child->path,child->directory,child->x11_listener,child->x11_path,child->x11_directory);free(child);return NULL;
}

/* Android mounts a shell-owned debug tmpfs at /tmp. Apps that request
 * filesystems=/tmp (Brave, Chromium-based apps) would get it bound into the
 * sandbox, where bwrap, running as system, cannot create flatpak's
 * /tmp/.X11-unix mount point. A system-wide override gives every sandbox a
 * private /tmp instead; applied once, it lives in the Flatpak installation. */
static void deny_host_tmp(void) {
    static int applied;
    if (applied) return;
    char buffer[4096] = {0};
    int fd = open("/data/matonos/linux/flatpak/overrides/global", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) { ssize_t got = read(fd, buffer, sizeof(buffer) - 1); close(fd); if (got > 0 && strstr(buffer, "!/tmp")) { applied = 1; return; } }
    const char* args[] = {"--system", "override", "--nofilesystem=/tmp"};
    ChildResult child = run_cli(args, 3, 0);
    if (child.status == 0) applied = 1;
    else __android_log_print(ANDROID_LOG_WARN, "matonos-linuxd", "cannot apply the global /tmp override (status %d)", child.status);
    free(child.output);
}

/* Exact Context/devices tokens; no app-specific policy or override files. */
static int declared_controllers(const char* metadata, int* all_devices) {
    char* copy = strdup(metadata ? metadata : "");
    if (!copy) return 0;
    int context = 0, controllers = 0;
    char* state;
    for (char* line = strtok_r(copy, "\n", &state); line; line = strtok_r(NULL, "\n", &state)) {
        while (*line == ' ' || *line == '\t') ++line;
        size_t length = strlen(line);
        while (length && strchr(" \t\r", line[length-1])) line[--length] = 0;
        if (*line == '[') { context = !strcmp(line, "[Context]"); continue; }
        char* equals = strchr(line, '=');
        if (!context || !equals) continue;
        char* key_end = equals;
        while (key_end > line && strchr(" \t", key_end[-1])) --key_end;
        if (key_end-line != 7 || strncmp(line, "devices", 7)) continue;
        controllers = 0; *all_devices = 0;
        char* tokens;
        for (char* token = strtok_r(equals+1, ";", &tokens); token; token = strtok_r(NULL, ";", &tokens)) {
            while (*token == ' ' || *token == '\t') ++token;
            length = strlen(token);
            while (length && strchr(" \t\r", token[length-1])) token[--length] = 0;
            if (!strcmp(token, "all")) { controllers = 1; *all_devices = 1; }
            if (!strcmp(token, "input")) controllers = 1;
        }
    }
    free(copy); return controllers;
}

static int owned_directory(const char* path,int uid) {
    char copy[512];if(strlen(path)>=sizeof(copy))return -1;strcpy(copy,path);
    int fd=open("/",O_PATH|O_DIRECTORY|O_CLOEXEC);char* save=NULL;
    for(char* part=strtok_r(copy,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
        if(!strcmp(part,".")||!strcmp(part,"..")){close(fd);return -1;}
        int next=openat(fd,part,O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(fd);fd=next;
        if(fd<0)return -1;
    }
    struct stat st;if(fstat(fd,&st) || (uid>=0 && st.st_uid!=(uid_t)uid)){close(fd);return -1;}
    return fd;
}
/* linux-data: no caller-selected path and no symlink traversal. fscreate is
 * thread-local, so concurrent Binder calls cannot exchange MLS categories. */
static int create_context(const char* label) {
    int fd=open("/proc/thread-self/attr/fscreate",O_WRONLY|O_CLOEXEC);
    if(fd<0)return -1;
    size_t n=label?strlen(label):0;
    int rc=write(fd,label?label:"",n)==(ssize_t)n?0:-1;close(fd);return rc;
}
static int data_child(int parent,const char* name,int uid,const char* label) {
    int made=mkdirat(parent,name,0700)==0;
    if(!made&&errno!=EEXIST)return -1;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    /* AOSP: libcutils/private/android_projectid_config.h PROJECT_ID_APP_START
     * = 50000; installd/utils.cpp:438-440 uses uid - 10000 + range start.
     * Set before chown while linuxd owns the new directory. Existing trees
     * inherit their original project ID; dev builds require no migration. */
    struct fsxattr attrs={0};
    if(made && ioctl(fd,FS_IOC_FSGETXATTR,&attrs)==0) {
        attrs.fsx_projid=(unsigned)uid-10000+50000;
        attrs.fsx_xflags|=FS_XFLAG_PROJINHERIT;
        if(ioctl(fd,FS_IOC_FSSETXATTR,&attrs))
            __android_log_print(ANDROID_LOG_WARN,"matonos-linuxd","project quota unavailable uid=%d: %s",uid,strerror(errno));
    } else if(made) __android_log_print(ANDROID_LOG_WARN,"matonos-linuxd","project quota unsupported uid=%d",uid);
    struct stat st;char context[256]={0};
    if((made&&fchown(fd,uid,uid))||fstat(fd,&st)||st.st_uid!=(uid_t)uid||
       (st.st_mode&0777)!=0700||fgetxattr(fd,"security.selinux",context,sizeof(context)-1)<=0||
       strcmp(context,label)){close(fd);errno=EPERM;return -1;}
    return fd;
}
static int prepare_linux_data(int uid,int pid,const char* ref) {
    char level[64],label[160],path[64],process[256]={0};struct stat st;
    if(uid%100000<10000||uid%100000>19999||maton_mls_level_from_uid(uid,level,sizeof(level)))return -1;
    snprintf(path,sizeof(path),"/proc/%d",pid);
    if(stat(path,&st)||st.st_uid!=(uid_t)uid)return -1;
    snprintf(path,sizeof(path),"/proc/%d/attr/current",pid);
    int proc=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(proc<0)return -1;
    ssize_t got=read(proc,process,sizeof(process)-1);close(proc);if(got<=0)return -1;
    process[strcspn(process,"\n")]=0;
    char* range=strstr(process,":s0");if(!range||strcmp(range+1,level))return -1;
    snprintf(label,sizeof(label),"u:object_r:matonos_linux_data_file:%s",level);
    int root=owned_directory("/data/matonos/linux/apps",1000);
    if(root<0)return -1;
    char name[32];snprintf(name,sizeof(name),"%d",uid);
    /* Retain the existing system-owned UID owner record: a recycled UID
     * must never inherit another Flatpak's home. The stub chooses no path. */
    char record_name[48],id[256];
    const char* end=strchr(ref+4,'/');size_t length=end?(size_t)(end-ref-4):0;
    if(!length||length>=sizeof(id)){close(root);return -1;}
    memcpy(id,ref+4,length);id[length]=0;
    snprintf(record_name,sizeof(record_name),"%d.owner",uid);
    int record=openat(root,record_name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0640);
    if(record>=0) {
        int bad=write(record,id,length)!=(ssize_t)length;close(record);
        if(bad){close(root);return -1;}
    } else {
        if(errno!=EEXIST){close(root);return -1;}
        record=openat(root,record_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        char stored[256]={0};struct stat owner;
        ssize_t n=record>=0?read(record,stored,sizeof(stored)-1):-1;
        int bad=record<0||fstat(record,&owner)||!S_ISREG(owner.st_mode)||
            owner.st_uid!=1000||n!=(ssize_t)length||memcmp(stored,id,length);
        if(record>=0)close(record);
        if(bad){close(root);errno=EPERM;return -1;}
    }
    int app=-1,home=-1,rc=-1;
    if(create_context(label))goto done;
    app=data_child(root,name,uid,label);if(app<0)goto done;
    home=data_child(app,"home",uid,label);if(home<0)goto done;
    rc=0;
done:
    (void)create_context(NULL);
    if(home>=0)close(home);if(app>=0)close(app);close(root);
    return rc;
}
/* Never follow symlinks or cross a mount while deleting an orphan. */
static int remove_contents(int fd,dev_t device) {
    DIR* dir=fdopendir(dup(fd));if(!dir)return -1;
    struct dirent* entry;int rc=0;
    while((entry=readdir(dir))) {
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        struct stat st;
        if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)){rc=-1;break;}
        if(S_ISDIR(st.st_mode)) {
            int child=openat(fd,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            struct stat pinned;
            if(child<0){rc=-1;break;}
            if(fstat(child,&pinned)||pinned.st_dev!=device||remove_contents(child,device))rc=-1;
            close(child);
            if(rc||unlinkat(fd,entry->d_name,AT_REMOVEDIR)){rc=-1;break;}
        }else if(unlinkat(fd,entry->d_name,0)){rc=-1;break;}
    }
    closedir(dir);return rc;
}
int flatpak_manager_delete_data(int uid) {
    if(uid%100000<10000||uid%100000>19999){errno=EINVAL;return -1;}
    int root=owned_directory("/data/matonos/linux/apps",1000);if(root<0)return -1;
    char name[32];snprintf(name,sizeof(name),"%d",uid);
    int app=openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(app<0){
        int rc=-1;
        if(errno==ENOENT){snprintf(name,sizeof(name),"%d.owner",uid);rc=unlinkat(root,name,0);if(rc&&errno==ENOENT)rc=0;}
        close(root);return rc;
    }
    struct stat st;int rc=fstat(app,&st)||st.st_uid!=(uid_t)uid?-1:remove_contents(app,st.st_dev);
    close(app);if(!rc)rc=unlinkat(root,name,AT_REMOVEDIR);
    if(!rc){snprintf(name,sizeof(name),"%d.owner",uid);if(unlinkat(root,name,0)&&errno!=ENOENT)rc=-1;}
    close(root);return rc;
}

/* The compositor delegates only its socket directories, never its app data root. */
void flatpak_manager_launch_graphical(const char* ref, int runtime_directory_fd, const char* dns_servers, int x11_directory_fd, const char* x11_display, int game_controllers, int stub_uid, int stub_pid, int lifeline_fd, FlatpakResult* result) {
    struct stat directory, socket_info;
    int runtime_uid = -1;
    char system_install[128], user_install[128];
    if (stub_uid < 10000 || stub_pid <= 0 || lifeline_fd < 0) {
        set_error(result, "verified stub process required"); return;
    }
    if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4) != 0) {
        set_error(result, "valid installed application ref required"); return;
    }
    if (!valid_install_uid(stub_uid) || remembered_runtime_install(stub_uid, &runtime_uid) ||
            runtime_uid / 100000 != stub_uid / 100000) {
        set_error(result, "shared runtime installation is unavailable"); return;
    }
    snprintf(system_install, sizeof(system_install), "/data/matonos/linux/apps/%d", runtime_uid);
    snprintf(user_install, sizeof(user_install), "/data/matonos/linux/apps/%d", stub_uid);
    bool has_x11 = x11_display && x11_display[0];
    struct stat x11_directory;
    if (has_x11 && (x11_display[0] != 'X' || !x11_display[1] || strlen(x11_display) >= 64 ||
            strspn(x11_display + 1, "0123456789") != strlen(x11_display + 1))) {
        set_error(result, "invalid X11 socket name"); return;
    }
    if (has_x11 && (fstat(x11_directory_fd, &x11_directory) || !S_ISDIR(x11_directory.st_mode) ||
            fstatat(x11_directory_fd, x11_display, &socket_info, AT_SYMLINK_NOFOLLOW) ||
            !S_ISSOCK(socket_info.st_mode) || socket_info.st_uid != x11_directory.st_uid)) {
        /* A dead Xwayland must not block the app: launch Wayland-only. */
        __android_log_print(ANDROID_LOG_WARN, "matonos-linuxd", "X11 socket %s unavailable; launching %s without X11", x11_display, ref);
        has_x11 = false;
    }
    if (fstat(runtime_directory_fd, &directory) || !S_ISDIR(directory.st_mode) ||
            fstatat(runtime_directory_fd, "wayland-0", &socket_info, AT_SYMLINK_NOFOLLOW) ||
            !S_ISSOCK(socket_info.st_mode) || socket_info.st_uid != directory.st_uid) {
        set_error(result, "compositor socket directory is unavailable"); return;
    }
    if (prepare_linux_data(stub_uid,stub_pid,ref)) {
        set_error(result,"cannot prepare verified Linux data");return;
    }
    /* Check the full installed ref rather than accepting arbitrary commands. */
    const char* info_args[] = {"--user", "info", ref};
    ChildResult installed = run_cli_in(info_args, 3, 0, system_install, user_install);
    if (installed.status != 0) { result_from_child(result, &installed); return; }
    free(installed.output);
    const char* metadata_args[] = {"--user", "info", "--show-metadata", ref};
    ChildResult metadata = run_cli_in(metadata_args, 4, 0, system_install, user_install);
    int controllers = 0, all_devices = 0;
    if (metadata.status == 0 && !metadata.truncated)
        controllers = declared_controllers(metadata.output, &all_devices) && game_controllers;
    free(metadata.output);
    deny_host_tmp();
    /* Keep sources above the fixed child slots 198/199 so spawn dup2 actions
     * cannot overwrite another capability before it has been copied. */
    int capability = fcntl(runtime_directory_fd, F_DUPFD_CLOEXEC, 200);
    if (capability < 0) { set_error(result, "cannot duplicate compositor directory"); return; }
    int x11_capability = has_x11 ? fcntl(x11_directory_fd, F_DUPFD_CLOEXEC, 200) : -1;
    if(has_x11 && x11_capability<0){close(capability);set_error(result,"cannot duplicate X11 directory");return;}
    int x11_listener = -1;char x11_path[108]={0};
    int listener = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    char socket_path[108];
    snprintf(socket_path,sizeof(socket_path),"/data/matonos/linux/runtime/wayland-%d",capability);
    struct sockaddr_un address = {.sun_family=AF_UNIX};
    snprintf(address.sun_path,sizeof(address.sun_path),"%s",socket_path);
    unlink(socket_path);
    if (listener < 0 || bind(listener,(struct sockaddr*)&address,sizeof(address)) || listen(listener,16)) {
        close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability);set_error(result,"cannot create Wayland relay socket");return;
    }
    if(has_x11) {
        snprintf(x11_path,sizeof(x11_path),"%.*s-x11",(int)sizeof(x11_path)-5,socket_path);
        x11_listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        snprintf(address.sun_path,sizeof(address.sun_path),"%s",x11_path);
        unlink(x11_path);
        if(x11_listener<0 || bind(x11_listener,(struct sockaddr*)&address,sizeof(address)) || listen(x11_listener,16)) {
            close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability);
            set_error(result,"cannot create X11 relay socket");return;
        }
    }
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "/data/matonos/linux/cache/launch-%.*s.log",
            (int)(strchr(ref + 4, '/') - (ref + 4)), ref + 4);
    int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) { close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability); if (logfd >= 0) close(logfd); set_error(result, "Flatpak launch setup failed"); return; }
    rc = 0;
    if (!rc && logfd >= 0) rc = posix_spawn_file_actions_adddup2(&actions, logfd, STDOUT_FILENO);
    if (!rc && logfd >= 0) rc = posix_spawn_file_actions_adddup2(&actions, logfd, STDERR_FILENO);
    if (!rc && logfd >= 0) rc = posix_spawn_file_actions_addclose(&actions, logfd);
    /* Delegate only the already validated session directory to the wrapper.
     * It connects the broker control socket, then closes this FD before CLI exec. */
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, capability, 198);
    int owner_fd = fcntl(lifeline_fd, F_DUPFD_CLOEXEC, 200);
    if (owner_fd < 0) rc = errno;
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, owner_fd, 196);
    if (!rc && has_x11) rc = posix_spawn_file_actions_adddup2(&actions, x11_capability, 199);
    /* Phase 1: no Android inventory relay yet; fail closed with no pads. */
    MatonSessionPads *pads=maton_pads_create(NULL,0,(unsigned)stub_uid,NULL,NULL);
    if(!pads)rc=errno;
    char pad_env[320];
    snprintf(pad_env,sizeof(pad_env),"MATON_SESSION_PAD_NODES=%s",maton_pads_nodes(pads));
    char owner_env[384];
    snprintf(owner_env,sizeof(owner_env),"MATON_APP_OWNER=%d:%d:%d:%.*s",stub_uid,stub_pid,controllers,(int)(strchr(ref+4,'/')-ref-4),ref+4);
    char display_env[128];
    snprintf(display_env,sizeof(display_env),"WAYLAND_DISPLAY=%s",socket_path);
    char dns_env[2048];
    snprintf(dns_env,sizeof(dns_env),"MATON_FLATPAK_DNS=%s",dns_servers ? dns_servers : "");
    char x11_env[160];
    snprintf(x11_env,sizeof(x11_env),"MATON_SESSION_X11_NAME=%s",has_x11?x11_display:"");
    char system_install_env[160], user_install_env[160];
    snprintf(system_install_env, sizeof(system_install_env), "FLATPAK_SYSTEM_DIR=%s", system_install);
    snprintf(user_install_env, sizeof(user_install_env), "FLATPAK_USER_DIR=%s", user_install);
    size_t env_count = 0;
    while (environ[env_count]) ++env_count;
    char** env = calloc(env_count + 14, sizeof(char*));
    size_t n = 0;
    if (env) {
        for (size_t i = 0; i < env_count; ++i)
            if (strncmp(environ[i], "WAYLAND_DISPLAY=", 16) != 0 &&
                    strncmp(environ[i],"MATON_FLATPAK_DNS=",18) != 0 &&
                    strncmp(environ[i],"DISPLAY=",8) != 0 &&
                    strncmp(environ[i],"MATON_GAME_CONTROLLERS=",23) != 0 &&
                    strncmp(environ[i],"MATON_SESSION_DIRECTORY_FD=",27) != 0 &&
                    strncmp(environ[i],"MATON_SESSION_X11_",18) != 0 &&
                    strncmp(environ[i],"MATON_SESSION_PAD_NODES=",24) != 0 &&
                    strncmp(environ[i],"FLATPAK_SYSTEM_DIR=",19) != 0 &&
                    strncmp(environ[i],"FLATPAK_USER_DIR=",17) != 0) env[n++] = environ[i];
        env[n++] = system_install_env;
        env[n++] = user_install_env;
        env[n++] = pad_env;
        env[n++] = owner_env;
        env[n++] = "MATON_APP_LIFELINE=196";
        env[n++] = display_env;
        env[n++] = dns_env;
        env[n++] = "MATON_SESSION_DIRECTORY_FD=198";
        if(has_x11) {env[n++]="MATON_SESSION_X11_FD=199";env[n++]=x11_env;}
        env[n] = controllers ? "MATON_GAME_CONTROLLERS=1" : "MATON_GAME_CONTROLLERS=0";
    } else rc = ENOMEM;
    /* Both display sockets are granted; toolkits choose their backend.
     * Android's caption bar is the only window decoration, so toolkits that
     * ignore the decoration protocol are told to drop their own. The GPU's
     * render node is passed in: Wayland clients render on it and hand their
     * dma-bufs to the compositor, which shows them without a copy. */
    char* argv[] = {(char*)k_flatpak, "--user", "run",
            "--socket=wayland", "--socket=x11", "--no-documents-portal",
            /* Session-bus access is the installation's global override.
             * Keep per-app permissions free of this launch-chain lever. */
            "--no-a11y-bus",
            "--nodevice=all", "--device=dri",
            "--env=QT_WAYLAND_DISABLE_WINDOWDECORATION=1", "--env=GTK_CSD=0",
            "--env=NO_AT_BRIDGE=1",
            (char*)ref + 4, NULL};
    pid_t child = -1;
    if (!rc) rc = posix_spawn(&child, k_flatpak, &actions, NULL, argv, env);
    posix_spawn_file_actions_destroy(&actions);
    if (owner_fd >= 0) close(owner_fd);
    if (logfd >= 0) close(logfd); free(env);
    if (rc) { maton_pads_destroy(pads); close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability); set_error(result, strerror(rc)); return; }
    int launch_slot=record_launch(ref,log_path,child);
    if(launch_slot<0){maton_pads_destroy(pads);kill(child,SIGTERM);while(waitpid(child,NULL,0)<0&&errno==EINTR){}close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability);set_error(result,"Too many active launches");return;}
    /* Report immediate CLI failures to the launch Activity instead of a blank window. */
    for (int i = 0; i < 8; ++i) {
        int status; pid_t exited = waitpid(child, &status, WNOHANG);
        if (exited == child) {
            maton_pads_destroy(pads);
            record_exit(launch_slot,child);
            FILE* log = fopen(log_path, "re");
            char message[4096] = "Flatpak exited before opening a window";
            if (log) { size_t count = fread(message, 1, sizeof(message)-1, log); message[count] = 0; fclose(log); }
            close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability);
            if (WIFEXITED(status) && WEXITSTATUS(status)==0) { result->ok=1; result->exit_code=0; return; }
            set_error(result, message[0] ? message : "Flatpak exited before opening a window"); return;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 250000000}; nanosleep(&delay, NULL);
    }
    GraphicalChild* state = malloc(sizeof(*state));
    pthread_t reaper;
    if (state) { state->pads=pads; state->pid = child; state->slot=launch_slot; state->directory = capability; state->listener = listener; state->x11_directory=x11_capability; state->x11_listener=x11_listener;
        snprintf(state->x11_path,sizeof(state->x11_path),"%s",x11_path);snprintf(state->x11_name,sizeof(state->x11_name),"%s",has_x11?x11_display:""); snprintf(state->path,sizeof(state->path),"%s",socket_path); }
    if (!state || pthread_create(&reaper, NULL, reap_graphical, state)) {
        maton_pads_destroy(pads);record_exit(launch_slot,child);free(state); close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability); kill(child, SIGTERM); while (waitpid(child, NULL, 0) < 0 && errno == EINTR) { }
        set_error(result, "cannot reap Flatpak process"); return;
    }
    pthread_detach(reaper);
    result->ok = 1; result->has_pid = 1; result->pid = (long)child;
}

/* Return only an installed app's exported desktop entry to the bridge. */
static void read_desktop_entry(const char* ref, FlatpakResult* result) {
    if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4) != 0) {
        set_error(result, "valid installed application ref required"); return;
    }
    const char* args[] = {"--system", "info", "--show-location", ref};
    ChildResult child = run_cli(args, 4, 0);
    if (child.status != 0) {
        result_from_child(result, &child); return;
    }
    if (child.truncated || !child.output) {
        free(child.output); set_error(result, "invalid application location response"); return;
    }
    child.output[strcspn(child.output, "\r\n")] = 0;
    char app_id[256], path[4096];
    const char* slash = strchr(ref + 4, '/');
    size_t length = (size_t)(slash - (ref + 4));
    if (length >= sizeof(app_id)) { free(child.output); set_error(result, "application ID too long"); return; }
    memcpy(app_id, ref + 4, length); app_id[length] = 0;
    if (strncmp(child.output, "/data/matonos/linux/flatpak/app/", strlen("/data/matonos/linux/flatpak/app/")) != 0 ||
            snprintf(path, sizeof(path), "%s/export/share/applications/%s.desktop", child.output, app_id) >= (int)sizeof(path)) {
        free(child.output); set_error(result, "invalid application location"); return;
    }
    char root[4096], resolved[4096];
    if (!realpath(child.output, root) ||
            strncmp(root, "/data/matonos/linux/flatpak/app/", strlen("/data/matonos/linux/flatpak/app/")) ||
            !realpath(path, resolved) ||
            strncmp(resolved, root, strlen(root)) != 0 || resolved[strlen(root)] != '/') {
        free(child.output); set_error(result, "desktop entry must remain inside its installed deployment"); return;
    }
    free(child.output);
    FILE* file = safe_fopen_absolute(resolved);
    if (!file) { set_error(result, "installed application has no exported desktop entry"); return; }
    // Localized desktop entries (for example Firefox) exceed the CLI log limit.
    const size_t desktop_limit = 128 * 1024;
    char* contents = calloc(desktop_limit + 1, 1);
    if (!contents) { fclose(file); set_error(result, "cannot allocate desktop entry"); return; }
    size_t count = fread(contents, 1, desktop_limit, file);
    int failed = ferror(file) || (count == desktop_limit && fgetc(file) != EOF);
    fclose(file);
    if (failed) { free(contents); set_error(result, "desktop entry is unreadable or too large"); return; }
    result->output = contents; result->ok = 1; result->exit_code = 0;
}

static void read_exported_icon(const char* ref, FlatpakResult* result) {
    if (!flatpak_manager_valid_ref(ref) || strncmp(ref,"app/",4) != 0) {
        set_error(result,"valid installed application ref required"); return;
    }
    const char* args[] = {"--system","info","--show-location",ref};
    ChildResult location = run_cli(args,4,0);
    if (location.status != 0) { result_from_child(result,&location); return; }
    if (!location.output || location.truncated) { free(location.output); set_error(result,"invalid deployment location"); return; }
    location.output[strcspn(location.output,"\r\n")] = 0;
    char root[4096];
    if (strncmp(location.output,"/data/matonos/linux/flatpak/app/",strlen("/data/matonos/linux/flatpak/app/")) || !realpath(location.output,root) ||
            strncmp(root,"/data/matonos/linux/flatpak/app/",strlen("/data/matonos/linux/flatpak/app/"))) {
        free(location.output); set_error(result,"invalid deployment location"); return;
    }
    free(location.output);
    const char* sizes[] = {"256x256","128x128","64x64","48x48","512x512","32x32"};
    const char* end = strchr(ref+4,'/');
    char path[4096], resolved[4096]; FILE* file = NULL;
    for (size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        int length = snprintf(path,sizeof(path),"%s/export/share/icons/hicolor/%s/apps/%.*s.png",root,sizes[i],(int)(end-ref-4),ref+4);
        if (length<0 || length>=(int)sizeof(path) || !realpath(path,resolved)) continue;
        if (strncmp(resolved,root,strlen(root)) || resolved[strlen(root)]!='/') continue;
        struct stat metadata;
        if (stat(resolved,&metadata) || !S_ISREG(metadata.st_mode) || metadata.st_size<8 || metadata.st_size>256*1024) continue;
        file=safe_fopen_absolute(resolved); if (file) break;
    }
    /* appstream-compose writes flatpak-context PNGs into every deployment it
     * processed; apps exporting only SVG (Brave) still have icons here. */
    if (!file) {
        const char* flat_sizes[] = {"512x512","256x256","128x128","128x128@2","64x64","64x64@2","48x48","32x32"};
        for (size_t i=0;i<sizeof(flat_sizes)/sizeof(flat_sizes[0]) && !file;++i) {
            int length = snprintf(path,sizeof(path),"%s/files/share/app-info/icons/flatpak/%s/%.*s.png",root,flat_sizes[i],(int)(end-ref-4),ref+4);
            if (length<0 || length>=(int)sizeof(path) || !realpath(path,resolved)) continue;
            if (strncmp(resolved,root,strlen(root)) || resolved[strlen(root)]!='/') continue;
            struct stat metadata;
            if (stat(resolved,&metadata) || !S_ISREG(metadata.st_mode) || metadata.st_size<8 || metadata.st_size>256*1024) continue;
            file=safe_fopen_absolute(resolved);
        }
    }
    /* AppStream supplies PNG thumbnails when an app exports only SVG. */
    if (!file) {
        // Some deployments bundle their AppStream PNG alongside their SVG export.
        char app_id[256], media[4096];
        size_t app_length=(size_t)(end-ref-4);
        if(app_length<sizeof(app_id)) {
            memcpy(app_id,ref+4,app_length);app_id[app_length]=0;
            for(size_t i=0;i<app_length;i++)if(app_id[i]=='.')app_id[i]='/';
            int length=snprintf(media,sizeof(media),"%s/files/share/app-info/media/%s",root,app_id);
            DIR* directory=length>0 && length<(int)sizeof(media) ? safe_opendir_absolute(media) : NULL;
            if(directory) {
                struct dirent* entry;unsigned visited=0;
                while(!file && visited++<256 && (entry=readdir(directory))) {
                    if(entry->d_name[0]=='.')continue;
                    const char* thumbnails[]={"128x128@2","128x128","64x64"};
                    for(size_t i=0;i<3;i++) {
                        length=snprintf(path,sizeof(path),"%s/%s/icons/%s/%.*s.png",media,entry->d_name,thumbnails[i],(int)app_length,ref+4);
                        if(length<0 || length>=(int)sizeof(path) || !realpath(path,resolved))continue;
                        if(strncmp(resolved,root,strlen(root)) || resolved[strlen(root)]!='/')continue;
                        struct stat metadata;
                        if(stat(resolved,&metadata)||!S_ISREG(metadata.st_mode)||metadata.st_size<8||metadata.st_size>256*1024)continue;
                        file=safe_fopen_absolute(resolved);if(file)break;
                    }
                }
                closedir(directory);
            }
        }
    }
    if (!file) {
        char metadata_root[4096];
        const char* arch_end=strchr(end+1,'/');
        const char* thumbnails[]={"128x128","64x64"};
        if (arch_end && realpath("/data/matonos/linux/flatpak/appstream/flathub",metadata_root) &&
                !strncmp(metadata_root,"/data/matonos/linux/flatpak/appstream/",strlen("/data/matonos/linux/flatpak/appstream/"))) {
            for(size_t i=0;i<2;++i) {
                int length=snprintf(path,sizeof(path),"%s/%.*s/active/icons/%s/%.*s.png",metadata_root,(int)(arch_end-end-1),end+1,thumbnails[i],(int)(end-ref-4),ref+4);
                if(length<0 || length>=(int)sizeof(path) || !realpath(path,resolved))continue;
                if(strncmp(resolved,metadata_root,strlen(metadata_root)) || resolved[strlen(metadata_root)]!='/')continue;
                struct stat metadata;
                if(stat(resolved,&metadata) || !S_ISREG(metadata.st_mode) || metadata.st_size<8 || metadata.st_size>256*1024)continue;
                file=safe_fopen_absolute(resolved);if(file)break;
            }
        }
    }
    if (!file) { set_error(result,"application has no exported PNG icon"); return; }
    unsigned char* bytes=malloc(256*1024+1);
    if (!bytes) { fclose(file); set_error(result,"cannot allocate icon"); return; }
    size_t count=fread(bytes,1,256*1024+1,file);
    int failed=ferror(file); fclose(file);
    if (failed || count<8 || count>256*1024 || memcmp(bytes,"\x89PNG\r\n\x1a\n",8)) {
        free(bytes); set_error(result,"invalid exported PNG icon"); return;
    }
    const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char* encoded=calloc(4*((count+2)/3)+1,1);
    if (!encoded) { free(bytes); set_error(result,"cannot encode icon"); return; }
    size_t j=0;
    for (size_t i=0;i<count;i+=3) {
        unsigned value=(unsigned)bytes[i]<<16;
        if (i+1<count) value|=(unsigned)bytes[i+1]<<8;
        if (i+2<count) value|=bytes[i+2];
        encoded[j++]=alphabet[(value>>18)&63]; encoded[j++]=alphabet[(value>>12)&63];
        encoded[j++]=i+1<count?alphabet[(value>>6)&63]:'=';
        encoded[j++]=i+2<count?alphabet[value&63]:'=';
    }
    free(bytes); result->output=encoded; result->ok=1; result->exit_code=0;
}

void flatpak_manager_call(const char* command, const char* ref, const char* app_id,
        const char* const* run_args, size_t run_arg_count, int delete_data,
        const char* operation_id, const char* app_commit, const char* runtime_ref,
        const char* runtime_commit, const char* remote, int app_uid, int runtime_uid,
        FlatpakResult* result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->exit_code = 127;
    if (!command) { set_error(result, "unsupported Flatpak command"); return; }
    if (strcmp(command, "list_installed") == 0 || strcmp(command, "list_remotes") == 0 ||
            strcmp(command, "add_flathub") == 0) {
        ChildResult child;
        int changes_remote = strcmp(command, "add_flathub") == 0;
        // Flatpak supports listing the committed state during a transaction.
        // Never queue a Binder caller behind a ten-minute install.
        if (changes_remote && pthread_mutex_trylock(&g_operation_mutex) != 0) {
            set_error(result, "A Flatpak operation is already running. Try again when it finishes.");
            return;
        }
        if (strcmp(command, "list_installed") == 0) {
            const char* args[] = {"--system", "list", "--app", "--columns=ref"};
            child = run_cli(args, sizeof(args) / sizeof(args[0]), 0);
        } else if (strcmp(command, "list_remotes") == 0) {
            const char* args[] = {"--system", "remotes", "--show-details"};
            child = run_cli(args, sizeof(args) / sizeof(args[0]), 0);
        } else {
            /* remote-add accepts neither transaction flag; --from imports the
             * repository URL and signing key from the .flatpakrepo file. */
            const char* args[] = {"--system", "remote-add", "--if-not-exists", "--from",
                    "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo"};
            child = run_cli(args, sizeof(args) / sizeof(args[0]), 0);
        }
        if (changes_remote) pthread_mutex_unlock(&g_operation_mutex);
        result_from_child(result, &child);
        free(child.output);
    } else if (strcmp(command, "launch_status") == 0) {
        read_launch_status(ref,result);
    } else if (strcmp(command, "icon") == 0) {
        read_exported_icon(ref, result);
    } else if (strcmp(command, "metadata") == 0) {
        if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4)) {
            set_error(result, "valid application ref required"); return;
        }
        const char* args[] = {"--system", "info", "--show-metadata", ref};
        ChildResult child = run_cli(args, 4, 0);
        result_from_child(result, &child); free(child.output);
    } else if (strcmp(command, "desktop_entry") == 0) {
        read_desktop_entry(ref, result);
    } else if (strcmp(command, "install") == 0 || strcmp(command, "uninstall") == 0) {
        if (!flatpak_manager_valid_ref(ref)) set_error(result, "valid complete Flatpak ref required");
        else if (strcmp(command, "install") == 0 && (!flatpak_manager_valid_ref(runtime_ref) ||
                strncmp(runtime_ref, "runtime/", 8) || !remote || !*remote || strlen(remote) > 128 ||
                !valid_install_uid(app_uid) || !valid_install_uid(runtime_uid) ||
                app_uid / 100000 != runtime_uid / 100000))
            set_error(result, "valid pinned runtime ref, remote, and same-user installation UIDs are required");
        else start_package_operation(strcmp(command, "uninstall") == 0, ref, delete_data,
                operation_id, app_commit, runtime_ref, runtime_commit, remote,
                app_uid, runtime_uid, result);
    } else if (strcmp(command, "run") == 0) {
        if (!flatpak_manager_valid_app_id(app_id)) set_error(result, "valid Flatpak app ID required");
        else if (run_arg_count > 64) set_error(result, "too many run arguments");
        else {
            int valid = 1;
            for (size_t i = 0; i < run_arg_count; ++i) {
                if (!run_args[i] || run_args[i][0] == '-' || strlen(run_args[i]) > 4096) { valid = 0; break; }
            }
            if (!valid) set_error(result, "invalid run argument");
            else run_async(app_id, run_args, run_arg_count, result);
        }
    } else if (strcmp(command, "kill") == 0) {
        ChildResult child;
        if (!flatpak_manager_valid_app_id(app_id)) {
            set_error(result, "valid Flatpak app ID required");
            return;
        }
        {
            const char* args[] = {"--system", "kill", app_id};
            child = run_cli(args, sizeof(args) / sizeof(args[0]), 0);
        }
        result_from_child(result, &child);
        free(child.output);
    } else {
        set_error(result, "unsupported Flatpak command");
    }
}
