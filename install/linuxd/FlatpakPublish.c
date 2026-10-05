#define _GNU_SOURCE
#include "FlatpakPublish.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#ifndef MATONOS_FLATPAK_CLI
#define MATONOS_FLATPAK_CLI "/apex/com.matonos.flatpak/bin/flatpak-env-wrapper"
#endif
#ifndef MATONOS_OSTREE_CLI
#define MATONOS_OSTREE_CLI "/apex/com.matonos.flatpak/bin/ostree"
#endif
#ifndef MATONOS_FLATPAK_STORE_HELPER
#define MATONOS_FLATPAK_STORE_HELPER "/apex/com.matonos.flatpak/bin/matonos-flatpak-store"
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
    if (!value || !*value || strlen(value) > 128) return 0;
    for (const unsigned char* p = (const unsigned char*)value; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return 0;
    return 1;
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

static int invoke_path(const char* path, const char* system_dir, const char* user_dir,
        char* const args[], char* output, size_t output_size) {
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
    char chunk[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
        if (n > 0) {
            if (output && output_size && used < output_size - 1) {
                size_t take = (size_t)n;
                if (take > output_size - 1 - used) take = output_size - 1 - used;
                memcpy(output + used, chunk, take);
                used += take;
                output[used] = '\0';
            }
        } else if (n == 0) break;
        else if (errno != EINTR) break;
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static int invoke(const char* applet, const char* system_dir, const char* user_dir,
        char* const args[], char* output, size_t output_size) {
    return invoke_path(tool_path(applet), system_dir, user_dir, args, output, output_size);
}

static int run_flatpak(const char* system_dir, const char* user_dir,
        const char* const input[], size_t count) {
    char** args = calloc(count + 2, sizeof(char*));
    if (!args) return -1;
    args[0] = (char*)tool_path("flatpak");
    for (size_t i = 0; i < count; ++i) args[i + 1] = (char*)input[i];
    char output[4096] = {0};
    int rc = invoke("flatpak", system_dir, user_dir, args, output, sizeof(output));
    if (rc && output[0]) {
        fprintf(stderr, "flatpak command failed (%d):", rc);
        for (size_t i = 1; i <= count; ++i) fprintf(stderr, " %s", args[i]);
        fprintf(stderr, "\n%s\n", output);
    }
    free(args);
    return rc;
}

static int run_ostree(const char* repo, const char* const input[], size_t count,
        char* output, size_t output_size) {
    char repo_arg[PATH_MAX + 16];
    if (snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", repo) >= (int)sizeof(repo_arg)) return -1;
    char** args = calloc(count + 3, sizeof(char*));
    if (!args) return -1;
    args[0] = (char*)tool_path("ostree");
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

static int stage_ref(const char* staging, const char* remote, const char* install_ref) {
#ifdef MATONOS_PUBLISH_HOST_TEST
    const char* args[] = {"install", "--system", "--no-deploy", "--noninteractive",
        "--assumeyes", remote, install_ref};
    return run_flatpak(staging, NULL, args, sizeof(args) / sizeof(args[0]));
#else
    char* args[] = {(char*)MATONOS_FLATPAK_STORE_HELPER, "stage", (char*)staging,
        (char*)remote, (char*)install_ref, NULL};
    char output[4096] = {0};
    int rc = invoke_path(MATONOS_FLATPAK_STORE_HELPER, NULL, NULL, args, output, sizeof(output));
    if (rc && output[0]) fprintf(stderr, "installer staging failed (%d): %s\n", rc, output);
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

static int create_local_ref(const char* repo, const char* remote, const char* ref) {
    char option[512], rev[1024];
    if (snprintf(option, sizeof(option), "--create=%s", ref) >= (int)sizeof(option) ||
            snprintf(rev, sizeof(rev), "%s:%s", remote, ref) >= (int)sizeof(rev)) return -1;
    const char* args[] = {"refs", "--force", option, rev};
    return run_ostree(repo, args, 4, NULL, 0);
}

/* OSTree object payloads are immutable after verification. Keep directories
 * writable for later refs/objects while sealing every object file. */
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
                    lstat(file, &st) || !S_ISREG(st.st_mode) || chmod(file, 0444)) { rc = -1; break; }
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

int flatpak_publish(const char* app_ref, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        const char* staging_dir, const char* system_dir, const char* user_dir,
        char* error, unsigned long error_size) {
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
    if (snprintf(staging_repo, sizeof(staging_repo), "%s/repo", staging_dir) >= (int)sizeof(staging_repo)) {
        fail(error, error_size, "staging path too long"); return -1;
    }
    char system_repo[PATH_MAX], user_repo[PATH_MAX];
    if (snprintf(system_repo, sizeof(system_repo), "%s/repo", system_dir) >= (int)sizeof(system_repo) ||
            snprintf(user_repo, sizeof(user_repo), "%s/repo", user_dir) >= (int)sizeof(user_repo)) {
        fail(error, error_size, "installation path too long"); return -1;
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
    int skip_stage = 0;
#ifdef MATONOS_PUBLISH_HOST_TEST
    skip_stage = getenv("MATONOS_PUBLISH_SKIP_STAGE") &&
            !strcmp(getenv("MATONOS_PUBLISH_SKIP_STAGE"), "1");
#endif
    if (!skip_stage) {
        if (stage_ref(staging_dir, remote, runtime_install_ref) || stage_ref(staging_dir, remote, app_install_ref)) {
            fail(error, error_size, "signature-verified staging pull failed"); return -1;
        }
    }
    if (compare_remote_commit(staging_repo, remote, runtime_ref, runtime_commit) ||
            compare_remote_commit(staging_repo, remote, app_ref, app_commit)) {
        fail(error, error_size, "staged commit does not match the signed stub pin"); return -1;
    }
    /* Flatpak's no-deploy cache stores refs under refs/remotes/<name>. Copy
     * the exact pinned revisions to local heads so pull-local can address
     * the source refs without any network lookup. */
    if (create_local_ref(staging_repo, remote, runtime_ref) ||
            create_local_ref(staging_repo, remote, app_ref)) {
        fail(error, error_size, "cannot prepare local pinned refs"); return -1;
    }
    if (transfer_and_verify(staging_dir, user_dir, remote, app_ref, app_commit)) {
        fail(error, error_size, "app signature verification or publication failed"); return -1;
    }
    if (transfer_and_verify(staging_dir, system_dir, remote, runtime_ref, runtime_commit)) {
        fail(error, error_size, "runtime signature verification or publication failed"); return -1;
    }
    if (seal_object_files(system_repo) || seal_object_files(user_repo)) {
        fail(error, error_size, "cannot relabel and seal verified OSTree objects"); return -1;
    }
    if (deploy_ref(system_dir, user_dir, remote, runtime_install_ref, 0) ||
            deploy_ref(system_dir, user_dir, remote, app_install_ref, 1)) {
        fail(error, error_size, "Flatpak no-pull deployment failed"); return -1;
    }
    if (error && error_size) error[0] = '\0';
    return 0;
}
