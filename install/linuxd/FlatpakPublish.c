#define _GNU_SOURCE
#include "FlatpakPublish.h"
#include "StagedVerity.h"
#include "PendingDeployment.h"
#include "StoreUpdate.h"

#include <dirent.h>
#include <grp.h>
#include "../../linux/flatpak/linux-paths.h"
#include "LinuxStorage.h"
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <unistd.h>

#define AID_SYSTEM 1000

extern char** environ;
static _Thread_local void (*progress_callback)(const char*);
void flatpak_publish_progress(void (*callback)(const char*)) { progress_callback=callback; }

#ifndef MATONOS_FLATPAK_CLI
#define MATONOS_FLATPAK_CLI "/apex/com.matonos.flatpak/bin/flatpak-env-wrapper"
#endif
/* OSTree is an applet of the static multicall ELF; callers set argv[0]="ostree".
 * The APEX ships no bin/ostree symlink (Soong drops prebuilt-binary symlinks). */
#ifndef MATONOS_OSTREE_CLI
#define MATONOS_OSTREE_CLI "/apex/com.matonos.flatpak/bin/matonos-flatpak"
#endif
#ifndef MATONOS_FLATPAK_STAGE_HELPER
#define MATONOS_FLATPAK_STAGE_HELPER "/apex/com.matonos.flatpak/bin/matonos-flatpak-store"
#endif

static void fail(char* error, unsigned long size, const char* message) {
    if (error && size) snprintf(error, size, "%s", message);
}

static int valid_commit(const char* value) {
    if (!value || strlen(value) != 64) return 0;
    for (const char* p = value; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
    return 1;
}

static int valid_remote(const char* value) {
    if (!value || !*value || value[0]=='-' || strlen(value) > 128) return 0;
    for (const unsigned char* p = (const unsigned char*)value; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return 0;
    return 1;
}

static int valid_operation_id(const char* value) {
    if (!value || !*value || strlen(value) > 64) return 0;
    for (const unsigned char* p = (const unsigned char*)value; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

static int remove_tree_at(int parent, const char* name) {
    struct stat parent_st,child_st;
    if(fstat(parent,&parent_st)||fstatat(parent,name,&child_st,AT_SYMLINK_NOFOLLOW))return -1;
    if(parent_st.st_dev!=child_st.st_dev){errno=EXDEV;return -1;}
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return unlinkat(parent, name, 0);
    DIR* dir = fdopendir(dup(fd));
    if (!dir) { close(fd); return -1; }
    struct dirent* entry; int rc = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) rc |= remove_tree_at(fd, entry->d_name);
        else rc |= unlinkat(fd, entry->d_name, 0);
        if (rc) break;
    }
    closedir(dir); close(fd);
    if (!rc) rc = unlinkat(parent, name, AT_REMOVEDIR);
    return rc;
}

/* Copy exactly one remote group from the runtime system installation. */
static int seed_remote_from(const char* source, const char* staging, const char* remote);

static int seed_remote(const char* staging, const char* remote) {
    char source[PATH_MAX];
    if (snprintf(source, sizeof(source), MATON_RUNTIME_DIR "/repo") >= (int)sizeof(source)) return -1;
    return seed_remote_from(source, staging, remote);
}

/* Copy one remote group and its keyring from the repo at source into
 * <staging>/repo (an initialised repository). */
static int seed_remote_from(const char* source, const char* staging, const char* remote) {
    char config_path[PATH_MAX], key_path[PATH_MAX], group[192];
    if (snprintf(config_path, sizeof(config_path), "%s/repo/config", staging) >= (int)sizeof(config_path) ||
            snprintf(key_path, sizeof(key_path), "%s/repo/%s.trustedkeys.gpg", staging, remote) >= (int)sizeof(key_path) ||
            snprintf(group, sizeof(group), "[remote \"%s\"]", remote) >= (int)sizeof(group)) return -1;
    char src_config[PATH_MAX];
    if (snprintf(src_config, sizeof(src_config), "%s/config", source) >= (int)sizeof(src_config)) return -1;
    FILE* in = fopen(src_config, "re");
    if (!in) return -1;
    FILE* out = fopen(config_path, "ae");  /* after ostree init's [core] */
    if (!out) { fclose(in); return -1; }
    char* line = NULL; size_t cap = 0; ssize_t n; int found = 0, in_group = 0;
    while ((n = getline(&line, &cap, in)) >= 0) {
        if (line[0] == '[') {
            if (in_group) break;
            in_group = strcmp(line, group) == 0 || (n == (ssize_t)strlen(group) + 1 &&
                    line[n - 1] == '\n' && !strncmp(line, group, strlen(group)));
            if (in_group) found = 1;
        }
        if (in_group && fwrite(line, 1, (size_t)n, out) != (size_t)n) { found = 0; break; }
    }
    free(line);
    int bad = ferror(in);
    if (fclose(in)) bad = 1;
    if (fflush(out)) bad = 1;
    if (fclose(out)) bad = 1;
    if (bad || !found) return -1;
    char src_key[PATH_MAX];
    if (snprintf(src_key, sizeof(src_key), "%s/%s.trustedkeys.gpg", source, remote) >= (int)sizeof(src_key)) return -1;
    int src = open(src_key, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (src < 0) return -1;
    int dst = open(key_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (dst < 0) { close(src); return -1; }
    char buf[16384]; ssize_t got; int rc = 0;
    while ((got = read(src, buf, sizeof(buf))) > 0) {
        ssize_t off = 0; while (off < got) { ssize_t put = write(dst, buf + off, (size_t)(got - off)); if (put <= 0) { rc = -1; break; } off += put; }
        if (rc) break;
    }
    if (got < 0 || fchmod(dst, 0600) || close(dst)) rc = -1;
    close(src);
    if (rc) unlink(key_path);
    return rc;
}

/* The repo skeleton linuxd created becomes the installer's, group system: mode
 * before owner, since linuxd (no CAP_FOWNER) cannot chmod afterwards. */
static int hand_over_tree(int parent, const char* name, int uid) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) return -1;
    if (S_ISLNK(st.st_mode)) return fchownat(parent, name, uid, AID_SYSTEM, AT_SYMLINK_NOFOLLOW);
    if (!S_ISDIR(st.st_mode))
        return fchmodat(parent, name, 0640, 0) || fchownat(parent, name, uid, AID_SYSTEM, AT_SYMLINK_NOFOLLOW) ? -1 : 0;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    DIR* dir = fdopendir(dup(fd));
    int rc = dir ? 0 : -1;
    struct dirent* entry;
    while (!rc && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        rc = hand_over_tree(fd, entry->d_name, uid);
    }
    if (dir) closedir(dir);
    if (!rc && (fchmod(fd, 0750) || fchown(fd, uid, AID_SYSTEM))) rc = -1;
    close(fd);
    return rc;
}

static int give_to_installer(const char* staging, int uid, const char* remote) {
#ifdef MATONOS_PUBLISH_HOST_TEST
    (void)staging;(void)uid;(void)remote;return 0;
#else
    static const char* const dirs[] = {"home", "cache", "tmp", "runtime"};
    char path[PATH_MAX];
    int dir = open(staging, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return -1;
    int rc = 0;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]) && !rc; ++i) {
        if (mkdirat(dir, dirs[i], 0700) && errno != EEXIST) rc = -1;
        else if (fchownat(dir, dirs[i], uid, uid, AT_SYMLINK_NOFOLLOW)) rc = -1;
    }
    /* linuxd reads repo (publish) through group system. */
    (void)path; (void)remote;
    if (!rc && hand_over_tree(dir, "repo", uid)) rc = -1;
    /* Last: the operation itself (the store helper requires the stub to own it). */
    if (!rc && (fchmod(dir, 02750) || fchown(dir, uid, AID_SYSTEM))) rc = -1;
    close(dir);
    return rc;
#endif
}

/* Staging trees are written by the fixed installer UID; linuxd has no
 * DAC override, so it removes them in a child running as that UID. */
static int remove_staging_as(int uid, const char* operation) {
    if ((uid != MATON_FPINSTALL_UID && !(uid >= 10000 && uid % 100000 >= 10000)) || !valid_operation_id(operation)) return -1;
#ifndef MATONOS_PUBLISH_HOST_TEST
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        char path[192];
        snprintf(path, sizeof(path), MATON_STAGING_ROOT "/%d", uid);
        if (setgroups(0, NULL) || setresgid(uid, uid, uid) || setresuid(uid, uid, uid)) _exit(1);
        size_t used = strlen(path);
        if (snprintf(path + used, sizeof(path) - used, "/%s", operation) >= (int)(sizeof(path) - used)) _exit(1);
        int op = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (op < 0) _exit(errno == ENOENT ? 0 : 1);
        /* Empty the operation (FPINSTALL owns it and what it wrote). */
        DIR* dir = fdopendir(dup(op));
        int rc = dir ? 0 : 1;
        struct dirent* entry;
        while (dir && (entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (remove_tree_at(op, entry->d_name) && errno != ENOENT && errno != EACCES) rc = 1;
        }
        if (dir) closedir(dir);
        _exit(rc);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
    #endif
    /* linuxd owns staging/: remove the emptied operation and anything it
     * created itself before handing over. */
    char staging_path[96];
    snprintf(staging_path, sizeof(staging_path), MATON_STAGING_ROOT "/%d", uid);
    int staging = open(staging_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (staging < 0) return errno == ENOENT ? 0 : -1;
    int rc = remove_tree_at(staging, operation) && errno != ENOENT ? -1 : 0;
    close(staging);
    if(!rc && rmdir(staging_path) && errno!=ENOTEMPTY && errno!=ENOENT)rc=-1;
    return rc;
}

int flatpak_staging_dir(int uid, const char* operation, char* path, unsigned long size) {
    if ((uid != MATON_FPINSTALL_UID && !(uid >= 10000 && uid % 100000 >= 10000)) || !valid_operation_id(operation)) return -1;
    return snprintf(path, size, MATON_STAGING_ROOT "/%d/%s", uid, operation) < (int)size ? 0 : -1;
}

static int remove_registered_staging(int uid,const char* operation);
int flatpak_remove_staging(int uid, const char* operation) {
    return uid==MATON_FPINSTALL_UID?remove_staging_as(uid, operation):remove_registered_staging(uid,operation);
}

static int cleanup_registered_staging(void);
int flatpak_cleanup_stale_staging(void) {
    if(cleanup_registered_staging())return -1;
    int rc=0;
    unsigned uid=MATON_FPINSTALL_UID;
    {
        char path[160];snprintf(path,sizeof(path),MATON_STAGING_ROOT "/%u",uid);
        int app=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(app<0)return errno==ENOENT?0:-1;
        int staging=dup(app);close(app);
        if(staging<0)return -1;
        DIR* ops=fdopendir(staging);
        if(!ops){close(staging);return -1;}
        struct dirent* e;
        while((e=readdir(ops))) {
            if(e->d_name[0]=='.')continue;
            if(remove_staging_as((int)uid,e->d_name)){rc=-1;break;}
        }
        closedir(ops);
    }
    return rc;
}

static int create_staging(int uid, const char* operation, char* path, size_t size) {
    if ((uid != MATON_FPINSTALL_UID && !(uid >= 10000 && uid % 100000 >= 10000)) || !valid_operation_id(operation)) return -1;
    if(maton_store_prepare())return -1;
    char install_root[160];
    snprintf(install_root,sizeof(install_root),"%s",MATON_STAGING_ROOT);
    int data = open(install_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (data < 0) return -1;
    char owner[32]; snprintf(owner, sizeof(owner), "%d", uid);
    int app = maton_storage_child(data,owner,"u:object_r:matonos_flatpak_staging_file:s0",0711);
    close(data); if (app < 0) return -1;
    /* FPINSTALL must traverse it (mkdirat's mode is masked by umask). */
    if (fchmod(app, 0711)) { close(app); return -1; }
    /* staging/2902 and staging/ stay linuxd's (system); each operation is
     * built by linuxd and handed to FPINSTALL by give_to_installer(). */
    int staging = dup(app);
    close(app); if (staging < 0) return -1;
    struct stat st;
    if (fstat(staging, &st) || st.st_uid != AID_SYSTEM || fchmod(staging, 0711) ||
            maton_storage_context("u:object_r:matonos_flatpak_staging_file:s0") || mkdirat(staging, operation, 0700)) {
        maton_storage_context(NULL);close(staging); return -1;
    }
    maton_storage_context(NULL);close(staging);
    return snprintf(path, size, MATON_STAGING_ROOT "/%d/%s", uid, operation) < (int)size ? 0 : -1;
}

static const char* tool_path(const char* applet) {
    const char* override = getenv(applet[0] == 'f' ? "MATONOS_FLATPAK_CLI" : "MATONOS_OSTREE_CLI");
    if (override && *override) return override;
    return applet[0] == 'f' ? MATONOS_FLATPAK_CLI : MATONOS_OSTREE_CLI;
}

/* Child environment differs only in Flatpak's installation roots. */
static char** child_environment(const char* system_dir, const char* user_dir) {
    size_t count = 0;
    while (environ[count]) ++count;
    char** env = calloc(count + 4, sizeof(char*));
    if (!env) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!strncmp(environ[i], "FLATPAK_SYSTEM_DIR=", 19) ||
                !strncmp(environ[i], "FLATPAK_USER_DIR=", 17)) continue;
        env[out++] = environ[i];
    }
    if (system_dir) {
        if (asprintf(&env[out++], "FLATPAK_SYSTEM_DIR=%s", system_dir) < 0) goto bad;
    }
    if (user_dir) {
        if (asprintf(&env[out++], "FLATPAK_USER_DIR=%s", user_dir) < 0) goto bad;
    }
    return env;
bad:
    while (out) {
        char* value = env[--out];
        if (value && (strstr(value, "FLATPAK_SYSTEM_DIR=") == value ||
                    strstr(value, "FLATPAK_USER_DIR=") == value)) free(value);
    }
    free(env);
    return NULL;
}

static int invoke_path_capture(const char* path, const char* system_dir, const char* user_dir,
        char* const args[], char* output, size_t output_size, char** full) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) return -1;
    char** env = child_environment(system_dir, user_dir);
    if (!env) { close(pipefd[0]); close(pipefd[1]); return -1; }
    pid_t child = fork();
    if (child == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execve(path, args, env);
        _exit(127);
    }
    close(pipefd[1]);
    for (size_t i = 0; env[i]; ++i)
        if (strstr(env[i], "FLATPAK_SYSTEM_DIR=") == env[i] ||
                strstr(env[i], "FLATPAK_USER_DIR=") == env[i]) free(env[i]);
    free(env);
    if (child < 0) { close(pipefd[0]); return -1; }
    size_t used = 0;
    int failed = 0;
    if (full) { *full = calloc(1, 1); if (!*full) failed = 1; }
    char progress[4096];size_t progress_used=0;
    char chunk[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
        if (n > 0) {
            if(progress_callback)for(ssize_t i=0;i<n;i++) {
                if(chunk[i]=='\n'||chunk[i]=='\r'||progress_used==sizeof(progress)-1) {
                    progress[progress_used]=0;
                    if(progress_used)progress_callback(progress);
                    progress_used=0;
                } else progress[progress_used++]=chunk[i];
            }
            if (full && !failed) {
                if (memchr(chunk, 0, (size_t)n) || (size_t)n >= SIZE_MAX - used) failed = 1;
                else {
                    char* next = realloc(*full, used + (size_t)n + 1);
                    if (!next) failed = 1;
                    else { *full = next; memcpy(next + used, chunk, (size_t)n); used += (size_t)n; next[used] = 0; }
                }
            } else if (!full && output && output_size && used < output_size - 1) {
                size_t take = (size_t)n;
                if (take > output_size - 1 - used) take = output_size - 1 - used;
                memcpy(output + used, chunk, take);
                used += take;
                output[used] = '\0';
            }
        } else if (n == 0) break;
        else if (errno != EINTR) { failed = 1; break; }
    }
    if(progress_callback && progress_used){progress[progress_used]=0;progress_callback(progress);}
    close(pipefd[0]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (failed || waited < 0) {
        fprintf(stderr, "%s: %s failed (%s)\n", path, waited < 0 ? "waitpid" : "output read", strerror(errno));
        return -1;
    }
    if (WIFSIGNALED(status)) fprintf(stderr, "%s: killed by signal %d\n", path, WTERMSIG(status));
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static int invoke_path(const char* path, const char* system_dir, const char* user_dir,
        char* const args[], char* output, size_t output_size) {
    return invoke_path_capture(path, system_dir, user_dir, args, output, output_size, NULL);
}

static int invoke(const char* applet, const char* system_dir, const char* user_dir,
        char* const args[], char* output, size_t output_size) {
    return invoke_path(tool_path(applet), system_dir, user_dir, args, output, output_size);
}

static int run_flatpak(const char* system_dir, const char* user_dir,
        const char* const input[], size_t count) {
    char** args = calloc(count + 2, sizeof(char*));
    if (!args) return -1;
    /* argv[0] selects the applet; the wrapper rejects its own path here. */
    args[0] = (char*)"flatpak";
    for (size_t i = 0; i < count; ++i) args[i + 1] = (char*)input[i];
    char output[4096] = {0};
    int rc = invoke("flatpak", system_dir, user_dir, args, output, sizeof(output));
    if (rc) {
        fprintf(stderr, "flatpak command failed (%d):", rc);
        for (size_t i = 1; i <= count; ++i) fprintf(stderr, " %s", args[i]);
        fprintf(stderr, "\n%s\n", output);
    }
    free(args);
    return rc;
}

static int run_flatpak_output(const char* system,const char* user,
        const char* const* input,size_t count,char* output,size_t size) {
    char** args=calloc(count+2,sizeof(char*));if(!args)return -1;
    args[0]=(char*)"flatpak";
    for(size_t i=0;i<count;i++)args[i+1]=(char*)input[i];
    int rc=invoke("flatpak",system,user,args,output,size);free(args);return rc;
}

static int run_ostree(const char* repo, const char* const input[], size_t count,
        char* output, size_t output_size) {
    char repo_arg[PATH_MAX + 16];
    if (snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", repo) >= (int)sizeof(repo_arg)) return -1;
    char** args = calloc(count + 3, sizeof(char*));
    if (!args) return -1;
    args[0] = (char*)"ostree";
    args[1] = repo_arg;
    for (size_t i = 0; i < count; ++i) args[i + 2] = (char*)input[i];
    char local_output[4096] = {0};
    int rc = invoke("ostree", NULL, NULL, args,
            output && output_size ? output : local_output,
            output && output_size ? output_size : sizeof(local_output));
    const char* diagnostic = output && output_size ? output : local_output;
    if (rc && diagnostic[0]) {
        fprintf(stderr, "ostree command failed (%d):", rc);
        for (size_t i = 0; i < count; ++i) fprintf(stderr, " %s", input[i]);
        fprintf(stderr, "\n%s\n", diagnostic);
    }
    free(args);
    return rc;
}

static int make_install_ref(const char* ref, char* out, size_t size) {
    const char* first = strchr(ref, '/');
    const char* second = first ? strchr(first + 1, '/') : NULL;
    const char* third = second ? strchr(second + 1, '/') : NULL;
    if (!first || !second || !third || strchr(third + 1, '/')) return -1;
    size_t id = (size_t)(second - first - 1);
    return snprintf(out, size, "%.*s//%s", (int)id, first + 1, third + 1) < (int)size ? 0 : -1;
}

static int stage_ref(const char* staging, int uid, const char* remote, const char* install_ref,
        char* error, unsigned long error_size) {
#ifdef MATONOS_PUBLISH_HOST_TEST
    (void)uid; (void)error; (void)error_size;
    const char* args[] = {"install", "--system", "--no-deploy", "--noninteractive",
        "--assumeyes", remote, install_ref};
    return run_flatpak(staging, NULL, args, sizeof(args) / sizeof(args[0]));
#else
    char uid_arg[32]; snprintf(uid_arg, sizeof(uid_arg), "%d", uid);
    char* args[] = {(char*)MATONOS_FLATPAK_STAGE_HELPER, "stage", (char*)staging,
        uid_arg, (char*)remote, (char*)install_ref, NULL};
    char output[4096] = {0};
    fprintf(stderr, "installer staging: %s stage %s %s %s %s\n",
            MATONOS_FLATPAK_STAGE_HELPER, staging, uid_arg, remote, install_ref);
    int rc = invoke_path(MATONOS_FLATPAK_STAGE_HELPER, NULL, NULL, args, output, sizeof(output));
    if (rc) {
        /* Surface the helper's own message: it is the only place the real
         * cause (GPG/summary/network/ostree) is reported, and logcat does not
         * capture this daemon's stderr. */
        for (char* p = output; *p; ++p) if (*p == '\n' || *p == '\r') *p = ' ';
        fprintf(stderr, "installer staging failed (%d): %s\n", rc, output);
        fail(error, error_size, output[0] ? output : "installer staging helper failed with no output");
    }
    return rc;
#endif
}

static int compare_remote_commit(const char* staging_repo, const char* remote,
        const char* ref, const char* pinned) {
    char rev[1024], actual[128] = {0};
    if (snprintf(rev, sizeof(rev), "%s:%s", remote, ref) >= (int)sizeof(rev)) return -1;
    const char* args[] = {"rev-parse", rev};
    if (run_ostree(staging_repo, args, 2, actual, sizeof(actual)) != 0) return -1;
    actual[strcspn(actual, "\r\n \t")] = '\0';
    return strcmp(actual, pinned) == 0 ? 0 : -1;
}

static int remote_verification_enabled(const char* repo, const char* remote) {
    char group[192], option[208], value[64] = {0};
    if (snprintf(group, sizeof(group), "remote \"%s\"", remote) >= (int)sizeof(group) ||
            snprintf(option, sizeof(option), "--group=%s", group) >= (int)sizeof(option)) return 0;
    const char* keys[] = {"gpg-verify", "gpg-verify-summary"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        value[0] = '\0';
        const char* args[] = {option, "config", "get", keys[i]};
        if (run_ostree(repo, args, 4, value, sizeof(value))) return 0;
        value[strcspn(value, "\r\n \t")] = '\0';
        if (strcmp(value, "true")) return 0;
    }
    return 1;
}

int flatpak_remote_verified(const char* remote) {
    return valid_remote(remote) && remote_verification_enabled(MATON_RUNTIME_DIR "/repo",remote);
}

static int create_local_ref(const char* repo, const char* remote, const char* ref) {
    char option[512], rev[1024];
    if (snprintf(option, sizeof(option), "--create=%s", ref) >= (int)sizeof(option) ||
            snprintf(rev, sizeof(rev), "%s:%s", remote, ref) >= (int)sizeof(rev)) return -1;
    const char* args[] = {"refs", "--force", option, rev};
    return run_ostree(repo, args, 4, NULL, 0);
}

/* Label verified OSTree objects as code. The installation stays system-owned
 * and app domains cannot write code files. Preserve bare-user-only modes:
 * they are part of the object checksum and shared with hardlinked deployments. */
static int seal_object_files(const char* repo) {
#ifdef MATONOS_PUBLISH_HOST_TEST
    (void)repo;
    return 0;
#else
    char objects[PATH_MAX];
    if (snprintf(objects, sizeof(objects), "%s/objects", repo) >= (int)sizeof(objects)) return -1;
    static const char label[] = "u:object_r:matonos_linux_code_file:s0";
    if (setxattr(objects, "security.selinux", label, sizeof(label), 0)) return -1;
    DIR* root = opendir(objects);
    if (!root) return -1;
    struct dirent* shard;
    int rc = 0;
    while ((shard = readdir(root))) {
        if (shard->d_name[0] == '.') continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", objects, shard->d_name) >= (int)sizeof(path)) { rc = -1; break; }
        if (setxattr(path, "security.selinux", label, sizeof(label), 0)) { rc = -1; break; }
        DIR* dir = opendir(path);
        if (!dir) { rc = -1; break; }
        struct dirent* entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.') continue;
            char file[PATH_MAX];
            struct stat st;
            if (snprintf(file, sizeof(file), "%s/%s", path, entry->d_name) >= (int)sizeof(file) ||
                    lstat(file, &st)) { rc = -1; break; }
            /* bare-user-only stores symlink objects as real symlinks. */
            if (S_ISLNK(st.st_mode)) {
                if (lsetxattr(file, "security.selinux", label, sizeof(label), 0)) { rc = -1; break; }
                continue;
            }
            /* bare-user-only permits only canonical 0755 mode bits. Reject
             * an unsafe object instead of changing authenticated metadata. */
            if (!S_ISREG(st.st_mode) || st.st_uid != AID_SYSTEM ||
                    (st.st_mode & 07022)) { errno = EPERM; rc = -1; break; }
            /* Apply the code label after signature verification. Android
             * policy must authorize linuxd to relabel from the repo type. */
            if (setxattr(file, "security.selinux", label, sizeof(label), 0)) { rc = -1; break; }
        }
        closedir(dir);
        if (rc) break;
    }
    closedir(root);
    return rc;
#endif
}

static int transfer_and_verify(const char* staging, const char* target,
        const char* remote, const char* ref, const char* commit) {
    char source_repo[PATH_MAX], target_repo[PATH_MAX];
    if (snprintf(source_repo, sizeof(source_repo), "%s/repo", staging) >= (int)sizeof(source_repo) ||
            snprintf(target_repo, sizeof(target_repo), "%s/repo", target) >= (int)sizeof(target_repo)) return -1;
    /* Check the pin before mutating the target; pull-local's signed commit
     * verification is the actual trust gate, never ostree show. */
    if (compare_remote_commit(source_repo, remote, ref, commit)) return -2;
    char remote_arg[160];
    if (snprintf(remote_arg, sizeof(remote_arg), "--remote=%s", remote) >= (int)sizeof(remote_arg)) return -1;
    const char* pull[] = {"pull-local", "--untrusted", "--gpg-verify",
        remote_arg, source_repo, ref};
    if (run_ostree(target_repo, pull, sizeof(pull) / sizeof(pull[0]), NULL, 0)) return -3;
    return 0;
}

/* Inspect only verified checkouts, fd-relative and without following links.
 * Reject unsafe authenticated metadata rather than rewriting signed objects. */
static int sanitize_checkout(int dir, dev_t device, unsigned depth, int is_user, int in_files) {
    DIR* entries=fdopendir(dup(dir));if(!entries)return -1;
    struct dirent* e;int rc=0;
    while(!rc && (e=readdir(entries))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;
        if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)||st.st_dev!=device || (st.st_mode & 06000)) {rc=-1;break;}
        if(S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];ssize_t n=readlinkat(dir,e->d_name,target,sizeof(target)-1);
            if(n<=0 || n==(ssize_t)sizeof(target)-1){rc=-1;break;}
            target[n]=0;
            if(target[0]=='/') {
                /* Flatpak files/ is /app or /usr inside the sandbox. Absolute
                 * links in signed runtimes must never resolve against the
                 * management host root. Rewrite the checkout link only;
                 * authenticated repository objects stay byte-for-byte intact. */
                const char* suffix=NULL;
                if(!is_user && (!strncmp(target,"/usr/",5)||!strcmp(target,"/usr")))suffix=target+4;
                else if(is_user && (!strncmp(target,"/app/",5)||!strcmp(target,"/app")))suffix=target+4;
                else if(!is_user && (!strncmp(target,"/etc/",5) || !strcmp(target,"/etc") ||
                        !strcmp(target,"/run/host/monitor/resolv.conf")))suffix=target;
                if(!suffix){rc=-1;break;}
                char confined[PATH_MAX];size_t used=0;
                unsigned up=in_files?depth-1:depth;
                for(unsigned i=0;i<up;i++) {memcpy(confined+used,"../",3);used+=3;}
                if(snprintf(confined+used,sizeof(confined)-used,"%s%s",in_files?"":"files",in_files && suffix[0]=='/'?suffix+1:suffix)>=(int)(sizeof(confined)-used)) {rc=-1;break;}
                /* Validate before changing the link. */
                char check[PATH_MAX];snprintf(check,sizeof(check),"%s",confined);
                int level=(int)depth;char* save=NULL;
                for(char* part=strtok_r(check,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
                    if(!strcmp(part,"..")){if(--level<0){rc=-1;break;}}
                    else if(strcmp(part,"."))level++;
                }
                if(rc)break;
                if(maton_storage_context("u:object_r:matonos_flatpak_deployment_file:s0")){rc=-1;break;}
                rc=unlinkat(dir,e->d_name,0) || symlinkat(confined,dir,e->d_name);
                if(maton_storage_context(NULL))rc=-1;
                if(rc)break;
                snprintf(target,sizeof(target),"%s",confined);
            }
            int level=(int)depth;char* save=NULL;
            for(char* part=strtok_r(target,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
                if(!strcmp(part,"..")){if(--level<0){rc=-1;break;}}
                else if(strcmp(part,"."))level++;
            }
        } else if(S_ISREG(st.st_mode)||S_ISDIR(st.st_mode)) {
            int fd=openat(dir,e->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(fd<0){rc=-1;break;}
            char attrs[4096];ssize_t n=flistxattr(fd,attrs,sizeof(attrs));
            if(n<0)rc=-1;
            for(ssize_t off=0;!rc && off<n;) {
                const char* attr=attrs+off;size_t length=strnlen(attr,(size_t)(n-off));
                if(length==(size_t)(n-off) || strcmp(attr,"security.selinux"))rc=-1;
                off+=(ssize_t)length+1;
            }
            if(!rc && S_ISDIR(st.st_mode))rc=depth>=128?-1:sanitize_checkout(fd,device,depth+1,is_user,in_files || (depth==0 && !strcmp(e->d_name,"files")));
            close(fd);
        } else rc=-1;
    }
    closedir(entries);return rc;
}
static int sanitize_deployed(const char* system,const char* user,const char* ref,int is_user) {
    const char* args[]={is_user?"--user":"--system","info","--show-location",ref};
    char path[PATH_MAX]={0},resolved[PATH_MAX];
    if(run_flatpak_output(system,user,args,4,path,sizeof(path)))return -1;
    path[strcspn(path,"\r\n")]=0;
    const char* root=is_user?user:system;
    if(!realpath(path,resolved)||strncmp(resolved,root,strlen(root))||resolved[strlen(root)]!='/')return -1;
    int fd=open(resolved,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat st;int rc=fstat(fd,&st)||sanitize_checkout(fd,st.st_dev,0,is_user,0);close(fd);return rc;
}

static int deploy_ref(const char* system_dir, const char* user_dir,
        const char* remote, const char* install_ref, int is_user) {
    const char* install_user[] = {"install", "--user", "--no-pull", "--noninteractive",
        "--assumeyes", remote, install_ref};
    const char* install_system[] = {"install", "--system", "--no-pull", "--noninteractive",
        "--assumeyes", remote, install_ref};
    return run_flatpak(system_dir, user_dir,
            is_user ? install_user : install_system,
            sizeof(install_user) / sizeof(install_user[0]));
}

/* Give every Flatpak installation the session bus at global scope. These
 * calls have no application ID, so Flatpak stores the socket grant in the
 * installation's global overrides. Reapplying is idempotent and repairs an
 * installation after its config is recreated. */
static int prepare_global_session_bus(const char* system_dir, const char* user_dir) {
    const char* system_override[] = {"override", "--system", "--socket=session-bus"};
    const char* user_override[] = {"override", "--user", "--socket=session-bus"};
    return run_flatpak(system_dir, user_dir, system_override,
                sizeof(system_override) / sizeof(system_override[0])) ||
            run_flatpak(system_dir, user_dir, user_override,
                sizeof(user_override) / sizeof(user_override[0]));
}

/* Publishing takes the staging tree back from FPINSTALL (linuxd has
 * CAP_CHOWN): the stub can no longer change it, and linuxd can read the
 * config Flatpak rewrote and write the local refs it creates. */
static int reclaim_tree(int parent, const char* name) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) return -1;
    if (fchownat(parent, name, AID_SYSTEM, AID_SYSTEM, AT_SYMLINK_NOFOLLOW)) return -1;
    if (!S_ISDIR(st.st_mode)) return (S_ISREG(st.st_mode)||S_ISLNK(st.st_mode))?0:-1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    DIR* dir = fdopendir(dup(fd));
    int rc = dir ? 0 : -1;
    struct dirent* entry;
    while (!rc && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        rc = reclaim_tree(fd, entry->d_name);
    }
    if (dir) closedir(dir);
    close(fd);
    return rc;
}

/* The installer's --user installation needs the remote (and keyring) before
 * linuxd checks and publishes into it; copied from the runtime install. */
/* ostree init inherits linuxd's restrictive umask; stubs read the
 * deployment, so match Flatpak's own modes (dirs 0755, files 0644). */
static int repo_world_readable(const char* path, const struct stat* st, int type, struct FTW* ftw) {
    (void)ftw;
    if (type == FTW_SL) return 0;
    return chmod(path, type == FTW_D || type == FTW_DP ? 0755 : (st->st_mode & 0111 ? 0755 : 0644));
}

static int ensure_user_remote(const char* system_repo, const char* user_dir, const char* remote) {
    char config[PATH_MAX], group[192], line[512];
    if (snprintf(config, sizeof(config), "%s/repo/config", user_dir) >= (int)sizeof(config) ||
            snprintf(group, sizeof(group), "[remote \"%s\"]", remote) >= (int)sizeof(group)) return -1;
    FILE* in = fopen(config, "re");
    if (!in) return -1;
    int present = 0;
    while (!present && fgets(line, sizeof(line), in)) present = !strncmp(line, group, strlen(group));
    fclose(in);
    return present ? 0 : seed_remote_from(system_repo, user_dir, remote);
}

struct dependency_ref {
    char ref[512];
    char commit[128];
    int extension;
    int user;
};
static int resolve_ref_commit(const char* repo, const char* remote, const char* ref,
        char* out, size_t size);
static int read_commit_file(const char* repo, const char* commit, const char* path,
        char* out, size_t size);
static int app_id_from_ref(const char* ref, char* out, size_t size);

/* A single ref uses the existing 512-byte protocol representation. */
static int valid_staged_ref(const char* ref) {
    if (strlen(ref) >= 512) return 0;
    size_t parts = 0, length = 0;
    for (const char* p = ref; ; p++) {
        if (!*p || *p == '/') {
            if (!length || (length == 1 && p[-1] == '.') ||
                    (length == 2 && p[-1] == '.' && p[-2] == '.')) return 0;
            if (!*p) break;
            parts++; length = 0;
        } else {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return 0;
            length++;
        }
    }
    return parts == 3;
}

/* Flatpak owns dependency selection. Only enumerate its completed staging pull.
 * ExtensionOf identifies deployment order; it never selects additional refs. */
static int staged_refs(const char* repo, const char* remote, const char* app_ref,
        const char* runtime_ref, struct dependency_ref** result, size_t* count) {
    char* list = NULL;
    struct dependency_ref* refs = NULL;
    size_t capacity = 0;
    char prefix[192], app_id[256];
    if (snprintf(prefix, sizeof(prefix), "%s:", remote) >= (int)sizeof(prefix) ||
            app_id_from_ref(app_ref, app_id, sizeof(app_id))) goto bad;
    char repo_arg[PATH_MAX + 16];
    if (snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", repo) >= (int)sizeof(repo_arg)) goto bad;
    /* "refs <remote>" filters by ref-name prefix, not remote, and matches
     * nothing; list everything and keep this remote's refs. */
    char* args[] = {"ostree", repo_arg, "refs", NULL};
    if (invoke_path_capture(tool_path("ostree"), NULL, NULL, args, NULL, 0, &list)) goto bad;
    *count = 0;
    int app_found = 0, runtime_found = 0;
    char* save = NULL;
    for (char* ref = strtok_r(list, "\n", &save); ref; ref = strtok_r(NULL, "\n", &save)) {
        if (strncmp(ref, prefix, strlen(prefix))) continue;
        ref += strlen(prefix);
        if (!valid_staged_ref(ref) || (strncmp(ref, "runtime/", 8) && strncmp(ref, "app/", 4))) goto bad;
        const char* id = strchr(ref, '/') + 1;
        const char* end = strchr(id, '/');
        if (!end) goto bad;
        size_t n = (size_t)(end - id), app_len = strlen(app_id);
        char install[512];
        if (make_install_ref(ref, install, sizeof(install))) goto bad;
        if ((n >= 6 && !strncmp(end - 6, ".Debug", 6)) ||
                (n >= 8 && !strncmp(end - 8, ".Sources", 8))) continue;
        if (*count == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            if (capacity > SIZE_MAX / 2 || next > SIZE_MAX / sizeof(*refs)) goto bad;
            struct dependency_ref* grown = realloc(refs, next * sizeof(*refs));
            if (!grown) goto bad;
            refs = grown; capacity = next;
        }
        for (size_t i = 0; i < *count; i++) if (!strcmp(refs[i].ref, ref)) goto bad;
        struct dependency_ref* dep = &refs[(*count)++];
        if (snprintf(dep->ref, sizeof(dep->ref), "%s", ref) >= (int)sizeof(dep->ref) ||
                resolve_ref_commit(repo, remote, ref, dep->commit, sizeof(dep->commit)) ||
                !valid_commit(dep->commit)) goto bad;
        char* metadata = NULL;
        char* metadata_args[] = {"ostree", repo_arg, "cat", dep->commit, "/metadata", NULL};
        int metadata_rc = invoke_path_capture(tool_path("ostree"), NULL, NULL, metadata_args, NULL, 0, &metadata);
        if (metadata_rc) { free(metadata); goto bad; }
        dep->extension = !strncmp(metadata, "[ExtensionOf]", 13) || strstr(metadata, "\n[ExtensionOf]") != NULL;
        free(metadata);
        dep->user = !strcmp(ref, app_ref) ||
                (n > app_len && !strncmp(id, app_id, app_len) && id[app_len] == '.');
        app_found |= !strcmp(ref, app_ref);
        runtime_found |= !strcmp(ref, runtime_ref);
    }
    if (!app_found || !runtime_found) goto bad;
    free(list); *result = refs; return 0;
bad:
    free(list); free(refs); return -1;
}

int flatpak_publish_tracked(const char* app_ref, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        const char* staging_dir, const char* system_dir, const char* user_dir,
        char* error, unsigned long error_size,
        int (*record)(const char*, void*),
        int (*complete)(const char* const*, size_t, void*), void* context) {
    char app_install_ref[512], runtime_install_ref[512];
    char staging_repo[PATH_MAX];
    if (!app_ref || !runtime_ref || !valid_remote(remote) || !staging_dir ||
            !system_dir || !user_dir || !valid_commit(app_commit) || !valid_commit(runtime_commit) ||
            strncmp(app_ref, "app/", 4) || strncmp(runtime_ref, "runtime/", 8) ||
            make_install_ref(app_ref, app_install_ref, sizeof(app_install_ref)) ||
            make_install_ref(runtime_ref, runtime_install_ref, sizeof(runtime_install_ref))) {
        fail(error, error_size, "invalid ref, remote, or pinned commit");
        return -1;
    }
#ifndef MATONOS_PUBLISH_HOST_TEST
    unsigned app_id=0;
    if((!maton_parse_install_path(user_dir,&app_id) && !pending_deployment_path(user_dir)) || strcmp(system_dir,MATON_RUNTIME_DIR) || maton_store_prepare()) {
        fail(error,error_size,"system-wide installation roots unavailable or inconsistent");return -1;
    }
#endif
    if (snprintf(staging_repo, sizeof(staging_repo), "%s/repo", staging_dir) >= (int)sizeof(staging_repo)) {
        fail(error, error_size, "staging path too long"); return -1;
    }
    char system_repo[PATH_MAX], user_repo[PATH_MAX];
    if (snprintf(system_repo, sizeof(system_repo), "%s/repo", system_dir) >= (int)sizeof(system_repo) ||
            snprintf(user_repo, sizeof(user_repo), "%s/repo", user_dir) >= (int)sizeof(user_repo)) {
        fail(error, error_size, "installation path too long"); return -1;
    }
    {
        const char* slash = strrchr(staging_dir, '/');
        char parent[PATH_MAX];
        if (!slash || (size_t)(slash - staging_dir) >= sizeof(parent)) {
            fail(error, error_size, "invalid staging path"); return -1;
        }
        memcpy(parent, staging_dir, (size_t)(slash - staging_dir)); parent[slash - staging_dir] = '\0';
        int up = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int rc = up < 0 ? -1 : reclaim_tree(up, slash + 1);
        if (up >= 0) close(up);
        if (rc) { fail(error, error_size, "cannot take back the staging tree"); return -1; }
    }
    {
        /* Each publish targets a fresh versioned deployment dir (only
         * .lease): create its repository before configuring the remote. */
        struct stat repo_st;
        const char* init_args[] = {"init", "--mode=bare-user-only"};
        if (stat(user_repo, &repo_st) && (run_ostree(user_repo, init_args, 2, NULL, 0) ||
                nftw(user_repo, repo_world_readable, 16, FTW_PHYS))) {
            fail(error, error_size, "cannot create the app installation repository"); return -1;
        }
    }
    if (ensure_user_remote(system_repo, user_dir, remote)) {
        fail(error, error_size, "cannot configure the remote in the app installation"); return -1;
    }
    if (!remote_verification_enabled(staging_repo, remote) ||
            !remote_verification_enabled(system_repo, remote) ||
            !remote_verification_enabled(user_repo, remote)) {
        fail(error, error_size, "remote GPG verification and summary verification must both be enabled"); return -1;
    }
    /* Use one complete locale set in R, S and U. Partial OSTree locale
     * subsets can produce trees that pull-local cannot reconstruct. */
    const char* config[] = {"config", "--system", "--set", "languages", "*"};
    const char* user_config[] = {"config", "--user", "--set", "languages", "*"};
    if (run_flatpak(staging_dir, NULL, config, 5) || run_flatpak(system_dir, NULL, config, 5) ||
            run_flatpak(system_dir, user_dir, user_config, 5)) {
        fail(error, error_size, "cannot set complete Flatpak locale configuration"); return -1;
    }
    /* R is a staging repository, not an installation. S and U are actual
     * Flatpak installations and each needs the open valve at global scope. */
    if (prepare_global_session_bus(system_dir, user_dir)) {
        fail(error, error_size, "cannot set global Flatpak session-bus overrides"); return -1;
    }
    /* flatpak_prepare staged the app transaction as FPINSTALL; the pinned
     * commits are verified against that staging repository below. */
    if (compare_remote_commit(staging_repo, remote, runtime_ref, runtime_commit) ||
            compare_remote_commit(staging_repo, remote, app_ref, app_commit)) {
        fail(error, error_size, "staged commit does not match the resolved transaction pin"); return -1;
    }
    struct dependency_ref* refs = NULL;
    size_t count = 0;
    if (staged_refs(staging_repo, remote, app_ref, runtime_ref, &refs, &count)) {
        fail(error, error_size, "invalid or missing staged Flatpak refs, or allocation failure"); return -1;
    }
    /* Verify every selected ref before deploying anything; strictly offline. */
    for (size_t i = 0; i < count; i++) {
        const char* target = refs[i].user ? user_dir : system_dir;
        if (create_local_ref(staging_repo, remote, refs[i].ref) ||
                transfer_and_verify(staging_dir, target, remote, refs[i].ref, refs[i].commit)) {
            fail(error, error_size, "dependency signature verification or publication failed"); goto bad;
        }
    }
    if (seal_object_files(system_repo) || seal_object_files(user_repo)) {
        fail(error, error_size, "cannot relabel and seal verified OSTree objects"); goto bad;
    }
    /* Persist protection before deployment, including partial failures. */
    if (record) for (size_t i = 0; i < count; i++) {
        if (!refs[i].user && record(refs[i].ref, context)) {
            fail(error, error_size, "cannot protect shared runtime references"); goto bad;
        }
    }
    /* Bases/runtimes, then extensions, then the app. */
    for (int phase = 0; phase < 3; phase++) {
        for (size_t i = 0; i < count; i++) {
            int order = !strcmp(refs[i].ref, app_ref) ? 2 : refs[i].extension ? 1 : 0;
            char install[512];
            if (order != phase) continue;
            if (make_install_ref(refs[i].ref, install, sizeof(install)) ||
                    deploy_ref(system_dir, user_dir, remote, install, refs[i].user) ||
                    sanitize_deployed(system_dir,user_dir,refs[i].ref,refs[i].user)) {
                fail(error, error_size, "Flatpak no-pull dependency deployment failed"); goto bad;
            }
        }
    }
    if (complete) {
        if (count > SIZE_MAX / sizeof(char*)) goto bad;
        const char** shared = malloc(count * sizeof(*shared));
        if (!shared) goto bad;
        size_t shared_count = 0;
        for (size_t i = 0; i < count; i++) if (!refs[i].user) shared[shared_count++] = refs[i].ref;
        int rc = complete(shared, shared_count, context);
        free(shared);
        if (rc) {
            fail(error, error_size, "published app but runtime reference cleanup was deferred"); goto bad;
        }
    }
    if (error && error_size) error[0] = '\0';
    free(refs); return 0;
bad:
    free(refs); return -1;
}

int flatpak_publish(const char* app_ref, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        const char* staging_dir, const char* system_dir, const char* user_dir,
        char* error, unsigned long error_size) {
    return flatpak_publish_tracked(app_ref, app_commit, runtime_ref, runtime_commit,
            remote, staging_dir, system_dir, user_dir, error, error_size, NULL, NULL, NULL);
}

/* ---- Installer staging (untrusted transport) ----------------------------------------
 * Stage the signed refs and read the app's /metadata and exported desktop
 * entry straight from the staged commit, so the bridge can create the stub
 * as transaction diagnostics only. These bytes are untrusted until publication;
 * the bridge must read final metadata from the verified deployment. */

static int resolve_ref_commit(const char* repo, const char* remote, const char* ref,
        char* out, size_t size) {
    char rev[1024];
    if (snprintf(rev, sizeof(rev), "%s:%s", remote, ref) >= (int)sizeof(rev)) return -1;
    const char* args[] = {"rev-parse", rev};
    if (run_ostree(repo, args, 2, out, size)) return -1;
    out[strcspn(out, "\r\n \t")] = '\0';
    return strlen(out) == 64 ? 0 : -1;
}

static int read_commit_file(const char* repo, const char* commit, const char* path,
        char* out, size_t size) {
    if (size) out[0] = '\0';
    const char* args[] = {"cat", commit, path};
    return run_ostree(repo, args, 3, out, size);
}

static int metadata_runtime_ref(const char* metadata, char* out, size_t size) {
    const char* section = strstr(metadata ? metadata : "", "[Application]");
    if (!section) return -1;
    const char* end = strchr(section + 1, '[');
    for (const char* line = section; line && (!end || line < end); ) {
        const char* nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len > 8 && !strncmp(line, "runtime=", 8)) {
            size_t vlen = len - 8;
            while (vlen && (line[8 + vlen - 1] == '\r' || line[8 + vlen - 1] == ' ')) --vlen;
            /* Metadata names the runtime as <id>/<arch>/<branch>; the rest of
             * the pipeline uses full refs (runtime/<id>/<arch>/<branch>). */
            if (!vlen || vlen + 8 >= size) return -1;
            int kinded = vlen > 8 && !strncmp(line + 8, "runtime/", 8);
            snprintf(out, size, "%s%.*s", kinded ? "" : "runtime/", (int)vlen, line + 8);
            return 0;
        }
        if (!nl) break;
        line = nl + 1;
    }
    return -1;
}

static int app_id_from_ref(const char* ref, char* out, size_t size) {
    if (!ref || strncmp(ref, "app/", 4)) return -1;
    const char* first = strchr(ref + 4, '/');
    if (!first) return -1;
    size_t len = (size_t)(first - (ref + 4));
    if (!len || len >= size) return -1;
    memcpy(out, ref + 4, len); out[len] = '\0';
    return 0;
}

/* A private, ephemeral installation supplies only the pinned remote/keyring.
 * Network I/O runs through the same fixed FPINSTALL helper as downloads. */
int flatpak_available_commit(const char* ref, const char* remote, const char* operation,
        char* commit, unsigned long size, char* error, unsigned long error_size) {
    char staging[PATH_MAX], repo[PATH_MAX], uid[32];
    if (!ref || strncmp(ref,"app/",4) || !valid_remote(remote) ||
            !valid_operation_id(operation) || size < 65) {
        fail(error,error_size,"invalid update query");return -1;
    }
    if(create_staging(MATON_FPINSTALL_UID,operation,staging,sizeof(staging))) {
        fail(error,error_size,"cannot create query staging");return -1;
    }
    if(snprintf(repo,sizeof(repo),"%s/repo",staging)>=(int)sizeof(repo)) {
        (void)flatpak_remove_staging(MATON_FPINSTALL_UID,operation);
        fail(error,error_size,"query path too long");return -1;
    }
    const char* init[]={"init","--mode=bare-user-only"};
    int rc=run_ostree(repo,init,2,NULL,0);
    if(!rc)rc=seed_remote(staging,remote);
    if(!rc && !remote_verification_enabled(repo,remote))rc=-1;
    if(!rc)rc=give_to_installer(staging,MATON_FPINSTALL_UID,remote);
    snprintf(uid,sizeof(uid),"%d",MATON_FPINSTALL_UID);
    char output[4096]={0};
    char* args[]={(char*)MATONOS_FLATPAK_STAGE_HELPER,"check",staging,uid,
        (char*)remote,(char*)ref,NULL};
    if(!rc)rc=invoke_path(MATONOS_FLATPAK_STAGE_HELPER,staging,NULL,args,output,sizeof(output));
    if(!rc)rc=store_parse_commit(output,commit,size);
    if(flatpak_remove_staging(MATON_FPINSTALL_UID,operation))rc=-1;
    if(rc)fail(error,error_size,"cannot query authenticated pinned remote commit");
    return rc;
}

int flatpak_prepare(const char* app_ref, const char* remote, int installer_uid,
        const char* operation_id,
        char* app_commit, unsigned long app_commit_size,
        char* runtime_ref_out, unsigned long runtime_ref_size,
        char* runtime_commit, unsigned long runtime_commit_size,
        char* metadata, unsigned long metadata_size,
        char* desktop, unsigned long desktop_size,
        char* error, unsigned long error_size) {
    char staging_dir[PATH_MAX], staging_repo[PATH_MAX], app_install[512];
    int created = 0;
    char runtime_ref[512] = {0}, app_id[256];
    if (!app_ref || !remote || !operation_id || !app_commit || !runtime_ref_out ||
            !runtime_commit || !metadata || !desktop || !*remote ||
            strncmp(app_ref, "app/", 4) || !valid_remote(remote) ||
            !valid_operation_id(operation_id)) {
        fail(error, error_size, "invalid prepare request"); return -1;
    }
    if (create_staging(installer_uid, operation_id, staging_dir, sizeof(staging_dir))) {
        fail(error, error_size, "cannot create installer staging directory"); return -1;
    }
    created = 1;
    if (snprintf(staging_repo, sizeof(staging_repo), "%s/repo", staging_dir) >= (int)sizeof(staging_repo)) {
        fail(error, error_size, "staging path too long"); goto bad;
    }
    {
        /* A real repository (Flatpak only opens an existing one), created
         * while linuxd still owns the operation. */
        const char* init_args[] = {"init", "--mode=bare-user-only"};
        if (run_ostree(staging_repo, init_args, 2, NULL, 0)) {
            fail(error, error_size, "cannot create staging repository"); goto bad;
        }
    }
    if (seed_remote(staging_dir, remote)) {
        fail(error, error_size, "runtime remote configuration or trusted keyring is missing"); goto bad;
    }
    if (!remote_verification_enabled(staging_repo, remote)) {
        fail(error, error_size, "remote GPG verification and summary verification must both be enabled"); goto bad;
    }
    /* Configure complete locales before the transaction selects related refs. */
    const char* languages[] = {"config", "--system", "--set", "languages", "*"};
    if (run_flatpak(staging_dir, NULL, languages, 5)) {
        fail(error, error_size, "cannot set complete staging locale configuration"); goto bad;
    }
    /* The staging pull runs as the installer UID: hand it the repo and the
     * seeded remote, and give it private home/cache/tmp/runtime dirs (the
     * wrapper points HOME, TMPDIR and Flatpak's caches here). */
    if (give_to_installer(staging_dir, installer_uid, remote)) {
        fail(error, error_size, "cannot prepare installer-owned staging files"); goto bad;
    }
    if (make_install_ref(app_ref, app_install, sizeof(app_install)) ||
            app_id_from_ref(app_ref, app_id, sizeof(app_id))) {
        fail(error, error_size, "invalid application ref"); goto bad;
    }
    if (stage_ref(staging_dir, installer_uid, remote, app_install, error, error_size)) goto bad;
    if (resolve_ref_commit(staging_repo, remote, app_ref, app_commit, app_commit_size)) {
        fail(error, error_size, "cannot resolve staged application commit"); goto bad;
    }
    metadata[0] = '\0';
    if (read_commit_file(staging_repo, app_commit, "/metadata", metadata, metadata_size)) {
        fail(error, error_size, "cannot read staged application metadata"); goto bad;
    }
    if (metadata_runtime_ref(metadata, runtime_ref, sizeof(runtime_ref))) {
        fail(error, error_size, "staged application declares no runtime"); goto bad;
    }
    if (resolve_ref_commit(staging_repo, remote, runtime_ref, runtime_commit, runtime_commit_size)) {
        fail(error, error_size, "cannot resolve staged runtime commit"); goto bad;
    }
    snprintf(runtime_ref_out, runtime_ref_size, "%s", runtime_ref);
    char desktop_path[512];
    desktop[0] = '\0';
    if (snprintf(desktop_path, sizeof(desktop_path),
            "/export/share/applications/%s.desktop", app_id) < (int)sizeof(desktop_path))
        (void)read_commit_file(staging_repo, app_commit, desktop_path, desktop, desktop_size);
    if (error && error_size) error[0] = '\0';
    return 0;
bad:
    if (created) remove_staging_as(installer_uid, operation_id);
    return -1;
}

/* Phase B file primitive. The registered-operation verifier must supply a
 * pinned, exclusively linuxd-owned directory, never a producer path. A seal
 * alone is not a signed-deployment verification or publication permission. */
int flatpak_seal_staged_file(int directory, const char* component,
        char* error, unsigned long error_size) {
    struct stat st;
    if (fstat(directory, &st) || !S_ISDIR(st.st_mode) ||
            st.st_uid != AID_SYSTEM || (st.st_mode & 0022)) {
        fail(error, error_size, "staging directory is not exclusively linuxd-owned");
        errno = EPERM;
        return -1;
    }
    return maton_verity_openat(directory, component, st.st_dev, AID_SYSTEM,
            error, error_size);
}

/* Registered compositor transaction backend. No Flatpak deployment command is
 * used here: OSTree only imports signed objects and creates comparison trees. */
#include "StagedTree.h"
#include "StagedDeployment.h"
#include "StagedProjection.h"

static int staged_app_uid(int uid) { return uid>=10000 && uid%100000>=10000 && uid%100000<=19999; }
static int staged_parent(int uid) {
    char path[192];
    if(!staged_app_uid(uid)||snprintf(path,sizeof(path),MATON_STAGING_ROOT "/%d",uid)>=(int)sizeof(path))return -1;
    return open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
}
static int staged_name(char* out,size_t size,const char* prefix,const char* operation) {
    return valid_operation_id(operation)&&snprintf(out,size,".%s-%s",prefix,operation)<(int)size?0:-1;
}
/* Pin only the selected remote's copied keyring. Do not retain configured
 * external keyring paths whose bytes could change after registration. */
static int staged_pin_remote(const char* installation,const char* remote) {
    char repo[PATH_MAX],group[192],url[8192]={0},config[PATH_MAX];
    if(snprintf(repo,sizeof(repo),"%s/repo",installation)>=(int)sizeof(repo)||
            snprintf(group,sizeof(group),"--group=remote \"%s\"",remote)>=(int)sizeof(group)||
            snprintf(config,sizeof(config),"%s/config",repo)>=(int)sizeof(config))return -1;
    const char* get[]={group,"config","get","url"};
    if(run_ostree(repo,get,4,url,sizeof(url)))return -1;
    size_t n=strlen(url);while(n&&(url[n-1]=='\n'||url[n-1]=='\r'))url[--n]=0;
    if(!n||strchr(url,'\n')||strchr(url,'\r'))return -1;
    FILE* file=fopen(config,"we");if(!file)return -1;
    int rc=fprintf(file,"[core]\nrepo_version=1\nmode=bare-user-only\n[remote \"%s\"]\nurl=%s\ngpg-verify=true\ngpg-verify-summary=true\n",remote,url)<0?-1:0;
    if(fflush(file)||fsync(fileno(file)))rc=-1;
    if(fclose(file))rc=-1;
    return rc;
}
int flatpak_begin_staged_install(int uid,const char* operation,const char* remote,
        char* error,unsigned long size) {
    if(error&&size)error[0]=0;
    if(!staged_app_uid(uid)||!valid_operation_id(operation)||!valid_remote(remote)||maton_store_prepare()) {
        fail(error,size,"invalid compositor staging registration");return -1;
    }
    int root=open(MATON_STAGING_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0)return -1;
    char owner[32];snprintf(owner,sizeof(owner),"%d",uid);
    int parent=maton_storage_child(root,owner,"u:object_r:matonos_flatpak_staging_file:s0",0711);
    close(root);if(parent<0)return -1;
    char trust_name[96]={0},trust[PATH_MAX],path[PATH_MAX],repo[PATH_MAX];int op=-1,rc=-1,trust_created=0,operation_created=0;
    if(staged_name(trust_name,sizeof(trust_name),"trust",operation)||
            snprintf(path,sizeof(path),MATON_STAGING_ROOT "/%d/%s",uid,operation)>=(int)sizeof(path)||
            snprintf(trust,sizeof(trust),MATON_STAGING_ROOT "/%d/%s",uid,trust_name)>=(int)sizeof(trust)||
            mkdirat(parent,trust_name,0700))goto done;
    trust_created=1;
    if(snprintf(repo,sizeof(repo),"%s/repo",trust)>=(int)sizeof(repo))goto done;
    const char* init[]={"init","--mode=bare-user-only"};
    if(run_ostree(repo,init,2,NULL,0)||seed_remote(trust,remote)||!remote_verification_enabled(repo,remote)||staged_pin_remote(trust,remote))goto done;
    if(mkdirat(parent,operation,0700))goto done;
    operation_created=1;
    op=openat(parent,operation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(op<0)goto done;
    const char* dirs[]={"installation","home","cache","tmp","run"};
    for(size_t i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++){
        if(mkdirat(op,dirs[i],0700))goto done;
        int fd=openat(op,dirs[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0)goto done;
        int bad=strcmp(dirs[i],"installation")?fchown(fd,uid,uid):0;close(fd);if(bad)goto done;
    }
    /* Only untrusted transport configuration is placed in producer space.
     * The private trust repository above remains linuxd-owned throughout. */
    char installation[PATH_MAX],trusted_repo_path[PATH_MAX];
    if(snprintf(trusted_repo_path,sizeof(trusted_repo_path),"%s/repo",trust)>=(int)sizeof(trusted_repo_path))goto done;
    if(snprintf(installation,sizeof(installation),"%s/installation",path)>=(int)sizeof(installation)||
            snprintf(repo,sizeof(repo),"%s/repo",installation)>=(int)sizeof(repo)||
            run_ostree(repo,init,2,NULL,0)||seed_remote_from(trusted_repo_path,installation,remote)||
            hand_over_tree(op,"installation",uid)||fchmod(op,0750)||fchown(op,uid,AID_SYSTEM)||fsync(parent))goto done;
    rc=0;
done:
    if(op>=0)close(op);
    if(rc){if(operation_created)(void)remove_tree_at(parent,operation);if(trust_created)(void)remove_tree_at(parent,trust_name);fail(error,size,"cannot initialize pinned compositor transaction");}
    close(parent);return rc;
}
static int staged_open_path(int root,const char* path) {
    /* Every component is derived from a validated ref/commit, never manifest
     * path input. O_NOFOLLOW on every intermediate component is essential. */
    char copy[1024];if(strlen(path)>=sizeof(copy))return -1;strcpy(copy,path);
    int fd=dup(root);char* save=NULL;
    for(char* part=strtok_r(copy,"/",&save);part&&fd>=0;part=strtok_r(NULL,"/",&save)){
        if(!strcmp(part,".")||!strcmp(part,"..")){close(fd);return -1;}
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(fd);fd=next;
    }
    return fd;
}
static int staged_zone_tree(int dir,dev_t device,int zone,struct maton_staged_inodes* all,unsigned depth) {
    if(depth>128)return -1;
    DIR* entries=fdopendir(dup(dir));if(!entries)return -1;rewinddir(entries);
    int rc=0;struct dirent* e;
    while(!rc&&(e=readdir(entries))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)||st.st_dev!=device){rc=-1;break;}
        if(S_ISDIR(st.st_mode)){
            int fd=openat(dir,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(fd<0)rc=-1;else{rc=staged_zone_tree(fd,device,zone,all,depth+1);close(fd);}
        }else if(S_ISREG(st.st_mode)){
            struct maton_staged_inode* item=maton_staged_inode(all,&st);
            if(!item||(item->zone&&item->zone!=zone))rc=-1;else item->zone=zone;
        }
    }
    closedir(entries);return rc;
}
static int staged_recount_tree(int dir,struct maton_staged_inodes* all,unsigned depth) {
    if(depth>128)return -1;
    DIR* entries=fdopendir(dup(dir));if(!entries)return -1;rewinddir(entries);
    int rc=0;struct dirent* e;
    while(!rc&&(e=readdir(entries))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){rc=-1;break;}
        if(S_ISDIR(st.st_mode)){
            int fd=openat(dir,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(fd<0)rc=-1;else{rc=staged_recount_tree(fd,all,depth+1);close(fd);}
        }else if(S_ISREG(st.st_mode)){
            struct maton_staged_inode* item=maton_staged_inode(all,&st);
            if(!item||item->links!=st.st_nlink)rc=-1;else item->seen++;
        }
    }
    closedir(entries);return rc;
}
static int staged_recount(int installation,int trusted) {
    struct maton_staged_inodes* all=calloc(1,sizeof(*all));if(!all)return -1;
    int rc=staged_recount_tree(installation,all,0);
    if(!rc)rc=staged_recount_tree(trusted,all,0);
    for(size_t i=0;!rc&&i<MATON_STAGED_INODE_BUCKETS;i++)
        for(struct maton_staged_inode* item=all->buckets[i];item;item=item->next)
            if(item->seen!=item->links){rc=-1;break;}
    maton_staged_inodes_free(all);return rc;
}
/* Metadata strings are authenticated bytes from the trusted comparison tree. */
static int staged_metadata_value(const char* metadata,const char* section,const char* key,
        char* out,size_t size) {
    char group[512];if(snprintf(group,sizeof(group),"[%s]",section)>=(int)sizeof(group))return -1;
    int in=0,found=0;const char* p=metadata;
    while(p&&*p){
        const char* nl=strchr(p,'\n');size_t len=nl?(size_t)(nl-p):strlen(p);
        if(len&&p[len-1]=='\r')len--;
        if(len&&p[0]=='[')in=len==strlen(group)&&!memcmp(p,group,len);
        else if(in&&len>strlen(key)&&!memcmp(p,key,strlen(key))&&p[strlen(key)]=='='){
            size_t n=len-strlen(key)-1;if(found||n>=size)return -1;
            memcpy(out,p+strlen(key)+1,n);out[n]=0;found=1;
        }
        p=nl?nl+1:NULL;
    }
    return found?0:-1;
}
static int staged_extension_declared(const char* metadata,const char* extension_id,
        const char* ref,const char* base_ref,int partial) {
    char id[256];if(strlen(extension_id)>=sizeof(id))return 0;strcpy(id,extension_id);
    const char* branch=strrchr(ref,'/');const char* base_branch=strrchr(base_ref,'/');
    if(!branch||!base_branch)return 0;
    const char* arch_end=branch;const char* arch=arch_end;
    while(arch>ref&&arch[-1]!='/')arch--;
    const char* base_arch_end=base_branch;const char* base_arch=base_arch_end;
    while(base_arch>base_ref&&base_arch[-1]!='/')base_arch--;
    if(arch_end-arch!=base_arch_end-base_arch||memcmp(arch,base_arch,(size_t)(arch_end-arch)))return 0;
    for(;;){
        char group[512],directory[1024];snprintf(group,sizeof(group),"Extension %s",id);
        if(!staged_metadata_value(metadata,group,"directory",directory,sizeof(directory))){
            if(directory[0]=='/'||!sp_valid(directory))return 0;
            if(strcmp(id,extension_id)){
                char subdirs[16];if(staged_metadata_value(metadata,group,"subdirectories",subdirs,sizeof(subdirs))||strcmp(subdirs,"true"))return 0;
            }
            if(partial){char subset[16];if(staged_metadata_value(metadata,group,"locale-subset",subset,sizeof(subset))||strcmp(subset,"true"))return 0;}
            char versions[4096];
            if(staged_metadata_value(metadata,group,"versions",versions,sizeof(versions))&&
                    staged_metadata_value(metadata,group,"version",versions,sizeof(versions)))
                return !strcmp(branch+1,base_branch+1);
            char* save=NULL;
            for(char* v=strtok_r(versions,";",&save);v;v=strtok_r(NULL,";",&save))if(!strcmp(v,branch+1))return 1;
            return 0;
        }
        char* dot=strrchr(id,'.');if(!dot)return 0;*dot=0;
    }
}
static int staged_read_metadata(int dir,char* out,size_t size) {
    int fd=openat(dir,"metadata",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    struct stat st;int rc=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||(uint64_t)st.st_size>=size;
    size_t used=0;
    while(!rc&&used<(size_t)st.st_size){ssize_t n=read(fd,out+used,(size_t)st.st_size-used);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-1;break;}used+=(size_t)n;}
    if(!rc){out[used]=0;if(memchr(out,0,used))rc=-1;}close(fd);return rc;
}
static int staged_has_binding(const char* repo,const char* commit,const char* ref) {
    char text[8192]={0};const char* args[]={"show","--print-metadata-key=ostree.ref-binding",commit};
    if(run_ostree(repo,args,3,text,sizeof(text)))return 0;
    const char* p=text;while(*p==' '||*p=='\n'||*p=='\t')p++;
    if(!strncmp(p,"@as ",4))p+=4;
    if(*p++!='[')return 0;
    int found=0;
    for(;;){
        while(*p==' '||*p=='\n'||*p=='\t')p++;
        if(*p==']'){p++;break;}
        if(*p++!='\'')return 0;
        char value[512];size_t n=0;
        while(*p&&*p!='\''){
            if(*p=='\\'||n+1>=sizeof(value))return 0;
            value[n++]=*p++;
        }
        if(*p++!='\'')return 0;
        value[n]=0;if(!valid_staged_ref(value))return 0;
        if(!strcmp(value,ref))found=1;
        while(*p==' '||*p=='\n'||*p=='\t')p++;
        if(*p==','){p++;continue;}
        if(*p==']'){p++;break;}
        return 0;
    }
    while(*p==' '||*p=='\n'||*p=='\r'||*p=='\t')p++;
    return !*p&&found;
}
static int staged_source_repo(int installation,const char* path) {
    int repo=openat(installation,"repo",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(repo<0)return -1;
    int objects=openat(repo,"objects",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(objects<0){close(repo);return -1;}
    DIR* shards=fdopendir(dup(objects));int rc=shards?0:-1;struct dirent* shard;
    while(!rc&&(shard=readdir(shards))){
        if(!strcmp(shard->d_name,".")||!strcmp(shard->d_name,".."))continue;
        if(strlen(shard->d_name)!=2||strspn(shard->d_name,"0123456789abcdef")!=2){rc=-1;break;}
        int dir=openat(objects,shard->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(dir<0){rc=-1;break;}
        DIR* files=fdopendir(dup(dir));struct dirent* file;if(!files){close(dir);rc=-1;break;}
        while(!rc&&(file=readdir(files))){
            if(!strcmp(file->d_name,".")||!strcmp(file->d_name,".."))continue;
            struct stat st;const char* dot=strchr(file->d_name,'.');
            if(!dot||dot-file->d_name!=62||strspn(file->d_name,"0123456789abcdef")!=62||
                    fstatat(dir,file->d_name,&st,AT_SYMLINK_NOFOLLOW)||
                    (!S_ISREG(st.st_mode)&&!(S_ISLNK(st.st_mode)&&!strcmp(dot,".file")))||
                    (strcmp(dot,".file")&&strcmp(dot,".commit")&&strcmp(dot,".commitmeta")&&strcmp(dot,".dirtree")&&strcmp(dot,".dirmeta")&&strcmp(dot,".tombstone-commit")))rc=-1;
        }
        closedir(files);close(dir);
    }
    if(shards)closedir(shards);
    close(objects);
    /* Discard every producer repository control entry. In particular lock,
     * tmp, state and refs symlinks must never redirect OSTree's own writes. */
    DIR* controls=rc?NULL:fdopendir(dup(repo));if(!rc&&!controls)rc=-1;
    struct dirent* control;
    while(!rc&&controls&&(control=readdir(controls))){
        if(!strcmp(control->d_name,".")||!strcmp(control->d_name,"..")||!strcmp(control->d_name,"objects"))continue;
        if(remove_tree_at(repo,control->d_name))rc=-1;
    }
    if(controls)closedir(controls);
    const char* dirs[]={"tmp","state","refs","refs/heads","refs/remotes"};
    for(size_t i=0;!rc&&i<sizeof(dirs)/sizeof(dirs[0]);i++)if(mkdirat(repo,dirs[i],0700))rc=-1;
    /* Never consult producer-configured parent repositories or object dirs. */
    int fd=rc?-1:openat(repo,"config",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    static const char config[]="[core]\nrepo_version=1\nmode=bare-user-only\n";
    if(fd<0)rc=-1;else{if(write(fd,config,sizeof(config)-1)!=(ssize_t)sizeof(config)-1||fsync(fd))rc=-1;close(fd);}
    if(!rc)rc=fsync(repo);
    close(repo);(void)path;return rc;
}
static int staged_label_tree(int dir,unsigned depth) {
    if(depth>128)return -1;
#ifndef MATONOS_PUBLISH_HOST_TEST
    static const char deployment[]="u:object_r:matonos_flatpak_deployment_file:s0";
    static const char code[]="u:object_r:matonos_linux_code_file:s0";
    if(fsetxattr(dir,"security.selinux",deployment,sizeof(deployment),0))return -1;
#endif
    DIR* entries=fdopendir(dup(dir));if(!entries)return -1;rewinddir(entries);
    int rc=0;struct dirent* e;
    while(!rc&&(e=readdir(entries))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){rc=-1;break;}
        if(S_ISDIR(st.st_mode)||S_ISREG(st.st_mode)){
            int fd=openat(dir,e->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(fd<0){rc=-1;break;}
            if(S_ISDIR(st.st_mode))rc=staged_label_tree(fd,depth+1);
#ifndef MATONOS_PUBLISH_HOST_TEST
            else rc=fsetxattr(fd,"security.selinux",code,sizeof(code),0);
#endif
            if(!rc)rc=fsync(fd);
            close(fd);
        }else if(S_ISLNK(st.st_mode)){
#ifndef MATONOS_PUBLISH_HOST_TEST
            char path[4096];
            if(snprintf(path,sizeof(path),"/proc/self/fd/%d/%s",dir,e->d_name)>=(int)sizeof(path)||
                    lsetxattr(path,"security.selinux",deployment,sizeof(deployment),0))rc=-1;
#endif
        }else rc=-1;
    }
    closedir(entries);return rc;
}
/* Compare an existing shared runtime without changing any pointer or trusting
 * its generated deploy record. The existing file inodes are sealed/measured
 * again and signed payload bytes compare against the fresh trusted checkout. */
static int staged_existing_runtime(const char* system,const struct FlatpakStagedDeployment* deployment,
        const char* expected_path,const char* remote,char* error,size_t size) {
    const char* ref=deployment->ref;const char* commit=deployment->commit;
    char basename[256];snprintf(basename,sizeof(basename),"%s",commit);size_t name_size=strlen(basename);
    const char* subpaths[MATON_STAGED_SUBPATHS_MAX];
    for(size_t i=0;i<deployment->subpath_count;i++){
        subpaths[i]=deployment->subpaths[i];if(name_size+1>=sizeof(basename))return -1;basename[name_size++]='-';
        for(const char* c=subpaths[i];*c;c++)if(*c!='/'){if(name_size+1>=sizeof(basename))return -1;basename[name_size++]=*c;}
        basename[name_size]=0;
    }
    int root=open(system,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)return -1;
    char relative[1024];if(snprintf(relative,sizeof(relative),"%s/%s",ref,basename)>=(int)sizeof(relative)){close(root);return -1;}
    int actual=staged_open_path(root,relative);close(root);
    if(actual<0)return errno==ENOENT?1:-1;
    int expected=open(expected_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat a,b;int rc=expected<0||fstat(actual,&a)||fstat(expected,&b)||a.st_uid!=AID_SYSTEM||(a.st_mode&0022);
    if(!rc)rc=sanitize_checkout(expected,b.st_dev,0,0,0);
    DIR* entries=rc?NULL:fdopendir(dup(actual));if(!rc&&!entries)rc=-1;
    struct dirent* e;
    while(!rc&&entries&&(e=readdir(entries))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||!strcmp(e->d_name,"deploy"))continue;
        struct stat x,y;
        if(fstatat(actual,e->d_name,&x,AT_SYMLINK_NOFOLLOW)||fstatat(expected,e->d_name,&y,AT_SYMLINK_NOFOLLOW)||
                x.st_uid!=AID_SYSTEM||(x.st_mode&S_IFMT)!=(y.st_mode&S_IFMT)||(x.st_mode&0777)!=(y.st_mode&0777)){rc=-1;break;}
        if(S_ISDIR(x.st_mode)){
            int af=openat(actual,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),bf=openat(expected,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(af<0||bf<0||maton_staged_compare_inner(af,bf,a.st_dev,b.st_dev,1,0,0,error,size))rc=-1;
            if(af>=0)close(af);
            if(bf>=0)close(bf);
        }else if(S_ISREG(x.st_mode)){
            int af=maton_staged_open_existing(actual,e->d_name,a.st_dev,AID_SYSTEM,error,size),bf=openat(expected,e->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
            if(af<0||bf<0||maton_staged_equal_bytes(af,bf))rc=-1;
            if(af>=0)close(af);
            if(bf>=0)close(bf);
        }else rc=-1;
    }
    if(entries)closedir(entries);
    entries=rc?NULL:fdopendir(dup(expected));if(!rc&&!entries)rc=-1;
    if(entries)rewinddir(entries);
    while(!rc&&entries&&(e=readdir(entries))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat st;if(fstatat(actual,e->d_name,&st,AT_SYMLINK_NOFOLLOW))rc=-1;
    }
    if(entries)closedir(entries);
    /* Rebuild expected deploy bytes and compare exactly, including the
     * origin, commit, subpaths and authenticated runtime/extension relation. */
    if(!rc){
        char runtime[513],extension[513];uint64_t bytes=0;
        struct sd_bytes* encoded=calloc(1,sizeof(*encoded));
        if(!encoded||sd_metadata(expected,runtime,sizeof(runtime),extension,sizeof(extension))||sd_tree_size(expected,0,&bytes)||
                sd_deploy_bytes(encoded,remote,commit,subpaths,deployment->subpath_count,runtime,extension,bytes))rc=-1;
        int deploy=rc?-1:openat(actual,"deploy",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        struct stat ds;if(deploy<0||fstat(deploy,&ds)||!S_ISREG(ds.st_mode)||ds.st_uid!=AID_SYSTEM||(ds.st_mode&0777)!=0644||ds.st_nlink!=1||ds.st_size!=(off_t)(encoded?encoded->size:0))rc=-1;
        if(!rc){unsigned char buf[65536];size_t used=0;while(used<encoded->size){ssize_t n=read(deploy,buf,sizeof(buf));if(n<0&&errno==EINTR)continue;if(n<=0||(size_t)n>encoded->size-used||memcmp(buf,encoded->data+used,(size_t)n)){rc=-1;break;}used+=(size_t)n;}}
        if(deploy>=0)close(deploy);
        free(encoded);
    }
    if(expected>=0)close(expected);
    close(actual);return rc;
}
static int staged_target_repo(const char* target,const char* trusted_repo,const char* remote,
        const char* ref,const char* commit,int create) {
    char repo[PATH_MAX],remote_arg[160],create_arg[768];
    if(snprintf(repo,sizeof(repo),"%s/repo",target)>=(int)sizeof(repo)||
            snprintf(remote_arg,sizeof(remote_arg),"--remote=%s",remote)>=(int)sizeof(remote_arg)||
            snprintf(create_arg,sizeof(create_arg),"--create=%s:%s",remote,ref)>=(int)sizeof(create_arg))return -1;
    if(create){
        struct stat st;if(lstat(repo,&st)){
            if(errno!=ENOENT)return -1;
            const char* init[]={"init","--mode=bare-user-only"};
            if(run_ostree(repo,init,2,NULL,0)||seed_remote_from(trusted_repo,target,remote))return -1;
        }else if(!S_ISDIR(st.st_mode)||st.st_uid!=AID_SYSTEM||(st.st_mode&0022))return -1;
    }
    const char* pull[]={"pull-local","--untrusted","--gpg-verify",remote_arg,trusted_repo,commit};
    const char* refs[]={"refs","--force",create_arg,commit};
    return run_ostree(repo,pull,6,NULL,0)||run_ostree(repo,refs,4,NULL,0)||
        nftw(repo,repo_world_readable,16,FTW_PHYS)||seal_object_files(repo);
}
static int staged_activate_existing(const char* system,const struct FlatpakStagedDeployment* d) {
    int root=open(system,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)return -1;
    int branch=staged_open_path(root,d->ref);close(root);if(branch<0)return -1;
    char basename[256];snprintf(basename,sizeof(basename),"%s",d->commit);size_t used=strlen(basename);
    for(size_t i=0;i<d->subpath_count;i++){
        if(used+1>=sizeof(basename)){close(branch);return -1;}basename[used++]='-';
        for(const char* c=d->subpaths[i];*c;c++)if(*c!='/'){
            if(used+1>=sizeof(basename)){close(branch);return -1;}basename[used++]=*c;
        }
        basename[used]=0;
    }
    int rc=sd_link(branch,"active",basename);close(branch);return rc;
}
int flatpak_verify_publish_staged_tracked(const struct FlatpakStagedManifest* m,
        const char* staging,const char* system,const char* pending,char* error,unsigned long size,
        int (*record)(const char*,void*),int (*complete)(const char*const*,size_t,void*),void* context) {
    if(error&&size)error[0]=0;
    int parent=-1,op=-1,source=-1,captured=-1,installation=-1,rc=-1;
    char parent_path[PATH_MAX],trust[PATH_MAX],repo[PATH_MAX],capture[PATH_MAX],capture_name[96]={0},trust_name[96]={0};
    char source_repo[PATH_MAX];
    struct maton_staged_inodes* zones=NULL;
    struct staged_verified {char path[PATH_MAX];char expected[PATH_MAX];char metadata[65536];int user,existing;}* verified=NULL;
    if(!m||m->version!=1||!memchr(m->operation_id,0,sizeof(m->operation_id))||
            !memchr(m->remote,0,sizeof(m->remote))||!memchr(m->app_ref,0,sizeof(m->app_ref))||
            !memchr(m->app_commit,0,sizeof(m->app_commit))||!valid_operation_id(m->operation_id)||!valid_remote(m->remote)||
            !valid_staged_ref(m->app_ref)||strncmp(m->app_ref,"app/",4)||!valid_commit(m->app_commit)||
            !m->deployment_count||m->deployment_count>MATON_STAGED_DEPLOYMENTS_MAX||
            !staging||!system||!pending){fail(error,size,"invalid staged manifest");return -1;}
#ifndef MATONOS_PUBLISH_HOST_TEST
    if(strcmp(system,MATON_RUNTIME_DIR)||!pending_deployment_path(pending)){fail(error,size,"invalid staged publication roots");return -1;}
#endif
    const char* slash=strrchr(staging,'/');
    if(!slash||strcmp(slash+1,m->operation_id)||(size_t)(slash-staging)>=sizeof(parent_path))return -1;
    memcpy(parent_path,staging,(size_t)(slash-staging));parent_path[slash-staging]=0;
    const char* uid_text=strrchr(parent_path,'/');char* end=NULL;
    long uid=uid_text?strtol(uid_text+1,&end,10):-1;
    char canonical[PATH_MAX];
    if(!uid_text||!end||*end||uid>INT_MAX||!staged_app_uid((int)uid)||
            snprintf(canonical,sizeof(canonical),MATON_STAGING_ROOT "/%ld/%s",uid,m->operation_id)>=(int)sizeof(canonical)||strcmp(canonical,staging)){
        fail(error,size,"staging path does not match registered operation");return -1;
    }
    parent=staged_parent((int)uid);if(parent<0)goto done;
    if(staged_name(trust_name,sizeof(trust_name),"trust",m->operation_id)||staged_name(capture_name,sizeof(capture_name),"capture",m->operation_id)||
            snprintf(trust,sizeof(trust),"%s/%s",parent_path,trust_name)>=(int)sizeof(trust)||
            snprintf(repo,sizeof(repo),"%s/repo",trust)>=(int)sizeof(repo)||
            snprintf(capture,sizeof(capture),"%s/%s",parent_path,capture_name)>=(int)sizeof(capture))goto done;
    int trustfd=openat(parent,trust_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat trusted;
    if(trustfd<0)goto done;
    int bad=fstat(trustfd,&trusted)||trusted.st_uid!=AID_SYSTEM||(trusted.st_mode&0077);
    close(trustfd);if(bad||!remote_verification_enabled(repo,m->remote))goto done;
    struct stat operation_st;
    if(fstatat(parent,m->operation_id,&operation_st,AT_SYMLINK_NOFOLLOW)||!S_ISDIR(operation_st.st_mode)||
            fchownat(parent,m->operation_id,AID_SYSTEM,AID_SYSTEM,AT_SYMLINK_NOFOLLOW)||
            fchmodat(parent,m->operation_id,0700,0)||operation_st.st_dev!=trusted.st_dev)goto done;
    op=openat(parent,m->operation_id,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(op<0)goto done;
    struct stat source_st;
    if(fstatat(op,"installation",&source_st,AT_SYMLINK_NOFOLLOW)||!S_ISDIR(source_st.st_mode)||
            fchownat(op,"installation",AID_SYSTEM,AID_SYSTEM,AT_SYMLINK_NOFOLLOW)||fchmodat(op,"installation",0700,0))goto done;
    source=openat(op,"installation",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(source<0||mkdirat(parent,capture_name,0700))goto done;
    captured=openat(parent,capture_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(captured<0||mkdirat(captured,"installation",0700))goto done;
    installation=openat(captured,"installation",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;if(installation<0||fstat(installation,&st)||
            maton_staged_capture(source,installation,st.st_dev,AID_SYSTEM,0,error,size))goto done;
    if(snprintf(source_repo,sizeof(source_repo),"%s/installation/repo",capture)>=(int)sizeof(source_repo)||staged_source_repo(installation,source_repo))goto done;
    verified=calloc(m->deployment_count,sizeof(*verified));zones=calloc(1,sizeof(*zones));
    if(!verified||!zones)goto done;
    size_t app_index=SIZE_MAX;
    for(size_t i=0;i<m->deployment_count;i++){
        const struct FlatpakStagedDeployment* d=&m->deployments[i];
        if(!memchr(d->ref,0,sizeof(d->ref))||!memchr(d->commit,0,sizeof(d->commit))||!valid_staged_ref(d->ref)||!valid_commit(d->commit)||d->subpath_count>MATON_STAGED_SUBPATHS_MAX||
                (strncmp(d->ref,"runtime/",8)&&strcmp(d->ref,m->app_ref)))goto done;
        for(size_t j=0;j<i;j++)if(!strcmp(d->ref,m->deployments[j].ref))goto done;
        if(!strcmp(d->ref,m->app_ref)){if(strcmp(d->commit,m->app_commit))goto done;app_index=i;}
        char remote_arg[160];snprintf(remote_arg,sizeof(remote_arg),"--remote=%s",m->remote);
        const char* pull[]={"pull-local","--untrusted","--gpg-verify",remote_arg,source_repo,d->commit};
        if(run_ostree(repo,pull,6,NULL,0)||!staged_has_binding(repo,d->commit,d->ref))goto done;
        char expected[PATH_MAX];
        if(snprintf(expected,sizeof(expected),"%s/check-%zu",trust,i)>=(int)sizeof(expected))goto done;
        snprintf(verified[i].expected,sizeof(verified[i].expected),"%s",expected);
        const char* checkout[]={"checkout","--user-mode",d->commit,expected};
        if(run_ostree(repo,checkout,4,NULL,0))goto done;
        char basename[256];snprintf(basename,sizeof(basename),"%s",d->commit);size_t used=strlen(basename);
        for(size_t j=0;j<d->subpath_count;j++){
            if(!memchr(d->subpaths[j],0,sizeof(d->subpaths[j]))||!sp_valid(d->subpaths[j])||used+1>=sizeof(basename))goto done;
            basename[used++]='-';
            for(const char* c=d->subpaths[j];*c;c++)if(*c!='/'){
                if(used+1>=sizeof(basename))goto done;
                basename[used++]=*c;
            }
            basename[used]=0;
        }
        char relative[1024];if(snprintf(relative,sizeof(relative),"%s/%s",d->ref,basename)>=(int)sizeof(relative)||
                snprintf(verified[i].path,sizeof(verified[i].path),"%s/installation/%s",capture,relative)>=(int)sizeof(verified[i].path))goto done;
        int actual=staged_open_path(installation,relative),expectedfd=open(expected,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(actual<0||expectedfd<0){if(actual>=0)close(actual);if(expectedfd>=0)close(expectedfd);goto done;}
        /* Flatpak deploy bookkeeping is not signed. The publisher rebuilds
         * every field from the authenticated transaction instead. */
        if(unlinkat(actual,"deploy",0)&&errno!=ENOENT){close(actual);close(expectedfd);goto done;}
        struct stat es;bad=fstat(expectedfd,&es)||staged_read_metadata(expectedfd,verified[i].metadata,sizeof(verified[i].metadata));
        const char* selected[MATON_STAGED_SUBPATHS_MAX];
        for(size_t j=0;j<d->subpath_count;j++)selected[j]=d->subpaths[j];
        if(!bad&&d->subpath_count)bad=maton_staged_project(expectedfd,selected,d->subpath_count,error,size);
        int extra=0;
        if(!bad&&(strstr(verified[i].metadata,"[Extra Data]")||strstr(verified[i].metadata,"[Extra Data "))){
            char sources[65536]={0};const char* extra_args[]={"show","--print-metadata-key=xa.extra-data-sources",d->commit};
            if(run_ostree(repo,extra_args,3,sources,sizeof(sources))||sources[0]!='['||sources[1]!= '('){bad=-1;}
            else extra=1;
        }
        if(!bad)bad=maton_staged_compare(actual,expectedfd,st.st_dev,es.st_dev,0,extra?1:0,error,size);
        close(actual);close(expectedfd);if(bad)goto done;
    }
    if(app_index==SIZE_MAX)goto done;
    char runtime[512];
    if(metadata_runtime_ref(verified[app_index].metadata,runtime,sizeof(runtime))||!valid_staged_ref(runtime))goto done;
    char declared_name[256],authorized_name[256];
    if(app_id_from_ref(m->app_ref,authorized_name,sizeof(authorized_name))||
            staged_metadata_value(verified[app_index].metadata,"Application","name",declared_name,sizeof(declared_name))||
            strcmp(declared_name,authorized_name))goto done;
    size_t runtime_index=SIZE_MAX;
    for(size_t i=0;i<m->deployment_count;i++)if(!strcmp(m->deployments[i].ref,runtime))runtime_index=i;
    if(runtime_index==SIZE_MAX||m->deployments[app_index].subpath_count||m->deployments[runtime_index].subpath_count)goto done;
    char app_id[256],runtime_id[256];
    if(app_id_from_ref(m->app_ref,app_id,sizeof(app_id)))goto done;
    const char* rstart=runtime+8;const char* rend=strchr(rstart,'/');
    if(!rend||(size_t)(rend-rstart)>=sizeof(runtime_id))goto done;
    memcpy(runtime_id,rstart,(size_t)(rend-rstart));runtime_id[rend-rstart]=0;
    if(staged_metadata_value(verified[runtime_index].metadata,"Runtime","name",declared_name,sizeof(declared_name))||strcmp(declared_name,runtime_id))goto done;
    for(size_t i=0;i<m->deployment_count;i++){
        const struct FlatpakStagedDeployment* d=&m->deployments[i];
        verified[i].user=i==app_index;
        if(i!=app_index){
            const char* id=d->ref+8;const char* endid=strchr(id,'/');
            char name[256],declared[256];
            if(!endid||(size_t)(endid-id)>=sizeof(name))goto done;
            memcpy(name,id,(size_t)(endid-id));name[endid-id]=0;
            if(staged_metadata_value(verified[i].metadata,"Runtime","name",declared,sizeof(declared))||strcmp(name,declared))goto done;
        }
        if(i!=app_index&&i!=runtime_index){
            char base[512],extension_id[256];
            if(staged_metadata_value(verified[i].metadata,"ExtensionOf","ref",base,sizeof(base)))goto done;
            int app_extension=!strcmp(base,m->app_ref);
            if(!app_extension&&strcmp(base,runtime))goto done;
            const char* id=d->ref+8;const char* endid=strchr(id,'/');
            if(!endid||(size_t)(endid-id)>=sizeof(extension_id))goto done;
            memcpy(extension_id,id,(size_t)(endid-id));extension_id[endid-id]=0;
            const char* base_metadata=verified[app_extension?app_index:runtime_index].metadata;
            if(!staged_extension_declared(base_metadata,extension_id,d->ref,base,d->subpath_count!=0))goto done;
            verified[i].user=app_extension;
        }
        int actual=open(verified[i].path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(actual<0)goto done;
        bad=staged_zone_tree(actual,st.st_dev,verified[i].user?1:2,zones,0);
        if(!bad)bad=sanitize_checkout(actual,st.st_dev,0,verified[i].user,0);
        if(!bad)bad=staged_label_tree(actual,0);
        close(actual);if(bad){fail(error,size,"cross-zone hardlink or unsafe signed symlink");goto done;}
        if(!verified[i].user){
            int existing=staged_existing_runtime(system,d,verified[i].expected,m->remote,error,size);
            if(existing<0){fail(error,size,"existing runtime failed sealed signed verification");goto done;}
            verified[i].existing=existing==0;
        }
    }
    /* Canonical extra modes and exact signed modes now prevent new producer
     * hardlinks. Catch aliases added through retained O_PATH during capture. */
    int trusted_tree=open(trust,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(trusted_tree<0)goto done;
    int recount=staged_recount(installation,trusted_tree);close(trusted_tree);
    if(recount){fail(error,size,"staged hardlink alias changed after capture");goto done;}
    /* Object checksum verification complements commit signature verification;
     * an existing staging seal alone never authenticates an OSTree object. */
    const char* fsck[]={"fsck","--all"};if(run_ostree(repo,fsck,2,NULL,0))goto done;
    if(record)for(size_t i=0;i<m->deployment_count;i++)if(!verified[i].user&&record(m->deployments[i].ref,context))goto done;
    /* Nothing shared changes until every deployment has passed all checks. */
    for(size_t i=0;i<m->deployment_count;i++){
        const struct FlatpakStagedDeployment* d=&m->deployments[i];
        if(staged_target_repo(verified[i].user?pending:system,repo,m->remote,d->ref,d->commit,verified[i].user)){
            fail(error,size,"cannot import authenticated installation repository");goto done;
        }
    }
    for(int phase=0;phase<2;phase++)for(size_t i=0;i<m->deployment_count;i++){
        if(verified[i].user!=phase)continue;
        if(verified[i].existing){if(staged_activate_existing(system,&m->deployments[i]))goto done;continue;}
        const struct FlatpakStagedDeployment* d=&m->deployments[i];const char* subpaths[MATON_STAGED_SUBPATHS_MAX];
        for(size_t j=0;j<d->subpath_count;j++)subpaths[j]=d->subpaths[j];
        if(maton_staged_deploy(verified[i].path,verified[i].user?pending:system,d->ref,d->commit,m->remote,subpaths,d->subpath_count,error,size))goto done;
    }
    if(complete){const char* refs[MATON_STAGED_DEPLOYMENTS_MAX];size_t n=0;
        for(size_t i=0;i<m->deployment_count;i++)if(!verified[i].user)refs[n++]=m->deployments[i].ref;
        if(complete(refs,n,context))goto done;
    }
    rc=0;
done:
    if(rc&&error&&size&&!error[0])fail(error,size,"staged transaction verification failed");
    if(installation>=0)close(installation);
    if(captured>=0)close(captured);
    if(source>=0)close(source);
    if(op>=0)close(op);
    if(parent>=0){if(capture_name[0])(void)remove_tree_at(parent,capture_name);close(parent);}
    maton_staged_inodes_free(zones);free(verified);return rc;
}

/* Remove both the producer operation and private siblings. Ownership has no
 * effect on a retained writable mapping; only verity above is the trust gate. */
static int remove_registered_staging(int uid,const char* operation) {
    if(!staged_app_uid(uid)||!valid_operation_id(operation))return -1;
    int parent=staged_parent(uid);if(parent<0)return errno==ENOENT?0:-1;
    int rc=0;
    /* Producer-owned trees may be mode0700. Reap their namespace using the
     * producer UID, then collect reclaimed linuxd-owned trees ourselves. */
#ifndef MATONOS_PUBLISH_HOST_TEST
    pid_t child=fork();
    if(child<0){close(parent);return -1;}
    if(!child){
        if(setgroups(0,NULL)||setresgid(uid,uid,uid)||setresuid(uid,uid,uid))_exit(1);
        int fd=openat(parent,operation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0)_exit(errno==ENOENT||errno==EACCES?0:1);
        DIR* dir=fdopendir(dup(fd));if(!dir)_exit(1);struct dirent* e;int failed=0;
        while((e=readdir(dir))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
            if(remove_tree_at(fd,e->d_name)&&errno!=EACCES&&errno!=ENOENT)failed=1;
        }
        closedir(dir);close(fd);_exit(failed);
    }
    int status=0;pid_t waited;do{waited=waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
    if(waited<0||!WIFEXITED(status)||WEXITSTATUS(status))rc=-1;
#endif
    int fd=openat(parent,operation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0){
        if(fchown(fd,AID_SYSTEM,AID_SYSTEM)||fchmod(fd,0700))rc=-1;
        close(fd);
    }else if(errno!=ENOENT)rc=-1;
    if(!rc&&remove_tree_at(parent,operation)&&errno!=ENOENT)rc=-1;
    const char* prefixes[]={"trust","capture"};
    for(size_t i=0;i<2;i++){char name[96];
        if(staged_name(name,sizeof(name),prefixes[i],operation)||(remove_tree_at(parent,name)&&errno!=ENOENT))rc=-1;
    }
    if(!rc)rc=fsync(parent);
    close(parent);return rc;
}
static int cleanup_registered_staging(void) {
    int root=open(MATON_STAGING_ROOT,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0)return errno==ENOENT?0:-1;
    DIR* dirs=fdopendir(dup(root));if(!dirs){close(root);return -1;}
    struct dirent* e;int rc=0;
    while(!rc&&(e=readdir(dirs))){
        if(e->d_name[0]=='.')continue;
        char* end=NULL;long uid=strtol(e->d_name,&end,10);
        if(!end||*end||uid>INT_MAX||!staged_app_uid((int)uid))continue;
        int parent=openat(root,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(parent<0){rc=-1;break;}
        DIR* ops=fdopendir(dup(parent));if(!ops){close(parent);rc=-1;break;}
        struct dirent* op;
        while(!rc&&(op=readdir(ops))){
            if(!strcmp(op->d_name,".")||!strcmp(op->d_name,".."))continue;
            const char* name=op->d_name;
            if(!strncmp(name,".trust-",7))name+=7;
            else if(!strncmp(name,".capture-",9))name+=9;
            if(!valid_operation_id(name)){rc=-1;break;}
            rc=remove_registered_staging((int)uid,name);
        }
        closedir(ops);close(parent);
    }
    closedir(dirs);close(root);return rc;
}
