#include "../FlatpakPublish.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../RuntimeRefs.h"

static int record(const char* ref, void* context) {
    RuntimeRefs* db = context;
    if (db && (runtime_refs_add(db, ref, 10120) || runtime_refs_save(db))) return -1;
    const char* path = getenv("PUBLISH_REF_LOG");
    if (!path) return 0;
    FILE* f = fopen(path, "a"); if (!f) return -1;
    int rc = fprintf(f, "%s\n", ref) < 0;
    if (fclose(f)) rc = 1;
    return rc || getenv("PUBLISH_RECORD_FAIL") != NULL;
}
static int prune(const char* ref, int* extension, void* context) {
    (void)context;
    if (extension) { *extension = 0; return 0; }
    pid_t pid = fork(); if (pid < 0) return -1;
    if (!pid) { execl(getenv("MATONOS_FLATPAK_CLI"), "flatpak", "uninstall", "--system", "--noninteractive", ref, (char*)NULL); _exit(127); }
    int status; if (waitpid(pid, &status, 0) < 0) return -1;
    return !WIFEXITED(status) || WEXITSTATUS(status);
}
static int complete(const char* const* refs, size_t count, void* context) {
    RuntimeRefs* db = context;
    return db ? runtime_refs_replace(db, 10120, refs, count) || runtime_refs_prune(db, prune, NULL) : 0;
}
int main(int argc, char** argv) {
    if (argc != 9) {
        fprintf(stderr, "usage: %s APP_REF APP_COMMIT RUNTIME_REF RUNTIME_COMMIT REMOTE R S U\n", argv[0]);
        return 2;
    }
    char error[512] = {0};
    RuntimeRefs* db = getenv("PUBLISH_DB_DIR") ? runtime_refs_open(getenv("PUBLISH_DB_DIR"), 0) : NULL;
    if (getenv("PUBLISH_DB_DIR") && !db) return 1;
    int rc = flatpak_publish_tracked(argv[1], argv[2], argv[3], argv[4], argv[5],
            argv[6], argv[7], argv[8], error, sizeof(error), record, complete, db);
    runtime_refs_close(db);
    if (rc) fprintf(stderr, "%s\n", error[0] ? error : "publish failed");
    return rc ? 1 : 0;
}
