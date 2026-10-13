#define _GNU_SOURCE
#include <android/log.h>
#include "FlatpakManager.h"
#include "FlatpakPublish.h"
#include "RuntimeRefs.h"
#include "StubVerify.h"
#include "UdevDatabase.h"
#include "SessionPads.h"
#include "SafePath.h"
#include "FlatpakIcon.h"
#include "MatonMls.h"
#include "DownloadsGrant.h"
#include "DownloadsMedia.h"
#include <sys/xattr.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "../../linux/flatpak/linux-paths.h"
#include "LinuxStorage.h"
#include "HomeLabels.h"
#include <sys/inotify.h>
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

static void start_user_removal_watch(void);

void flatpak_manager_init(void) {
    /* Per-operation install roots are supplied explicitly (app_install_roots /
     * flatpak_publish); the app --user install lives under
     * /data/misc_de/<u>/matonos-linux/install/<uid>, the shared --system runtime under
     * /data/misc_de/<u>/matonos-linux/runtime/<runtime_uid>, and app data under
     * /data/misc_ce/<u>/matonos-linux/home/<uid>. Only the CLI's own config/home/cache is
     * global here. */
    maton_udev_start();
    start_user_removal_watch();
    setenv("TMPDIR", "/data/matonos/linux/cache", 1);
    setenv("XDG_RUNTIME_DIR", "/data/matonos/linux/run", 1);
    setenv("FLATPAK_SYSTEM_CACHE_DIR", "/data/matonos/linux/cache", 1);
    setenv("HOME", "/data/matonos/linux/flatpak-data", 1);
    setenv("XDG_DATA_HOME", "/data/matonos/linux/flatpak-data/.local/share", 1);
}

static int valid_install_uid(int uid) {
    return uid >= 10000 && uid % 100000 >= 10000 && uid % 100000 <= 19999;
}

static int remember_runtime_install(int uid) {
    char temporary[160], path[160];
    char value[32];
    if (!valid_install_uid(uid) || maton_prepare_de((unsigned)uid)) { errno = EINVAL; return -1; }
    unsigned android_user = (unsigned)uid / 100000;
    snprintf(temporary, sizeof(temporary), MATON_DE_ROOT_FORMAT "/runtime-installation.tmp", android_user);
    snprintf(path, sizeof(path), MATON_DE_ROOT_FORMAT "/runtime-installation", android_user);
    int length = snprintf(value, sizeof(value), "%d\n", uid);
    if(maton_storage_context("u:object_r:matonos_flatpak_data_file:s0"))return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    int open_error=errno;
    if(maton_storage_context(NULL)){if(fd>=0)close(fd);return -1;}
    errno=open_error;
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
    snprintf(path, sizeof(path), MATON_DE_ROOT_FORMAT "/runtime-installation",
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
            !valid_install_uid((int)parsed) || maton_user_of((unsigned)parsed) != maton_user_of((unsigned)app_uid)) {
        errno = EINVAL;
        return -1;
    }
    *uid = (int)parsed;
    return 0;
}

/* r24 storage model: ONE runtime app owns the shared --system installation at
 * /data/misc_de/<u>/matonos-linux/runtime/<runtime_uid>; each stub owns a --user
 * installation at /data/misc_de/<u>/matonos-linux/install/<app_uid>, and its writable data
 * at /data/misc_ce/<u>/matonos-linux/home/<app_uid>. The old global
 * /data/matonos/linux/flatpak* installation roots are gone. Reuse stock
 * Flatpak to verify a ref in the explicitly selected UID installation. */
static ChildResult run_cli_in(const char* const* args, size_t count, int progress,
        const char* system_dir, const char* user_dir);

/* Whether the app ref is deployed in this stub UID's own installation. */
int flatpak_manager_installed_for_uid(const char* ref, int uid) {
    if (!ref || strncmp(ref, "app/", 4) || !flatpak_manager_valid_ref(ref) ||
            !valid_install_uid(uid)) return 0;
    char user_dir[256], system_dir[256];
    int runtime_uid = -1;
    if (maton_install_dir(user_dir, sizeof(user_dir), (unsigned)uid)) return 0;
    if (remembered_runtime_install(uid, &runtime_uid) == 0)
        maton_runtime_dir(system_dir, sizeof(system_dir), maton_user_of((unsigned)uid), (unsigned)runtime_uid);
    else
        snprintf(system_dir, sizeof(system_dir), "%s", user_dir);
    const char* args[] = {"--user", "info", ref};
    ChildResult child = run_cli_in(args, 3, 0, system_dir, user_dir);
    int installed = child.status == 0 && !child.truncated;
    free(child.output);
    return installed;
}

static int app_install_roots(const char* ref, int app_uid, char* system_dir, size_t system_size,
        char* user_dir, size_t user_size) {
    int runtime_uid = -1;
    if (!flatpak_manager_installed_for_uid(ref, app_uid)) return -1;
    if (remembered_runtime_install(app_uid, &runtime_uid)) return -1;
    if (maton_runtime_dir(system_dir, system_size, maton_user_of((unsigned)app_uid), (unsigned)runtime_uid) ||
            maton_install_dir(user_dir, user_size, (unsigned)app_uid)) return -1;
    return 0;
}

static void publish_progress_line(const char* line) {
    if (g_progress_callback) g_progress_callback(line ? line : "", g_callback_context);
}

static char** flatpak_environment(const char* system_dir, const char* user_dir) {
    size_t count = 0, out = 0;
    while (environ[count]) ++count;
    char** env = calloc(count + 4, sizeof(char*));
    if (!env) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (!strncmp(environ[i], "LC_ALL=", 7)) continue;
        if ((system_dir && !strncmp(environ[i], "FLATPAK_SYSTEM_DIR=", 19)) ||
                (user_dir && !strncmp(environ[i], "FLATPAK_USER_DIR=", 17))) continue;
        env[out] = strdup(environ[i]);
        if (!env[out++]) goto fail;
    }
    env[out] = strdup("LC_ALL=C");
    if (!env[out++]) goto fail;
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
    size_t output_capacity = k_output_limit + 1;
    int folder_permissions = count == 4 && !strcmp(args[2], "--show-permissions");
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
    /* The APEX ships one multicall ELF; its basename dispatches the applet. */
    argv[0] = (char*)"flatpak";
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
        if (folder_permissions) {
            if ((size_t)n > SIZE_MAX - used - 1) {
                result.truncated = 1; (void)kill(-child, SIGKILL); break;
            }
            size_t needed = used + (size_t)n + 1;
            if (needed > output_capacity) {
                size_t grown = output_capacity <= SIZE_MAX / 2 ? output_capacity * 2 : SIZE_MAX;
                if (grown < needed) grown = needed;
                char* output = realloc(result.output, grown);
                if (!output) { result.truncated = 1; (void)kill(-child, SIGKILL); break; }
                result.output = output; output_capacity = grown;
            }
            memcpy(result.output + used, buffer, (size_t)n);
            used += (size_t)n; result.output[used] = 0;
        } else if (used < k_output_limit) {
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



static void result_from_child(FlatpakResult* result, ChildResult* child) {
    result->exit_code = child->status;
    result->ok = child->status == 0;
    result->output_truncated = child->truncated;
    result->output = child->output;
    child->output = NULL;
}

typedef struct RefContext { RuntimeRefs* db; const char* system; unsigned user; int uid; } RefContext;
static int ref_record(const char* ref, void* opaque) {
    RefContext* c = opaque;
    return runtime_refs_add(c->db, ref, c->uid) || runtime_refs_save(c->db);
}
static int ref_live(int uid, void* opaque) { (void)opaque; return stub_uid_has_package(uid); }
static int ref_prune(const char* ref, int* extension, void* opaque) {
    RefContext* c = opaque;
    if (extension) {
        const char* args[] = {"--system", "info", "--show-metadata", ref};
        ChildResult r = run_cli_in(args, 4, 0, c->system, NULL);
        int rc = r.status || r.truncated;
        if (!rc) *extension = strstr(r.output, "[ExtensionOf]") != NULL;
        free(r.output); return rc;
    }
    const char* args[] = {"uninstall", "--system", "--noninteractive", "--assumeyes", ref};
    ChildResult r = run_cli_in(args, 5, 1, c->system, NULL);
    int rc = r.status; free(r.output); return rc;
}
static int ref_complete(const char* const* refs, size_t count, void* opaque) {
    RefContext* c = opaque;
    return runtime_refs_replace(c->db, c->uid, refs, count) ||
            runtime_refs_prune(c->db, ref_prune, c);
}
static RuntimeRefs* ref_open(RefContext* c) {
    char root[160];
    snprintf(root,sizeof(root),MATON_DE_ROOT_FORMAT "/runtime",c->user);
    return runtime_refs_open(root, c->user);
}
void flatpak_manager_reconcile_runtime_refs(void) {
    pthread_mutex_lock(&g_operation_mutex);
    MatonPathScan dir; unsigned scan_uid;
    if (!maton_scan_open(&dir,"runtime")) {
        while (maton_scan_next(&dir,&scan_uid)>0) {
            int uid=(int)scan_uid;
            if (stub_runtime_uid(uid)!=uid) continue;
            char system[256];maton_runtime_dir(system,sizeof(system),maton_user_of(scan_uid),scan_uid);
            RefContext c = {.system=system,.user=maton_user_of(scan_uid)};
            c.db=ref_open(&c);
            if(!c.db || runtime_refs_reconcile(c.db,ref_live,NULL) || runtime_refs_prune(c.db,ref_prune,&c))
                fprintf(stderr,"linuxd: runtime refs reconciliation deferred for user %u\n",c.user);
            runtime_refs_close(c.db);
        }
        maton_scan_close(&dir);
    }
    pthread_mutex_unlock(&g_operation_mutex);
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
        /* The app is a --user deployment in its own UID directory; the shared
         * runtime lives in the runtime app's --system directory. Both are
         * resolved from the verified UID; no global lookup exists. */
        char system_dir[256], user_dir[256];
        if (app_install_roots(operation->ref, operation->app_uid, system_dir, sizeof(system_dir),
                user_dir, sizeof(user_dir))) {
            result.ok = 0;
            result.exit_code = 1;
            result.error = strdup("installed application deployment not found");
        } else {
            unsigned parsed_uid=0;
            if(!maton_parse_install_path(user_dir,&parsed_uid)) { errno=EINVAL; parsed_uid=0; }
            int owner_uid=(int)parsed_uid;
            RefContext refs = {.system = system_dir, .user = (unsigned)owner_uid / 100000, .uid = owner_uid};
            refs.db = ref_open(&refs);
            const char* args_with_delete[] = {"--user", "uninstall", "--delete-data", "--noninteractive", "--assumeyes", operation->ref};
            const char* args_keep_data[] = {"--user", "uninstall", "--noninteractive", "--assumeyes", operation->ref};
            child = operation->delete_data ? run_cli_in(args_with_delete, sizeof(args_with_delete) / sizeof(args_with_delete[0]), 1, system_dir, user_dir) :
                    run_cli_in(args_keep_data, sizeof(args_keep_data) / sizeof(args_keep_data[0]), 1, system_dir, user_dir);
            result_from_child(&result, &child);
            free(child.output);
            if (result.ok) {
                int cleanup = !refs.db || runtime_refs_remove(refs.db, owner_uid) ||
                        runtime_refs_prune(refs.db, ref_prune, &refs);
                child.status = cleanup ? 1 : 0;
                result.has_unused_cleanup = 1;
                result.unused_cleanup_ok = child.status == 0;
                result.unused_cleanup_exit_code = child.status;
                if (child.status) {
                    result.ok = 0;
                    if (!result.error) result.error = strdup("App was removed, but runtime reference cleanup was deferred");
                }
                free(child.output);
            }
            runtime_refs_close(refs.db);
        }
    } else {
        char system_dir[256], user_dir[256], error[512] = {0};
        snprintf(system_dir, sizeof(system_dir), MATON_DE_ROOT_FORMAT "/runtime/%d", maton_user_of((unsigned)(operation->runtime_uid)), operation->runtime_uid);
        snprintf(user_dir, sizeof(user_dir), MATON_DE_ROOT_FORMAT "/install/%d", maton_user_of((unsigned)(operation->app_uid)), operation->app_uid);
        publish_progress_line("Verifying pinned Flatpak commits and publishing runtime/app");
        /* The stub staged this operation into its own staging directory. */
        char staging_dir[160];
        RefContext refs = {.system = system_dir, .user = (unsigned)operation->app_uid / 100000, .uid = operation->app_uid};
        refs.db = ref_open(&refs);
        int rc = maton_prepare_install((unsigned)operation->runtime_uid,1) || maton_prepare_install((unsigned)operation->app_uid,0);
        if(!rc) rc = flatpak_staging_dir(operation->app_uid, operation->operation_id,
                staging_dir, sizeof(staging_dir));
        if (rc) snprintf(error, sizeof(error), "invalid staging operation");
        else if (!refs.db) { rc = -1; snprintf(error, sizeof(error), "runtime reference database unavailable"); }
        else rc = flatpak_publish_tracked(operation->ref, operation->app_commit,
                operation->runtime_ref, operation->runtime_commit, operation->remote,
                staging_dir, system_dir, user_dir, error, sizeof(error), ref_record, ref_complete, &refs);
        runtime_refs_close(refs.db);
        if (operation->operation_id) flatpak_remove_staging(operation->app_uid, operation->operation_id);
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

int flatpak_manager_prepare_runtime(int uid) {
    if(uid<0||uid%100000<10000||uid%100000>19999)return -1;
    char level[64],label[160],name[32];
    if(maton_mls_level_from_uid(uid,level,sizeof(level)))return -1;
    /* Stock untrusted_app_* may create/unlink media_rw_data_file, but not
     * custom runtime types. Preserve levelFrom=all so socket creation and
     * the untrusted stub <-> same-level Linux payload satisfy stock MLS. */
    snprintf(label,sizeof(label),"u:object_r:media_rw_data_file:%s",level);
    int root=owned_directory("/data/matonos/linux/tmp",1000);
    if(root<0)return -1;
    snprintf(name,sizeof(name),"%d",uid);
    if(create_context(label)){close(root);return -1;}
    int made=mkdirat(root,name,0700)==0;
    int saved_errno=errno;
    if(create_context(NULL) || (!made && saved_errno!=EEXIST)){close(root);return -1;}
    /* Existing runtime roots are UID-owned 0700/0711; pin with O_PATH so
     * linuxd needs no DAC override just to validate/restore their mode. */
    int dir=openat(root,name,O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;char path[64],context[256]={0};
    snprintf(path,sizeof(path),"/proc/self/fd/%d",dir);
    if(dir<0||fstat(dir,&st)||!S_ISDIR(st.st_mode)||
       st.st_uid!=(uid_t)(made?1000:uid)||
       getxattr(path,"security.selinux",context,sizeof(context)-1)<=0||strcmp(context,label)) {
        if(dir>=0)close(dir);
        close(root);
        errno=EPERM;
        return -1;
    }
    /* No migration/relabel of old custom-type trees: fail closed. */
    if(chown(path,uid,uid)||chmod(path,0700)){close(dir);close(root);return -1;}
    close(dir);close(root);
    return 0;
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
    int root=maton_home_root((unsigned)uid,0);if(root<0)return -1;
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
typedef struct DownloadsAnswer {
    int uid,pid,group;
    char* request;
    struct stat cgroup;
    struct DownloadsAnswer* next;
} DownloadsAnswer;
static DownloadsAnswer* g_downloads_answers;
static pthread_mutex_t g_downloads_mutex=PTHREAD_MUTEX_INITIALIZER;

/* vold removes misc_de/<user> after destroying both encrypted trees. No
 * refs database is cached: each package operation opens and closes its DB.
 * Drop unused one-launch consent handles when that user disappears. */
static void* user_removal_watch(void* opaque) {
    int fd=(int)(intptr_t)opaque;
    union { struct inotify_event align; char bytes[sizeof(struct inotify_event)+NAME_MAX+1]; } buffer;
    for(;;) {
        ssize_t n=read(fd,buffer.bytes,sizeof(buffer.bytes));
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        for(size_t offset=0;offset<(size_t)n;) {
            struct inotify_event* e=(struct inotify_event*)(buffer.bytes+offset);
            offset+=sizeof(*e)+e->len;
            if(!(e->mask&(IN_DELETE|IN_MOVED_FROM|IN_Q_OVERFLOW)))continue;
            char* end=NULL;unsigned long user=e->len?strtoul(e->name,&end,10):0;
            if(!(e->mask&IN_Q_OVERFLOW) && (!e->len || !*e->name || *end || user>INT_MAX/100000))continue;
            pthread_mutex_lock(&g_downloads_mutex);
            DownloadsAnswer** slot=&g_downloads_answers;
            while(*slot) {
                DownloadsAnswer* answer=*slot;
                if((e->mask&IN_Q_OVERFLOW) || maton_user_of((unsigned)answer->uid)==user) {
                    *slot=answer->next;close(answer->group);free(answer->request);free(answer);
                }else slot=&answer->next;
            }
            pthread_mutex_unlock(&g_downloads_mutex);
        }
    }
    close(fd);return NULL;
}
static void start_user_removal_watch(void) {
    int fd=inotify_init1(IN_CLOEXEC);if(fd<0)return;
    if(inotify_add_watch(fd,"/data/misc_de",IN_DELETE|IN_MOVED_FROM)<0){close(fd);return;}
    pthread_t thread;
    if(pthread_create(&thread,NULL,user_removal_watch,(void*)(intptr_t)fd)){close(fd);return;}
    pthread_detach(thread);
}

void flatpak_manager_downloads_request(const char* ref,int uid,FlatpakResult* result) {
    pthread_mutex_lock(&g_downloads_mutex);
    DownloadsAnswer** previous=&g_downloads_answers;
    while(*previous && (*previous)->uid!=uid)previous=&(*previous)->next;
    if(*previous){DownloadsAnswer* old=*previous;*previous=old->next;close(old->group);free(old->request);free(old);}
    pthread_mutex_unlock(&g_downloads_mutex);
    int runtime_uid;
    if(!valid_install_uid(uid) || !flatpak_manager_valid_ref(ref) || strncmp(ref,"app/",4) ||
            remembered_runtime_install(uid,&runtime_uid) || runtime_uid/100000!=uid/100000) {
        set_error(result,"verified app installation required");return;
    }
    char system[128],user[128];
    snprintf(system,sizeof(system),MATON_DE_ROOT_FORMAT "/runtime/%d", maton_user_of((unsigned)(runtime_uid)), runtime_uid);
    snprintf(user,sizeof(user),MATON_DE_ROOT_FORMAT "/install/%d", maton_user_of((unsigned)(uid)), uid);
    const char* args[]={"--user","info","--show-permissions",ref};
    ChildResult child=run_cli_in(args,4,0,system,user);
    if(child.status){result_from_child(result,&child);return;}
    if(child.truncated){free(child.output);set_error(result,"cannot read complete effective permissions");return;}
    MatonFolderRequests folders={0};
    char* permissions=maton_permissions_unescape(child.output);free(child.output);
    if(!permissions || maton_folders_metadata(permissions,&folders)) {
        free(permissions);set_error(result,"invalid effective folder permissions");return;
    }
    free(permissions);
    result->output=maton_folders_encode(&folders);
    maton_folders_clear(&folders);
    if(!result->output){set_error(result,"cannot encode folder request");return;}
    result->ok=1;
}
int flatpak_manager_downloads_answer(const char* ref,int uid,int pid,const char* request,int allow) {
    FlatpakResult effective={0};flatpak_manager_downloads_request(ref,uid,&effective);
    int valid=effective.ok && effective.output && !strcmp(effective.output,request);
    flatpak_manager_result_clear(&effective);
    char path[128];snprintf(path,sizeof(path),"/sys/fs/cgroup/apps/uid_%d/pid_%d",uid,pid);
    int group=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat identity;
    if(group<0 || fstat(group,&identity))valid=0;
    DownloadsAnswer* answer=NULL;
    if(valid && allow && *request) {
        answer=calloc(1,sizeof(*answer));
        if(answer)answer->request=strdup(request);
        if(!answer || !answer->request){free(answer);answer=NULL;valid=0;}
        else {answer->uid=uid;answer->pid=pid;answer->group=group;answer->cgroup=identity;group=-1;}
    }
    if(group>=0)close(group);
    pthread_mutex_lock(&g_downloads_mutex);
    DownloadsAnswer** slot=&g_downloads_answers;
    while(*slot && (*slot)->uid!=uid)slot=&(*slot)->next;
    if(*slot){DownloadsAnswer* old=*slot;*slot=old->next;close(old->group);free(old->request);free(old);}
    if(answer){answer->next=g_downloads_answers;g_downloads_answers=answer;}
    pthread_mutex_unlock(&g_downloads_mutex);
    return valid?0:-1;
}
/* JSON escaping is shared by every argv/env/path field. */
static void launch_json_string(FILE *out,const char *value) {
    fputc('"',out);
    for(const unsigned char *p=(const unsigned char *)value;*p;p++) {
        if(*p=='"' || *p=='\\') {fputc('\\',out);fputc(*p,out);}
        else if(*p<32) fprintf(out,"\\u%04x",*p);
        else fputc(*p,out);
    }
    fputc('"',out);
}

void flatpak_manager_launch_spec(const char* ref, const char* dns_servers, int x11_directory_fd, const char* x11_display, int game_controllers, int stub_uid, int stub_pid, int lifeline_fd, FlatpakResult* result) {
    (void)stub_pid;
    struct stat socket_info;
    int runtime_uid = -1;
    char system_install[128], user_install[128];
    if (stub_uid < 10000 || lifeline_fd < 0) {
        set_error(result, "verified stub process required"); return;
    }
    if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4) != 0) {
        set_error(result, "valid installed application ref required"); return;
    }
    if (!valid_install_uid(stub_uid) || remembered_runtime_install(stub_uid, &runtime_uid) ||
            runtime_uid / 100000 != stub_uid / 100000) {
        set_error(result, "shared runtime installation is unavailable"); return;
    }
    snprintf(system_install, sizeof(system_install), MATON_DE_ROOT_FORMAT "/runtime/%d", maton_user_of((unsigned)(runtime_uid)), runtime_uid);
    snprintf(user_install, sizeof(user_install), MATON_DE_ROOT_FORMAT "/install/%d", maton_user_of((unsigned)(stub_uid)), stub_uid);
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
    /* A launch spec contains no privileged descriptors, labels or inherited
     * linuxd environment. The stub supplies its private HOME/cache/session.
     * Controller inventory is still empty: fail closed, as in v2. */
    (void)controllers;
    (void)all_devices;
    (void)has_x11;
    char *wire=NULL; size_t length=0;
    FILE *out=open_memstream(&wire,&length);
    if(!out){set_error(result,"cannot allocate launch specification");return;}
    fputs("{\"argv\":[",out);
    const char *argv[]={"/apex/com.matonos.flatpak/bin/matonos-app-flatpak",
        "--user","run","--socket=wayland","--socket=x11","--no-documents-portal",
        "--no-a11y-bus","--nofilesystem=host","--nofilesystem=/tmp",
        "--nodevice=all","--device=dri",
        "--env=QT_WAYLAND_DISABLE_WINDOWDECORATION=1","--env=GTK_CSD=0",
        "--env=NO_AT_BRIDGE=1",ref+4};
    for(size_t i=0;i<sizeof(argv)/sizeof(argv[0]);i++){
        if(i)fputc(',',out); launch_json_string(out,argv[i]);
    }
    fputs("],\"env\":{\"FLATPAK_SYSTEM_DIR\":",out);launch_json_string(out,system_install);
    fputs(",\"FLATPAK_USER_DIR\":",out);launch_json_string(out,user_install);
    fputs(",\"MATON_FLATPAK_DNS\":",out);launch_json_string(out,dns_servers?dns_servers:"");
    fputs(",\"MATON_GAME_CONTROLLERS\":\"0\",\"MATON_SESSION_PAD_NODES\":\"\"},\"installation\":",out);
    launch_json_string(out,user_install);
    fputs(",\"runtimeInstallation\":",out);launch_json_string(out,system_install);
    fputs(",\"root\":\"/\"}",out);
    int failed=ferror(out);if(fclose(out))failed=1;
    if(failed){free(wire);set_error(result,"cannot encode launch specification");return;}
    result->output=wire;result->ok=1;result->exit_code=0;
}

/* Return only an installed app's exported desktop entry to the bridge. */
static void read_desktop_entry(const char* ref, int app_uid, FlatpakResult* result) {
    if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4) != 0) {
        set_error(result, "valid installed application ref required"); return;
    }
    char system_dir[256], user_dir[256];
    if (app_install_roots(ref, app_uid, system_dir, sizeof(system_dir), user_dir, sizeof(user_dir))) {
        set_error(result, "installed application deployment not found"); return;
    }
    const char* args[] = {"--user", "info", "--show-location", ref};
    ChildResult child = run_cli_in(args, 4, 0, system_dir, user_dir);
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
    /* The deployment must be a per-app --user install under
     * /data/misc_de/<u>/matonos-linux/install/<uid>/app/. */
    char per_app[272];
    snprintf(per_app,sizeof(per_app),"%s/app/",user_dir);
    if (strncmp(child.output, per_app, strlen(per_app)) != 0 ||
            snprintf(path, sizeof(path), "%s/export/share/applications/%s.desktop", child.output, app_id) >= (int)sizeof(path)) {
        free(child.output); set_error(result, "invalid application location"); return;
    }
    char root[4096], resolved[4096];
    if (!realpath(child.output, root) ||
            strncmp(root, per_app, strlen(per_app)) != 0 ||
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

static void read_exported_icon(const char* ref, int app_uid, FlatpakResult* result) {
    if (!flatpak_manager_valid_ref(ref) || strncmp(ref,"app/",4) != 0) {
        set_error(result,"valid installed application ref required"); return;
    }
    char system_dir[256], user_dir[256];
    if (app_install_roots(ref, app_uid, system_dir, sizeof(system_dir), user_dir, sizeof(user_dir))) {
        set_error(result,"installed application deployment not found"); return;
    }
    const char* args[] = {"--user","info","--show-location",ref};
    ChildResult location = run_cli_in(args,4,0,system_dir,user_dir);
    if (location.status != 0) { result_from_child(result,&location); return; }
    if (!location.output || location.truncated) { free(location.output); set_error(result,"invalid deployment location"); return; }
    location.output[strcspn(location.output,"\r\n")] = 0;
    char root[4096];
    char per_app[272];
    snprintf(per_app,sizeof(per_app),"%s/app/",user_dir);
    if (strncmp(location.output,per_app,strlen(per_app)) || !realpath(location.output,root) ||
            strncmp(root,per_app,strlen(per_app))) {
        free(location.output); set_error(result,"invalid deployment location"); return;
    }
    free(location.output);
    const char* end = strchr(ref+4,'/');
    char app_id[256];
    snprintf(app_id,sizeof(app_id),"%.*s",(int)(end-ref-4),ref+4);
    FILE* file = flatpak_icon_open(root, app_id);
    if (!file) { set_error(result,"application has no bundled PNG icon"); return; }
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

/* List app refs by running stock Flatpak against each per-app --user
 * installation under /data/misc_de/<u>/matonos-linux/install/<uid>. There is no global
 * Flatpak installation any more. */
static void list_installed_refs(int user, FlatpakResult* result) {
    size_t capacity = 4096, used = 1;
    char* list = malloc(capacity);
    if (!list) { set_error(result, "cannot allocate installed list"); return; }
    list[0] = '\0';
    MatonPathScan apps;unsigned scan_uid;
    if(maton_scan_open_user(&apps,"install",(unsigned)user)) {free(list);set_error(result,"cannot read installed applications");return;}
    int truncated=0;
    while(!truncated && maton_scan_next(&apps,&scan_uid)>0) {
        long uid=scan_uid;
        char user_dir[256], system_dir[256];
        maton_install_dir(user_dir, sizeof(user_dir), (unsigned)uid);
        int runtime_uid = -1;
        if (remembered_runtime_install((int)uid, &runtime_uid) == 0)
            maton_runtime_dir(system_dir, sizeof(system_dir), maton_user_of((unsigned)uid), (unsigned)runtime_uid);
        else
            snprintf(system_dir, sizeof(system_dir), "%s", user_dir);
        const char* args[] = {"--user", "list", "--app", "--columns=ref"};
        ChildResult child = run_cli_in(args, 4, 0, system_dir, user_dir);
        if (child.status == 0 && child.output) {
            size_t n = strlen(child.output);
            if (used + n > capacity) {
                size_t next = capacity;
                while (next < used + n) next *= 2;
                char* grown = realloc(list, next);
                if (!grown) { free(child.output); truncated = 1; break; }
                list = grown; capacity = next;
            }
            memcpy(list + used - 1, child.output, n + 1);
            used += n;
        }
        free(child.output);
    }
    maton_scan_close(&apps);
    if (truncated) { free(list); set_error(result, "installed application list too large"); return; }
    result->output = list; result->ok = 1; result->exit_code = 0;
}

int flatpak_manager_prepare(const char* ref, const char* remote, int installer_uid,
        int runtime_uid_hint, const char* operation_id,
        char* app_commit, unsigned long app_commit_size,
        char* runtime_ref, unsigned long runtime_ref_size,
        char* runtime_commit, unsigned long runtime_commit_size,
        char* metadata, unsigned long metadata_size,
        char* desktop, unsigned long desktop_size,
        char* error, unsigned long error_size) {
    if (error && error_size) error[0] = '\0';
    int runtime_uid = -1;
    if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4) || !valid_install_uid(installer_uid)) {
        if (error && error_size) snprintf(error, error_size, "valid application ref required");
        return -1;
    }
    if (!operation_id || !*operation_id || strlen(operation_id) > 64) {
        if (error && error_size) snprintf(error, error_size, "valid operation ID required");
        return -1;
    }
    /* The marker is written after the first publish, so the first install of
     * each Android user takes the bridge's runtime UID; later ones must match. */
    if (remembered_runtime_install(installer_uid, &runtime_uid)) runtime_uid = runtime_uid_hint;
    if (!valid_install_uid(runtime_uid) || runtime_uid / 100000 != installer_uid / 100000 ||
            runtime_uid != runtime_uid_hint) {
        if (error && error_size) snprintf(error, error_size, "runtime installation UID mismatch");
        return -1;
    }
    return flatpak_prepare(ref, (remote && *remote) ? remote : "flathub",
            installer_uid, runtime_uid, operation_id, app_commit, app_commit_size,
            runtime_ref, runtime_ref_size, runtime_commit, runtime_commit_size,
            metadata, metadata_size, desktop, desktop_size, error, error_size);
}

void flatpak_manager_call(const char* command, const char* ref,
        int delete_data, const char* operation_id, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        int app_uid, int runtime_uid, FlatpakResult* result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->exit_code = 127;
    if (!command) { set_error(result, "unsupported Flatpak command"); return; }
    if (strcmp(command, "list_installed") == 0) {
        if (app_uid < 0 || app_uid > INT_MAX / 100000) { set_error(result, "valid Android user required"); return; }
        list_installed_refs(app_uid, result); return;
    }
    if (strcmp(command, "installed_for_uid") == 0) {
        if (!flatpak_manager_valid_ref(ref) || !valid_install_uid(app_uid)) {
            set_error(result, "valid app ref and stub UID are required"); return;
        }
        result->ok = 1; result->exit_code = 0;
        result->output = strdup(flatpak_manager_installed_for_uid(ref, app_uid) ? "true" : "false");
        if (!result->output) set_error(result, "cannot allocate install status");
        return;
    }
    /* Remote management targets the runtime app's --system installation for
     * the calling Android user (the bridge supplies its UID). */
    if (strcmp(command, "list_remotes") == 0 || strcmp(command, "add_flathub") == 0) {
        ChildResult child;
        int changes_remote = strcmp(command, "add_flathub") == 0;
        char system_dir[256];
        int have_runtime = valid_install_uid(runtime_uid) && !maton_prepare_install((unsigned)runtime_uid,1) &&
                !maton_runtime_dir(system_dir, sizeof(system_dir), maton_user_of((unsigned)runtime_uid), (unsigned)runtime_uid);
        if(!have_runtime) {set_error(result,"per-user runtime installation unavailable");return;}
        // Flatpak supports listing the committed state during a transaction.
        // Never queue a Binder caller behind a ten-minute install.
        if (changes_remote && pthread_mutex_trylock(&g_operation_mutex) != 0) {
            set_error(result, "A Flatpak operation is already running. Try again when it finishes.");
            return;
        }
        if (strcmp(command, "list_remotes") == 0) {
            const char* args[] = {"--system", "remotes", "--show-details"};
            child = run_cli_in(args, sizeof(args) / sizeof(args[0]), 0, system_dir, NULL);
        } else {
            /* remote-add accepts neither transaction flag; --from imports the
             * repository URL and signing key from the .flatpakrepo file. */
            const char* args[] = {"--system", "remote-add", "--if-not-exists", "--from",
                    "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo"};
            child = run_cli_in(args, sizeof(args) / sizeof(args[0]), 0, system_dir, NULL);
        }
        if (changes_remote) pthread_mutex_unlock(&g_operation_mutex);
        result_from_child(result, &child);
        free(child.output);
    } else if (strcmp(command, "icon") == 0) {
        read_exported_icon(ref, app_uid, result);
    } else if (strcmp(command, "metadata") == 0) {
        if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4)) {
            set_error(result, "valid application ref required"); return;
        }
        char system_dir[256], user_dir[256];
        if (app_install_roots(ref, app_uid, system_dir, sizeof(system_dir), user_dir, sizeof(user_dir))) {
            set_error(result, "installed application deployment not found"); return;
        }
        const char* args[] = {"--user", "info", "--show-metadata", ref};
        ChildResult child = run_cli_in(args, 4, 0, system_dir, user_dir);
        result_from_child(result, &child); free(child.output);
    } else if (strcmp(command, "desktop_entry") == 0) {
        read_desktop_entry(ref, app_uid, result);
    } else if (strcmp(command, "install") == 0 || strcmp(command, "uninstall") == 0) {
        if (!flatpak_manager_valid_ref(ref)) set_error(result, "valid complete Flatpak ref required");
        else if (strcmp(command, "install") == 0 && (!flatpak_manager_valid_ref(runtime_ref) ||
                strncmp(runtime_ref, "runtime/", 8) || !remote || !*remote || strlen(remote) > 128 ||
                !valid_install_uid(app_uid) || !valid_install_uid(runtime_uid) ||
                app_uid / 100000 != runtime_uid / 100000))
            set_error(result, "valid pinned runtime ref, remote, and same-user installation UIDs are required");
        else if (strcmp(command, "uninstall") == 0 && !flatpak_manager_installed_for_uid(ref, app_uid))
            set_error(result, "installed application deployment not found");
        else start_package_operation(strcmp(command, "uninstall") == 0, ref, delete_data,
                operation_id, app_commit, runtime_ref, runtime_commit, remote,
                app_uid, runtime_uid, result);
    } else {
        set_error(result, "unsupported Flatpak command");
    }
}
