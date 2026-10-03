#define _GNU_SOURCE
#include <android/log.h>
#include "FlatpakManager.h"
#include "UdevDatabase.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
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
    maton_udev_start();
    setenv("TMPDIR", "/data/matonos/linux/cache", 1);
    setenv("XDG_RUNTIME_DIR", "/data/matonos/linux/runtime", 1);
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

typedef struct GraphicalChild { pid_t pid; int directory, listener, slot, x11_directory, x11_listener; char path[108], x11_path[108], x11_name[64]; } GraphicalChild;
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
typedef struct WaylandRelay { int client, compositor; } WaylandRelay;

/* Wayland transfers SHM buffers and fences with SCM_RIGHTS, not just bytes. */
static int relay_wayland_packet(int source, int destination) {
    char bytes[16384];
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(64*sizeof(int))]; } control;
    struct iovec buffer = {.iov_base=bytes,.iov_len=sizeof(bytes)};
    struct msghdr message = {.msg_iov=&buffer,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    ssize_t count;
    do { count=recvmsg(source,&message,MSG_CMSG_CLOEXEC); } while (count<0 && errno==EINTR);
    if (count<=0) return -1;
    int fds[64]; size_t fd_count=0;
    for (struct cmsghdr* c=CMSG_FIRSTHDR(&message); c; c=CMSG_NXTHDR(&message,c)) {
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
            size_t n=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
            int* values=(int*)CMSG_DATA(c);
            for(size_t i=0;i<n;++i) { if(fd_count<64) fds[fd_count++]=values[i]; else close(values[i]); }
        }
    }
    int failed=(message.msg_flags & MSG_CTRUNC)!=0;
    message.msg_flags=0; buffer.iov_len=(size_t)count;
    ssize_t sent=-1;
    if(!failed) do { sent=sendmsg(destination,&message,MSG_NOSIGNAL); } while(sent<0 && errno==EINTR);
    for(size_t i=0;i<fd_count;++i)close(fds[i]);
    if(sent<=0)return -1;
    while(sent<count) {
        ssize_t n=send(destination,bytes+sent,(size_t)(count-sent),MSG_NOSIGNAL);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return -1;sent+=n;
    }
    return 0;
}
static void* relay_wayland(void* argument) {
    WaylandRelay* relay=argument;
    struct pollfd sockets[2]={{.fd=relay->client,.events=POLLIN},{.fd=relay->compositor,.events=POLLIN}};
    for(;;) {
        int ready=poll(sockets,2,-1);
        if(ready<0 && errno==EINTR)continue;
        if(ready<=0)break;
        int failed=0;
        for(int i=0;i<2;++i) {
            if(sockets[i].revents&POLLIN) { if(relay_wayland_packet(sockets[i].fd,sockets[1-i].fd))failed=1; }
            else if(sockets[i].revents&(POLLHUP|POLLERR|POLLNVAL))failed=1;
        }
        if(failed)break;
    }
    close(relay->client);close(relay->compositor);free(relay);return NULL;
}
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

/* Android's caption bar owns minimize/maximize/close, so GTK (including
 * Firefox's tab strip and libadwaita header bars) is told to draw no window
 * buttons. Only the gtk-decoration-layout key is set; other app settings in
 * the sandbox's settings.ini survive. Best effort: a failure keeps the app's
 * own buttons rather than blocking the launch. */
static int make_directory_chain(char* path) {
    for (char* p = path + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0'; int failed = mkdir(path, 0700) && errno != EEXIST; *p = '/';
        if (failed) return -1;
    }
    return mkdir(path, 0700) && errno != EEXIST ? -1 : 0;
}
static void set_gtk_settings_key(const char* directory) {
    static const char key[] = "gtk-decoration-layout";
    static const char line[] = "gtk-decoration-layout=:\n";
    char dir[512], path[600], old[8192] = {0}, out[8192 + sizeof(line) + 16];
    snprintf(dir, sizeof(dir), "%s", directory);
    if (make_directory_chain(dir)) return;
    snprintf(path, sizeof(path), "%s/settings.ini", directory);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    size_t length = 0;
    if (fd >= 0) {
        ssize_t got;
        while (length < sizeof(old) - 1 && (got = read(fd, old + length, sizeof(old) - 1 - length)) > 0) length += (size_t)got;
        close(fd);
        if (length == sizeof(old) - 1) return;  /* unexpectedly large: leave it alone */
    }
    size_t n = 0; int done = 0, in_settings = 0;
    for (char* cursor = old; *cursor; ) {
        char* end = strchr(cursor, '\n'); size_t len = end ? (size_t)(end - cursor) + 1 : strlen(cursor);
        if (cursor[0] == '[') {
            if (in_settings && !done) { memcpy(out + n, line, sizeof(line) - 1); n += sizeof(line) - 1; done = 1; }
            in_settings = !strncmp(cursor, "[Settings]", 10);
        }
        if (in_settings && !strncmp(cursor, key, sizeof(key) - 1) &&
                strchr(" \t=", cursor[sizeof(key) - 1])) {
            if (!done) { memcpy(out + n, line, sizeof(line) - 1); n += sizeof(line) - 1; done = 1; }
        } else {
            memcpy(out + n, cursor, len); n += len;
            if (!end && len) out[n++] = '\n';
        }
        cursor += len;
    }
    if (!done && in_settings) { memcpy(out + n, line, sizeof(line) - 1); n += sizeof(line) - 1; done = 1; }
    if (!done) n += (size_t)snprintf(out + n, sizeof(out) - n, "%s[Settings]\n%s", n ? "\n" : "", line);
    char temporary[620];
    snprintf(temporary, sizeof(temporary), "%s.matonos", path);
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    int ok = write(fd, out, n) == (ssize_t)n;
    close(fd);
    if (!ok || rename(temporary, path)) unlink(temporary);
}
static void hide_toolkit_window_buttons(const char* ref) {
    const char* id = ref + 4; const char* slash = strchr(id, '/');
    if (!slash) return;
    static const char* const versions[] = {"gtk-3.0", "gtk-4.0"};
    for (size_t i = 0; i < 2; ++i) {
        char directory[512];
        snprintf(directory, sizeof(directory), "/data/matonos/linux/flatpak-data/.var/app/%.*s/config/%s",
                (int)(slash - id), id, versions[i]);
        set_gtk_settings_key(directory);
    }
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

/* The compositor delegates only its socket directories, never its app data root. */
void flatpak_manager_launch_graphical(const char* ref, int runtime_directory_fd, const char* dns_servers, int x11_directory_fd, const char* x11_display, int game_controllers, FlatpakResult* result) {
    struct stat directory, socket_info;
    if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4) != 0) {
        set_error(result, "valid installed application ref required"); return;
    }
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
    /* Check the full installed ref rather than accepting arbitrary commands. */
    const char* info_args[] = {"--system", "info", ref};
    ChildResult installed = run_cli(info_args, 3, 0);
    if (installed.status != 0) { result_from_child(result, &installed); return; }
    free(installed.output);
    const char* metadata_args[] = {"--system", "info", "--show-metadata", ref};
    ChildResult metadata = run_cli(metadata_args, 4, 0);
    int controllers = 0, all_devices = 0;
    if (metadata.status == 0 && !metadata.truncated)
        controllers = declared_controllers(metadata.output, &all_devices) && game_controllers;
    free(metadata.output);
    hide_toolkit_window_buttons(ref);
    deny_host_tmp();
    int capability = fcntl(runtime_directory_fd, F_DUPFD_CLOEXEC, 3);
    if (capability < 0) { set_error(result, "cannot duplicate compositor directory"); return; }
    int x11_capability = has_x11 ? fcntl(x11_directory_fd, F_DUPFD_CLOEXEC, 3) : -1;
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
    char display_env[128];
    snprintf(display_env,sizeof(display_env),"WAYLAND_DISPLAY=%s",socket_path);
    char dns_env[2048];
    snprintf(dns_env,sizeof(dns_env),"MATON_FLATPAK_DNS=%s",dns_servers ? dns_servers : "");
    size_t env_count = 0;
    while (environ[env_count]) ++env_count;
    char** env = calloc(env_count + 5, sizeof(char*));
    size_t n = 0;
    if (env) {
        for (size_t i = 0; i < env_count; ++i)
            if (strncmp(environ[i], "WAYLAND_DISPLAY=", 16) != 0 &&
                    strncmp(environ[i],"MATON_FLATPAK_DNS=",18) != 0 &&
                    strncmp(environ[i],"DISPLAY=",8) != 0 &&
                    strncmp(environ[i],"MATON_GAME_CONTROLLERS=",23) != 0 &&
                    strncmp(environ[i],"MATON_INHIBIT_DIRECTORY_FD=",26) != 0) env[n++] = environ[i];
        env[n++] = display_env;
        env[n++] = dns_env;
        env[n++] = "MATON_INHIBIT_DIRECTORY_FD=198";
        env[n] = controllers ? "MATON_GAME_CONTROLLERS=1" : "MATON_GAME_CONTROLLERS=0";
    } else rc = ENOMEM;
    /* Both display sockets are granted; toolkits choose their backend.
     * Android's caption bar is the only window decoration, so toolkits that
     * ignore the decoration protocol are told to drop their own. The GPU's
     * render node is passed in: Wayland clients render on it and hand their
     * dma-bufs to the compositor, which shows them without a copy. */
    char* argv[] = {(char*)k_flatpak, "--system", "run",
            "--socket=wayland", "--socket=x11", "--no-documents-portal",
            controllers && all_devices ? "--device=all" : "--nodevice=all", "--device=dri",
            "--env=QT_WAYLAND_DISABLE_WINDOWDECORATION=1", "--env=GTK_CSD=0",
            "--env=NO_AT_BRIDGE=1",
            (char*)ref + 4, NULL};
    pid_t child = -1;
    if (!rc) rc = posix_spawn(&child, k_flatpak, &actions, NULL, argv, env);
    posix_spawn_file_actions_destroy(&actions);
    if (logfd >= 0) close(logfd); free(env);
    if (rc) { close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability); set_error(result, strerror(rc)); return; }
    int launch_slot=record_launch(ref,log_path,child);
    if(launch_slot<0){kill(child,SIGTERM);while(waitpid(child,NULL,0)<0&&errno==EINTR){}close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability);set_error(result,"Too many active launches");return;}
    /* Report immediate CLI failures to the launch Activity instead of a blank window. */
    for (int i = 0; i < 8; ++i) {
        int status; pid_t exited = waitpid(child, &status, WNOHANG);
        if (exited == child) {
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
    if (state) { state->pid = child; state->slot=launch_slot; state->directory = capability; state->listener = listener; state->x11_directory=x11_capability; state->x11_listener=x11_listener;
        snprintf(state->x11_path,sizeof(state->x11_path),"%s",x11_path);snprintf(state->x11_name,sizeof(state->x11_name),"%s",has_x11?x11_display:""); snprintf(state->path,sizeof(state->path),"%s",socket_path); }
    if (!state || pthread_create(&reaper, NULL, reap_graphical, state)) {
        record_exit(launch_slot,child);free(state); close_graphical_sockets(listener,socket_path,capability,x11_listener,x11_path,x11_capability); kill(child, SIGTERM); while (waitpid(child, NULL, 0) < 0 && errno == EINTR) { }
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
    if (!realpath(child.output, root) || !realpath(path, resolved) ||
            strncmp(resolved, root, strlen(root)) != 0 || resolved[strlen(root)] != '/') {
        free(child.output); set_error(result, "desktop entry must remain inside its installed deployment"); return;
    }
    free(child.output);
    FILE* file = fopen(resolved, "re");
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
    if (strncmp(location.output,"/data/matonos/linux/flatpak/app/",strlen("/data/matonos/linux/flatpak/app/")) || !realpath(location.output,root)) {
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
        file=fopen(resolved,"re"); if (file) break;
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
            file=fopen(resolved,"re");
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
            DIR* directory=length>0 && length<(int)sizeof(media) ? opendir(media) : NULL;
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
                        file=fopen(resolved,"re");if(file)break;
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
        if (arch_end && realpath("/data/matonos/linux/flatpak/appstream/flathub",metadata_root)) {
            for(size_t i=0;i<2;++i) {
                int length=snprintf(path,sizeof(path),"%s/%.*s/active/icons/%s/%.*s.png",metadata_root,(int)(arch_end-end-1),end+1,thumbnails[i],(int)(end-ref-4),ref+4);
                if(length<0 || length>=(int)sizeof(path) || !realpath(path,resolved))continue;
                if(strncmp(resolved,metadata_root,strlen(metadata_root)) || resolved[strlen(metadata_root)]!='/')continue;
                struct stat metadata;
                if(stat(resolved,&metadata) || !S_ISREG(metadata.st_mode) || metadata.st_size<8 || metadata.st_size>256*1024)continue;
                file=fopen(resolved,"re");if(file)break;
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
        const char* operation_id, FlatpakResult* result) {
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
