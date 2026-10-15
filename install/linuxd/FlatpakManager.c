#define _GNU_SOURCE
#include <android/log.h>
#include "FlatpakManager.h"
#include "FlatpakPublish.h"
#include "RuntimeRefs.h"
#include "SafePath.h"
#include <sys/xattr.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "../../linux/flatpak/linux-paths.h"
#include "LinuxStorage.h"
#include "DeploymentStore.h"
#include "PendingDeployment.h"
#include "RemotePin.h"
#include "StoreUpdate.h"
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
#include <ctype.h>
#include <unistd.h>

extern char** environ;

/* r24: linuxd stays in system_ext; the Flatpak stack ships in the updatable
 * com.matonos.flatpak APEX, mounted at /apex/com.matonos.flatpak. This is the
 * launcher (platform-signed Flatpak env wrapper); it execs the CLI in the
 * same APEX. */
static const char k_flatpak[] = "/apex/com.matonos.flatpak/bin/flatpak-env-wrapper";
static const size_t k_output_limit = 24 * 1024;
static pthread_mutex_t g_operation_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    memset(result, 0, sizeof(*result));
}

/* One private handoff at a time; all accesses hold g_operation_mutex. */
static struct {
    char operation[65], ref[513], remote[129], path[512];
    char** refs;
    size_t count;
    int compositor_uid;
    enum { HANDOFF_EMPTY, HANDOFF_STAGING, HANDOFF_VERIFYING, HANDOFF_PENDING, HANDOFF_FAILED } state;
} pending_install;

/* Never evict completed identities during this daemon lifetime: eviction would
 * turn an old completion into a new authorization. Bound memory by refusing
 * further registrations after the journal fills. Restart removes all staging. */
static char g_staged_ids[4096][65];
static unsigned char g_staged_bound[4096];
static char g_staged_refs[4096][513],g_staged_remotes[4096][129];
static size_t g_staged_id_count;
static int g_staging_ready;

void flatpak_manager_init(void) {
    g_staging_ready=flatpak_cleanup_stale_staging()==0;
    if(!g_staging_ready)fprintf(stderr,"linuxd: stale staging cleanup failed; registrations disabled\n");
    DIR* abandoned=opendir(MATON_INSTALL_ROOT);
    if(abandoned) {
        struct dirent* e;
        while((e=readdir(abandoned))) {
            char path[PATH_MAX];snprintf(path,sizeof(path),MATON_INSTALL_ROOT "/%s",e->d_name);
            if(pending_deployment_path(path) && pending_deployment_discard(path))
                fprintf(stderr,"linuxd: abandoned handoff cleanup deferred\n");
        }
        closedir(abandoned);
    }
    /* Fixed management scratch only; app runtimes belong to the stubs. */
    if (maton_storage_prepare()) fprintf(stderr, "linuxd: management storage unavailable\n");

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
    char user_dir[512], system_dir[512];
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
    if(app_uid==-1 && pending_install.state==HANDOFF_PENDING && pending_install.operation[0] && !strcmp(ref,pending_install.ref)) {
        if(strlen(pending_install.path)>=user_size)return -1;
        snprintf(system_dir,system_size,"%s",MATON_RUNTIME_DIR);
        strcpy(user_dir,pending_install.path);return 0;
    }
    if (!flatpak_manager_installed_for_uid(ref, app_uid)) return -1;
    snprintf(system_dir,system_size,"%s",MATON_RUNTIME_DIR);
    char pointer[PATH_MAX],resolved[PATH_MAX];unsigned appid;
    if(maton_install_dir(pointer,sizeof(pointer),(unsigned)app_uid)||!realpath(pointer,resolved)||
       !maton_parse_install_path(resolved,&appid)||appid!=maton_app_id((unsigned)app_uid)||
       strlen(resolved)>=user_size)return -1;
    strcpy(user_dir,resolved);
    return 0;
}

/* Existing installations are migrated from Flatpak's recorded origin. Never
 * bless a requested remote merely because the new metadata is absent. */
static int check_remote_pin(const char* ref,const char* remote,int uid,int commit) {
    if(maton_storage_prepare())return -1;
    int fd=open(MATON_LINUX_ROOT "/manager",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    int rc=remote_pin_check(fd,ref,remote);
    if(!rc && flatpak_manager_installed_for_uid(ref,uid)) {
        char system[256],user[256];
        if(app_install_roots(ref,uid,system,sizeof(system),user,sizeof(user)))rc=-1;
        else {
            const char* args[]={"--user","info","--show-origin",ref};
            ChildResult child=run_cli_in(args,4,0,system,user);
            if(child.status || child.truncated || !child.output)rc=-1;
            else {child.output[strcspn(child.output,"\r\n")]=0;if(strcmp(child.output,remote))rc=-2;}
            free(child.output);
        }
    }
    if(!rc && commit)rc=remote_pin_commit(fd,ref,remote);
    close(fd);return rc;
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
    const char* executable=k_flatpak;
#ifdef MATONOS_PUBLISH_HOST_TEST
    const char* override=getenv("MATONOS_FLATPAK_CLI");
    if(override && *override)executable=override;
#endif
    rc = posix_spawn(&child, executable, &actions, &attributes, argv, child_env);
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



typedef struct RefContext { RuntimeRefs* db; RuntimeRefs* version; const char* system; unsigned user; int uid; } RefContext;
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
static RuntimeRefs* ref_open(RefContext* c) {
    char root[160];
    snprintf(root,sizeof(root),"%s",MATON_RUNTIME_DIR);
    (void)c; return runtime_refs_open(root, 0);
}
static int collect_deployments(void);
static int retained_refs(RuntimeRefs* db);
void flatpak_manager_reconcile_runtime_refs(void) {
    pthread_mutex_lock(&g_operation_mutex);
    /* Publication is private until PM binding; protect its dependencies. */
    if(pending_install.operation[0]){pthread_mutex_unlock(&g_operation_mutex);return;}
    /* Keep all shared references until old versions can be collected. */
    int pending=collect_deployments();
    RefContext c={.system=MATON_RUNTIME_DIR};c.db=ref_open(&c);
    if(!c.db || retained_refs(c.db) ||
       (!pending && runtime_refs_prune(c.db,ref_prune,&c)))
        fprintf(stderr,"linuxd: shared runtime cleanup deferred\n");
    runtime_refs_close(c.db);
    pthread_mutex_unlock(&g_operation_mutex);
}

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
        /* A current install pointer is the owner record. PackageManager callbacks are not
         * part of linuxd; only unselected versions are collected here. */
        if(current)continue;
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
        long uid=scan_uid;
        char user_dir[512], system_dir[512];
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

void flatpak_manager_call(const char* command, const char* ref,
        int delete_data, const char* operation_id, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        int app_uid, FlatpakResult* result) {
    (void)delete_data; (void)operation_id; (void)app_commit; (void)runtime_ref; (void)runtime_commit; (void)remote;
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->exit_code = 127;
    if (!command) { set_error(result, "unsupported Flatpak command"); return; }
    if (!strcmp(command, "list_installed")) {
        if (app_uid < 0 || app_uid > INT_MAX / 100000) { set_error(result, "valid Android user required"); return; }
        list_installed_refs(app_uid, result); return;
    }
    if (!strcmp(command, "installed_commit")) {
        if (!flatpak_manager_valid_ref(ref) || strncmp(ref, "app/", 4) || app_uid < 10000 ||
                !flatpak_manager_installed_for_uid(ref, app_uid)) {
            set_error(result, "installed application deployment not found"); return;
        }
        char user[512], origin[129], commit[65];
        if (maton_install_dir(user, sizeof(user), (unsigned)app_uid)) { set_error(result, "invalid app UID"); return; }
        const char* origin_args[] = {"--user", "info", "--show-origin", ref};
        ChildResult child = run_cli_in(origin_args, 4, 0, MATON_RUNTIME_DIR, user);
        int rc = child.status || child.truncated || !child.output;
        if (!rc) {
            child.output[strcspn(child.output, "\r\n")] = 0;
            if (!*child.output || strlen(child.output) >= sizeof(origin)) rc = -1;
            else snprintf(origin, sizeof(origin), "%s", child.output);
        }
        free(child.output);
        const char* commit_args[] = {"--user", "info", "--show-commit", ref};
        if (!rc) {
            child = run_cli_in(commit_args, 4, 0, MATON_RUNTIME_DIR, user);
            rc = child.status || child.truncated || store_parse_commit(child.output, commit, sizeof(commit));
            free(child.output);
        }
        if (rc) { set_error(result, "cannot read installed origin and commit"); return; }
        result->output = malloc(256);
        if (!result->output) { set_error(result, "out of memory"); return; }
        snprintf(result->output, 256, "%s\t%s", origin, commit);
        result->ok = 1; result->exit_code = 0; return;
    }
    if (!strcmp(command, "uninstall")) {
        if (!flatpak_manager_valid_ref(ref) || !valid_install_uid(app_uid) ||
                !flatpak_manager_installed_for_uid(ref, app_uid)) {
            set_error(result, "installed application deployment not found"); return;
        }
        result->ok = 1; result->exit_code = 0;
        result->output = strdup("Stub uninstall authorized; PackageManager removes the app and reconciliation collects deployments");
        return;
    }
    set_error(result, "unsupported Flatpak command");
}

static void pending_clear(void) {
    for(size_t i=0;i<pending_install.count;i++)free(pending_install.refs[i]);
    free(pending_install.refs);memset(&pending_install,0,sizeof(pending_install));
}
static int pending_record(const char* ref,void* unused) {
    (void)unused;
    RuntimeRefs* db=runtime_refs_open(MATON_RUNTIME_DIR,0);
    int rc=!db || runtime_refs_note(db,ref) || runtime_refs_save(db);
    runtime_refs_close(db);return rc;
}
static void pending_prune(void) {
    RefContext refs={.system=MATON_RUNTIME_DIR};
    RuntimeRefs* db=ref_open(&refs);
    if(!db || retained_refs(db) || runtime_refs_prune(db,ref_prune,&refs))
        fprintf(stderr,"linuxd: failed handoff runtime cleanup deferred\n");
    runtime_refs_close(db);
}
static int pending_refs(const char* const* refs,size_t count,void* unused) {
    (void)unused;
    pending_install.refs=calloc(count?count:1,sizeof(char*));
    if(!pending_install.refs)return -1;
    for(size_t i=0;i<count;i++) {
        pending_install.refs[i]=strdup(refs[i]);
        if(!pending_install.refs[i])return -1;
        pending_install.count++;
    }
    return 0;
}
/* Bridge-only, operation-scoped transport. No caller-supplied path, keyring,
 * metadata or installer UID is accepted. No app pointer exists before bind. */
void flatpak_manager_handoff(const char* command,const char* ref,const char* remote,
        const char* operation,int uid,FlatpakResult* result) {
    memset(result,0,sizeof(*result));result->exit_code=1;
    if(!command || !flatpak_manager_valid_ref(ref) || strncmp(ref,"app/",4) ||
       !operation || !*operation || strlen(operation)>64) {
        set_error(result,"invalid deployment handoff");return;
    }
    for(const char* p=operation;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='_'&&*p!='-') {
        set_error(result,"invalid operation ID");return;
    }
    pthread_mutex_lock(&g_operation_mutex);
    if(strcmp(command,"bind_pending") || pending_install.state!=HANDOFF_PENDING ||
            strcmp(operation,pending_install.operation)||strcmp(ref,pending_install.ref)||
            !remote||strcmp(remote,pending_install.remote)) {
        set_error(result,"unknown deployment handoff");goto done;
    }
    {
            if(!valid_install_uid(uid) || uid/100000!=pending_install.compositor_uid/100000) {
                set_error(result,"stub and compositor must belong to the same Android user");goto done;
            }
            char bound[256]={0};
            int rc=deployment_create((unsigned)uid,bound,sizeof(bound));
            if(!rc)rc=deployment_discard(bound);
            if(!rc)rc=rename(pending_install.path,bound);
            if(!rc)snprintf(pending_install.path,sizeof(pending_install.path),"%s",bound);
            RuntimeRefs* version=!rc?runtime_refs_open(bound,0):NULL;
            RuntimeRefs* shared=!rc?runtime_refs_open(MATON_RUNTIME_DIR,0):NULL;
            int appid=(int)maton_app_id((unsigned)uid);
            if(!rc)rc=!version || !shared;
            for(size_t i=0;!rc && i<pending_install.count;i++)
                rc=runtime_refs_add(version,pending_install.refs[i],appid) ||
                        runtime_refs_add(shared,pending_install.refs[i],appid);
            if(!rc)rc=runtime_refs_save(version) || runtime_refs_save(shared);
            runtime_refs_close(version);runtime_refs_close(shared);
            if(!rc && deployment_commit((unsigned)uid,bound)) {
                char pointer[256],selected[PATH_MAX];
                /* rename may have committed before directory fsync failed.
                 * Finish binding if the selected version is ours; never
                 * delete an already selected deployment or its final stub. */
                rc=maton_install_dir(pointer,sizeof(pointer),(unsigned)uid) ||
                        !realpath(pointer,selected) || strcmp(selected,bound);
                if(!rc)fprintf(stderr,"linuxd: binding committed; directory fsync failed\n");
            }
            if(rc) {set_error(result,"cannot bind verified deployment");goto done;}
            for(size_t i=0;i<g_staged_id_count;i++)
                if(!strcmp(g_staged_ids[i],operation))g_staged_bound[i]=1;
            pending_clear();result->ok=1;result->exit_code=0;
    }
done:
    pthread_mutex_unlock(&g_operation_mutex);
}

static int staged_name(const char* s,size_t limit) {
    if(!s || !*s || strlen(s)>limit || s[0]=='.' || s[0]=='-')return 0;
    for(const unsigned char* p=(const unsigned char*)s;*p;p++)
        if(!ascii_alpha(*p) && !ascii_digit(*p) && *p!='_' && *p!='-' && *p!='.')return 0;
    return 1;
}

void flatpak_manager_staged(const char* command,const char* operation,
        const char* ref,const char* remote,int compositor_uid,
        const struct FlatpakStagedManifest* manifest,FlatpakResult* result) {
    memset(result,0,sizeof(*result));result->exit_code=1;
    if(!command || !staged_name(operation,64)) {set_error(result,"invalid staged operation");return;}
    /* Do not queue completion or abort behind verification. In particular a
     * provider death cannot delete an input while its verification is active. */
    if(pthread_mutex_trylock(&g_operation_mutex)) {set_error(result,"another store mutation is active");return;}
    char error[512]={0};
    if(!strcmp(command,"begin_staged_install")) {
        if(!flatpak_manager_valid_ref(ref) || strncmp(ref,"app/",4) ||
                !staged_name(remote,128) || !valid_install_uid(compositor_uid)) {
            set_error(result,"invalid staged authorization");goto done;
        }
        for(size_t i=0;i<g_staged_id_count;i++)if(!strcmp(g_staged_ids[i],operation)) {
            set_error(result,"operation ID already used");goto done;
        }
        if(!g_staging_ready || pending_install.operation[0] || g_staged_id_count==4096) {
            set_error(result,"staged transaction registry unavailable");goto done;
        }
        /* Reserve even failed registrations. A retry needs a fresh identity. */
        strcpy(g_staged_ids[g_staged_id_count++],operation);
        if(check_remote_pin(ref,remote,-1,0) || !flatpak_remote_verified(remote)) {
            set_error(result,"configured remote or application pin is invalid");goto done;
        }
        snprintf(pending_install.operation,sizeof(pending_install.operation),"%s",operation);
        snprintf(pending_install.ref,sizeof(pending_install.ref),"%s",ref);
        snprintf(pending_install.remote,sizeof(pending_install.remote),"%s",remote);
        pending_install.compositor_uid=compositor_uid;
        pending_install.state=HANDOFF_STAGING;
        strcpy(g_staged_refs[g_staged_id_count-1],ref);
        strcpy(g_staged_remotes[g_staged_id_count-1],remote);
        if(flatpak_begin_staged_install(compositor_uid,operation,remote,error,sizeof(error))) {
            if(flatpak_remove_staging(compositor_uid,operation))pending_install.state=HANDOFF_FAILED;
            else pending_clear();
            set_error(result,error[0]?error:"cannot create staged transaction");goto done;
        }
        result->ok=1;result->exit_code=0;goto done;
    }
    if(strcmp(operation,pending_install.operation)) {set_error(result,"unknown staged operation");goto done;}
    if(!strcmp(command,"abort_staged_install")) {
        if(pending_install.state!=HANDOFF_STAGING && pending_install.state!=HANDOFF_PENDING && pending_install.state!=HANDOFF_FAILED) {
            set_error(result,"staged operation cannot be aborted");goto done;
        }
        int rc=flatpak_remove_staging(pending_install.compositor_uid,operation);
        if(pending_install.path[0] && pending_deployment_discard(pending_install.path))rc=-1;
        if(rc){set_error(result,"staged cleanup deferred");goto done;}
        pending_clear();pending_prune();result->ok=1;result->exit_code=0;goto done;
    }
    if(strcmp(command,"verify_staged_install") || pending_install.state!=HANDOFF_STAGING ||
            !manifest || manifest->version!=1 || strcmp(manifest->operation_id,operation) ||
            strcmp(manifest->app_ref,pending_install.ref) || strcmp(manifest->remote,pending_install.remote)) {
        set_error(result,"staged manifest does not match authorization");goto done;
    }
    pending_install.state=HANDOFF_VERIFYING;
    char staging[PATH_MAX];
    int rc=flatpak_staging_dir(pending_install.compositor_uid,operation,staging,sizeof(staging));
    if(!rc)rc=pending_deployment_create(pending_install.ref,pending_install.path,sizeof(pending_install.path));
    if(!rc)rc=flatpak_verify_publish_staged_tracked(manifest,staging,MATON_RUNTIME_DIR,
            pending_install.path,error,sizeof(error),pending_record,pending_refs,NULL);
    if(!rc)rc=check_remote_pin(pending_install.ref,pending_install.remote,-1,1);
    int staging_cleanup=flatpak_remove_staging(pending_install.compositor_uid,operation);
    if(staging_cleanup && !rc) {
        snprintf(error,sizeof(error),"staged cleanup deferred");rc=-1;
    }
    if(rc) {
        int cleanup=pending_install.path[0]?pending_deployment_discard(pending_install.path):0;
        /* Retain identity for a cleanup retry if files could not be collected. */
        if(!cleanup && !staging_cleanup){pending_clear();pending_prune();}
        else pending_install.state=HANDOFF_FAILED;
        set_error(result,error[0]?error:"staged verification failed");goto done;
    }
    pending_install.state=HANDOFF_PENDING;
    result->ok=1;result->exit_code=0;
    result->output=strdup("Verified deployment awaiting final stub");
done:
    pthread_mutex_unlock(&g_operation_mutex);
}

int flatpak_manager_complete_handoff(const char* operation,const char* ref,const char* remote) {
    int rc=-1;
    pthread_mutex_lock(&g_operation_mutex);
    for(size_t i=0;operation && i<g_staged_id_count;i++)
        if(ref && remote && !strcmp(g_staged_ids[i],operation) && g_staged_bound[i]==1 &&
                !strcmp(g_staged_refs[i],ref) && !strcmp(g_staged_remotes[i],remote)) {
            g_staged_bound[i]=2;rc=0;break;
        }
    pthread_mutex_unlock(&g_operation_mutex);return rc;
}
