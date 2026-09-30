#define _GNU_SOURCE
#include "FlatpakManager.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

static const char k_flatpak[] = "/system_ext/bin/flatpak";
static const size_t k_output_limit = 24 * 1024;
static pthread_mutex_t g_operation_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_package_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_package_operation_active;
static FlatpakProgressCallback g_progress_callback;
static FlatpakCompleteCallback g_complete_callback;
static void* g_callback_context;

static void* reap_child(void* data);

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
    setenv("FLATPAK_SYSTEM_DIR", "/data/matonos/linux/flatpak", 1);
    setenv("FLATPAK_SYSTEM_CACHE_DIR", "/data/matonos/linux/cache", 1);
    setenv("FLATPAK_USER_DIR", "/data/matonos/linux/flatpak-user", 1);
    setenv("HOME", "/data/matonos/linux/flatpak-data", 1);
    setenv("XDG_DATA_HOME", "/data/matonos/linux/flatpak-data/.local/share", 1);
}

static void publish_progress_line(const char* line) {
    if (g_progress_callback) g_progress_callback(line ? line : "", g_callback_context);
}

static ChildResult run_cli(const char* const* args, size_t count, int progress) {
    ChildResult result;
    int pipes[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    char** argv;
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
    rc = posix_spawn(&child, k_flatpak, &actions, &attributes, argv, environ);
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

static int spawn_detached(pid_t pid) {
    pthread_t thread;
    pid_t* heap_pid = malloc(sizeof(*heap_pid));
    int error;
    if (!heap_pid) return ENOMEM;
    *heap_pid = pid;
    error = pthread_create(&thread, NULL, reap_child, heap_pid);
    if (error != 0) {
        free(heap_pid);
        return error;
    }
    (void)pthread_detach(thread);
    return 0;
}

static void* reap_child(void* data) {
    pid_t pid = *(pid_t*)data;
    int status;
    free(data);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    return NULL;
}

typedef struct PackageOperation {
    int uninstall;
    int delete_data;
    char* operation_id;
    char* ref;
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
        const char* args[] = {"--system", "install", "--noninteractive", "--assumeyes", "flathub", operation->ref};
        child = run_cli(args, sizeof(args) / sizeof(args[0]), 1);
        result_from_child(&result, &child);
        free(child.output);
    }
    pthread_mutex_unlock(&g_operation_mutex);
    pthread_mutex_lock(&g_package_state_mutex);
    g_package_operation_active = 0;
    pthread_mutex_unlock(&g_package_state_mutex);
    result.operation_id = operation->operation_id ? strdup(operation->operation_id) : NULL;
    if (g_complete_callback) g_complete_callback(&result, g_callback_context);
    flatpak_manager_result_clear(&result);
    free(operation->ref);
    free(operation->operation_id);
    free(operation);
    return NULL;
}

static void start_package_operation(int uninstall, const char* ref, int delete_data,
        const char* operation_id, FlatpakResult* result) {
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
    operation->operation_id = operation_id ? strdup(operation_id) : NULL;
    if (operation_id && !operation->operation_id) { free(operation); pthread_mutex_lock(&g_package_state_mutex); g_package_operation_active = 0; pthread_mutex_unlock(&g_package_state_mutex); set_error(result, "cannot allocate operation ID"); return; }
    operation->ref = strdup(ref);
    if (!operation->ref) {
        free(operation->operation_id);
        free(operation);
        pthread_mutex_lock(&g_package_state_mutex); g_package_operation_active = 0; pthread_mutex_unlock(&g_package_state_mutex);
        set_error(result, "cannot allocate Flatpak ref");
        return;
    }
    error = pthread_create(&thread, NULL, package_operation_thread, operation);
    if (error != 0) {
        free(operation->ref);
        free(operation->operation_id);
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
    int nullfd;
    posix_spawn_file_actions_t actions;
    char** argv;
    size_t count = 5 + extra_count;
    pid_t child = -1;
    int rc;
    if (access(k_flatpak, X_OK) != 0) {
        set_error(result, strerror(errno));
        return;
    }
    for (size_t i = 0; i < extra_count; ++i) {
        if (extra[i] == NULL || extra[i][0] == '-') {
            set_error(result, "Flatpak run arguments must not be options");
            return;
        }
    }
    argv = calloc(count + 1, sizeof(char*));
    if (!argv) { set_error(result, "cannot allocate Flatpak arguments"); return; }
    argv[0] = (char*)k_flatpak;
    argv[1] = "--system";
    argv[2] = "run";
    argv[3] = (char*)app_id;
    argv[4] = "--";
    for (size_t i = 0; i < extra_count; ++i) argv[5 + i] = (char*)extra[i];
    nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (posix_spawn_file_actions_init(&actions) != 0) {
        if (nullfd >= 0) close(nullfd);
        free(argv);
        set_error(result, "Flatpak run setup failed");
        return;
    }
    if (nullfd >= 0) {
        (void)posix_spawn_file_actions_adddup2(&actions, nullfd, STDOUT_FILENO);
        (void)posix_spawn_file_actions_adddup2(&actions, nullfd, STDERR_FILENO);
        (void)posix_spawn_file_actions_addclose(&actions, nullfd);
    }
    rc = posix_spawn(&child, k_flatpak, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (nullfd >= 0) close(nullfd);
    free(argv);
    if (rc != 0) {
        set_error(result, strerror(rc));
        return;
    }
    rc = spawn_detached(child);
    if (rc != 0) {
        (void)kill(child, SIGTERM);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) { }
        set_error(result, "cannot reap Flatpak process");
        return;
    }
    result->ok = 1;
    result->has_pid = 1;
    result->pid = (long)child;
}

void flatpak_manager_call(const char* command, const char* ref, const char* app_id,
        const char* const* run_args, size_t run_arg_count, int delete_data,
        const char* operation_id, FlatpakResult* result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->exit_code = 127;
    if (!command) { set_error(result, "unsupported Flatpak command"); return; }
    if (strcmp(command, "list_installed") == 0 || strcmp(command, "list_remotes") == 0 ||
            strcmp(command, "add_flathub") == 0) {
        ChildResult child;
        pthread_mutex_lock(&g_operation_mutex);
        if (strcmp(command, "list_installed") == 0) {
            const char* args[] = {"--system", "list", "--columns=application,ref"};
            child = run_cli(args, sizeof(args) / sizeof(args[0]), 0);
        } else if (strcmp(command, "list_remotes") == 0) {
            const char* args[] = {"--system", "remotes", "--show-details"};
            child = run_cli(args, sizeof(args) / sizeof(args[0]), 0);
        } else {
            const char* args[] = {"--system", "remote-add", "--if-not-exists", "--noninteractive",
                    "--assumeyes", "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo"};
            child = run_cli(args, sizeof(args) / sizeof(args[0]), 0);
        }
        pthread_mutex_unlock(&g_operation_mutex);
        result_from_child(result, &child);
        free(child.output);
    } else if (strcmp(command, "install") == 0 || strcmp(command, "uninstall") == 0) {
        if (!flatpak_manager_valid_ref(ref)) set_error(result, "valid complete Flatpak ref required");
        else start_package_operation(strcmp(command, "uninstall") == 0, ref, delete_data, operation_id, result);
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
