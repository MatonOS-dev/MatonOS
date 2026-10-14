#define _GNU_SOURCE
#include <android/log.h>
#include "FlatpakManager.h"
#include "FlatpakPublish.h"
#include "RuntimeRefs.h"
#include "StubVerify.h"
#include "SafePath.h"
#include "FlatpakIcon.h"
#include <sys/xattr.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "../../linux/flatpak/linux-paths.h"
#include "LinuxStorage.h"
#include "DeploymentStore.h"
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



void flatpak_manager_init(void) {
    /* Fixed management scratch only; app runtimes belong to the stubs. */
    if (maton_store_prepare()) fprintf(stderr, "linuxd: management storage unavailable\n");

    setenv("TMPDIR", "/data/linux-app/manager/cache", 1);
    setenv("XDG_RUNTIME_DIR", "/data/linux-app/manager/run", 1);
    setenv("FLATPAK_SYSTEM_CACHE_DIR", "/data/linux-app/manager/cache", 1);
    setenv("HOME", "/data/linux-app/manager/home", 1);
    setenv("XDG_DATA_HOME", "/data/linux-app/manager/home/.local/share", 1);
}

static int valid_install_uid(int uid) {
    return uid >= 10000 && uid % 100000 >= 10000 && uid % 100000 <= 19999;
}

static ChildResult run_cli_in(const char* const* args, size_t count, int progress,
        const char* system_dir, const char* user_dir);

/* Whether the app ref is deployed in this stub UID's own installation. */
int flatpak_manager_installed_for_uid(const char* ref, int uid) {
    if (!ref || strncmp(ref, "app/", 4) || !flatpak_manager_valid_ref(ref) ||
            !valid_install_uid(uid)) return 0;
    char user_dir[256], system_dir[256];
    if (maton_install_dir(user_dir,sizeof(user_dir),(unsigned)uid)) return 0;
    snprintf(system_dir,sizeof(system_dir),"%s",MATON_RUNTIME_DIR);
    const char* args[] = {"--user", "info", ref};
    ChildResult child = run_cli_in(args, 3, 0, system_dir, user_dir);
    int installed = child.status == 0 && !child.truncated;
    free(child.output);
    return installed;
}

static int app_install_roots(const char* ref, int app_uid, char* system_dir, size_t system_size,
        char* user_dir, size_t user_size) {
    if (!flatpak_manager_installed_for_uid(ref, app_uid)) return -1;
    snprintf(system_dir,system_size,"%s",MATON_RUNTIME_DIR);
    char pointer[PATH_MAX],resolved[PATH_MAX];unsigned appid;
    if(maton_install_dir(pointer,sizeof(pointer),(unsigned)app_uid)||!realpath(pointer,resolved)||
       !maton_parse_install_path(resolved,&appid)||appid!=maton_app_id((unsigned)app_uid)||
       strlen(resolved)>=user_size)return -1;
    strcpy(user_dir,resolved);
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
    char pending[4097];
    size_t pending_length = 0;
    result.status = 127;
    result.truncated = 0;
    result.output = calloc(k_output_limit + 1, 1);
    if (!result.output) return result;
    /* install/<appid> is only ever created by deployment_commit() as the
     * atomic link to a versioned dir. Flatpak would mkdir a missing
     * FLATPAK_USER_DIR itself, which blocks that link, so a query on an
     * app without a deployment just reports "not installed". */
    struct stat user_root;
    if (user_dir && (lstat(user_dir, &user_root) != 0 ||
            (strrchr(user_dir,'/') && !strchr(strrchr(user_dir,'/'),'-') && !S_ISLNK(user_root.st_mode)))) {
        snprintf(result.output, k_output_limit + 1, "not installed");
        result.status = 1;
        return result;
    }
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



static void result_from_child(FlatpakResult* result, ChildResult* child) {
    result->exit_code = child->status;
    result->ok = child->status == 0;
    result->output_truncated = child->truncated;
    result->output = child->output;
    child->output = NULL;
}

typedef struct RefContext { RuntimeRefs* db; RuntimeRefs* version; const char* system; unsigned user; int uid; } RefContext;
static int ref_record(const char* ref, void* opaque) {
    RefContext* c = opaque;
    return runtime_refs_add(c->db, ref, c->uid) || runtime_refs_save(c->db) ||
            runtime_refs_add(c->version,ref,c->uid) || runtime_refs_save(c->version);
}
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
    return runtime_refs_replace(c->version, c->uid, refs, count);
}
static RuntimeRefs* ref_open(RefContext* c) {
    char root[160];
    snprintf(root,sizeof(root),"%s",MATON_RUNTIME_DIR);
    (void)c; return runtime_refs_open(root, 0);
}
static int collect_deployments(void);
static int retained_refs(RuntimeRefs* db);
void flatpak_manager_reconcile_runtime_refs(void) {
    pthread_mutex_lock(&g_operation_mutex);
    /* Keep all shared references until old versions can be collected. */
    int pending=collect_deployments();
    RefContext c={.system=MATON_RUNTIME_DIR};c.db=ref_open(&c);
    if(!c.db || retained_refs(c.db) ||
       (!pending && runtime_refs_prune(c.db,ref_prune,&c)))
        fprintf(stderr,"linuxd: shared runtime cleanup deferred\n");
    runtime_refs_close(c.db);
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
} PackageOperation;

static void* package_operation_thread(void* data) {
    PackageOperation* operation = data;
    FlatpakResult result;
    memset(&result, 0, sizeof(result));
    pthread_mutex_lock(&g_operation_mutex);
    if (operation->uninstall) {
        result.ok=0;result.exit_code=1;
        result.error=strdup("Remove the stub package; deployment cleanup follows PackageManager removal for all users");
    } else {
        char system_dir[256], user_dir[256], error[512] = {0};
        snprintf(system_dir,sizeof(system_dir),"%s",MATON_RUNTIME_DIR);
        user_dir[0]=0;
        publish_progress_line("Verifying pinned Flatpak commits and publishing runtime/app");
        /* The stub staged this operation into its own staging directory. */
        char staging_dir[160];
        RefContext refs = {.system = system_dir, .user = 0, .uid = (int)maton_app_id((unsigned)operation->app_uid)};
        int rc = deployment_create((unsigned)operation->app_uid,user_dir,sizeof(user_dir));
        refs.db = ref_open(&refs);
        refs.version=!rc?runtime_refs_open(user_dir,0):NULL;
        if(!rc) rc = flatpak_staging_dir(operation->app_uid, operation->operation_id,
                staging_dir, sizeof(staging_dir));
        if (rc) snprintf(error, sizeof(error), "invalid staging operation");
        else if (!refs.db || !refs.version) { rc = -1; snprintf(error, sizeof(error), "runtime reference database unavailable"); }
        else rc = flatpak_publish_tracked(operation->ref, operation->app_commit,
                operation->runtime_ref, operation->runtime_commit, operation->remote,
                staging_dir, system_dir, user_dir, error, sizeof(error), ref_record, ref_complete, &refs);
        runtime_refs_close(refs.db);runtime_refs_close(refs.version);
        if (operation->operation_id) flatpak_remove_staging(operation->app_uid, operation->operation_id);
        if (!rc && deployment_commit((unsigned)operation->app_uid,user_dir)) {
            snprintf(error,sizeof(error),"cannot commit deployment pointer");rc=-1;
        }
        if(rc && user_dir[0] && deployment_discard(user_dir))
            fprintf(stderr,"linuxd: unpublished deployment cleanup deferred\n");
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
    if (!result.ok)
        __android_log_print(ANDROID_LOG_WARN, "matonos-linuxd", "%s %s failed: %s",
                operation->uninstall ? "uninstall" : "install", operation->ref ? operation->ref : "?",
                result.error ? result.error : "unknown error");
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
        const char* runtime_commit, const char* remote, int app_uid,
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
/* Return nonzero while any retired version must be preserved. */
static int collect_deployments(void) {
    int parent=open(MATON_INSTALL_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(parent<0)return errno==ENOENT?0:1;
    DIR* entries=fdopendir(dup(parent));if(!entries){close(parent);return 1;}
    struct dirent* e;int pending=0;
    while((e=readdir(entries))) {
        char path[PATH_MAX];unsigned appid;
        snprintf(path,sizeof(path),"%s/%s",MATON_INSTALL_ROOT,e->d_name);
        if(!deployment_version_name(e->d_name) || !maton_parse_install_path(path,&appid))continue;
        char pointer[32],target[NAME_MAX+1];snprintf(pointer,sizeof(pointer),"%u",appid);
        ssize_t n=readlinkat(parent,pointer,target,sizeof(target)-1);
        if(n>=0)target[n]=0;
        int current=n>=0 && !strcmp(target,e->d_name);
        if(!current){if(deployment_discard(path))pending=1;continue;}
        int live=stub_uid_has_package((int)appid);
        if(live<0 || (current && live)){if(!current)pending=1;continue;}
        int dir=openat(parent,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(dir<0){pending=1;continue;}
        int lease=deployment_exclusive(dir);struct stat st;
        if(lease<0){close(dir);pending=1;continue;}
        int rc=fstat(dir,&st)||st.st_uid!=1000;
        if(!rc && current && unlinkat(parent,pointer,0))rc=-1;
        if(!rc)rc=remove_contents(dir,st.st_dev);
        close(dir);
        if(!rc)rc=unlinkat(parent,e->d_name,AT_REMOVEDIR);
        close(lease);if(rc)pending=1;
    }
    closedir(entries);close(parent);return pending;
}

/* Reference counts include current and leased retired versions, across users.
 * Fail closed on a corrupted inventory; never publish a partial snapshot. */
static int retained_refs(RuntimeRefs* db) {
    int fd=open(MATON_INSTALL_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    DIR* entries=fdopendir(fd);if(!entries){close(fd);return -1;}
    runtime_refs_clear_owners(db);
    struct dirent* e;int rc=0;
    while((e=readdir(entries))) {
        char path[PATH_MAX];unsigned appid;
        snprintf(path,sizeof(path),"%s/%s",MATON_INSTALL_ROOT,e->d_name);
        if(!strchr(e->d_name,'-') || !maton_parse_install_path(path,&appid))continue;
        RuntimeRefs* version=runtime_refs_open(path,0);
        if(!version || runtime_refs_merge(db,version)){rc=-1;runtime_refs_close(version);break;}
        runtime_refs_close(version);
    }
    closedir(entries);
    return rc?rc:runtime_refs_save(db);
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
    /* The resolved deployment must stay inside this immutable version. */
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

/* Shared code inventory filtered by live per-user PackageManager identities. */
static void list_installed_refs(int user, FlatpakResult* result) {
    size_t capacity = 4096, used = 1;
    char* list = malloc(capacity);
    if (!list) { set_error(result, "cannot allocate installed list"); return; }
    list[0] = '\0';
    MatonPathScan apps;unsigned scan_uid;
    if(maton_scan_open_user(&apps,"install",(unsigned)user)) {free(list);set_error(result,"cannot read installed applications");return;}
    int truncated=0;
    while(!truncated && maton_scan_next(&apps,&scan_uid)>0) {
        int present=stub_user_has_package(user*100000+(int)scan_uid);
        if(present<0){maton_scan_close(&apps);free(list);set_error(result,"PackageManager inventory unavailable");return;}
        if(!present)continue;
        long uid=scan_uid;
        char user_dir[256], system_dir[256];
        maton_install_dir(user_dir, sizeof(user_dir), (unsigned)uid);
        snprintf(system_dir,sizeof(system_dir),"%s",MATON_RUNTIME_DIR);
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
        const char* operation_id,
        char* app_commit, unsigned long app_commit_size,
        char* runtime_ref, unsigned long runtime_ref_size,
        char* runtime_commit, unsigned long runtime_commit_size,
        char* metadata, unsigned long metadata_size,
        char* desktop, unsigned long desktop_size,
        char* error, unsigned long error_size) {
    if (error && error_size) error[0] = '\0';
    if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4) || !valid_install_uid(installer_uid)) {
        if (error && error_size) snprintf(error, error_size, "valid application ref required");
        return -1;
    }
    if (!operation_id || !*operation_id || strlen(operation_id) > 64) {
        if (error && error_size) snprintf(error, error_size, "valid operation ID required");
        return -1;
    }
    return flatpak_prepare(ref, (remote && *remote) ? remote : "flathub",
            installer_uid, operation_id, app_commit, app_commit_size,
            runtime_ref, runtime_ref_size, runtime_commit, runtime_commit_size,
            metadata, metadata_size, desktop, desktop_size, error, error_size);
}

void flatpak_manager_call(const char* command, const char* ref,
        int delete_data, const char* operation_id, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        int app_uid, FlatpakResult* result) {
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
    /* Remote configuration is system-wide and controlled only by linuxd. */
    if (strcmp(command, "list_remotes") == 0 || strcmp(command, "add_flathub") == 0) {
        ChildResult child;
        int changes_remote = strcmp(command, "add_flathub") == 0;
        char system_dir[256];
        if(maton_store_prepare()) {set_error(result,"system store unavailable");return;}
        snprintf(system_dir,sizeof(system_dir),"%s",MATON_RUNTIME_DIR);
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
                !valid_install_uid(app_uid)))
            set_error(result, "valid pinned runtime ref, remote, and stub UID are required");
        else if (strcmp(command, "uninstall") == 0 && !flatpak_manager_installed_for_uid(ref, app_uid))
            set_error(result, "installed application deployment not found");
        else start_package_operation(strcmp(command, "uninstall") == 0, ref, delete_data,
                operation_id, app_commit, runtime_ref, runtime_commit, remote,
                app_uid, result);
    } else {
        set_error(result, "unsupported Flatpak command");
    }
}
