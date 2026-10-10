#include "../RuntimeRefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int live(int uid, void* p) { (void)p; return uid != 10120; }
static int prune(const char* ref, int* extension, void* p) {
    (void)p;
    if (extension) { *extension = strstr(ref, ".Locale/") != NULL; return 0; }
    pid_t pid = fork(); if (pid < 0) return -1;
    if (!pid) { execl(getenv("FAKE_FLATPAK"), "flatpak", "uninstall", "--system", "--noninteractive", ref, (char*)NULL); _exit(127); }
    int status; if (waitpid(pid, &status, 0) < 0) return -1;
    return !WIFEXITED(status) || WEXITSTATUS(status);
}
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    RuntimeRefs* db = runtime_refs_open(argv[1], 0);
    if (!db) return 1;
    int rc = 0;
    if (!strcmp(argv[2], "add") && argc == 5) rc = runtime_refs_add(db, argv[3], atoi(argv[4])) || runtime_refs_save(db);
    else if (!strcmp(argv[2], "remove") && argc == 4) rc = runtime_refs_remove(db, atoi(argv[3])) || runtime_refs_prune(db, prune, NULL);
    else if (!strcmp(argv[2], "reconcile")) rc = runtime_refs_reconcile(db, live, NULL) || runtime_refs_prune(db, prune, NULL);
    else if (!strcmp(argv[2], "replace") && argc == 5) {
        const char* refs[] = {argv[3]};
        rc = runtime_refs_replace(db, atoi(argv[4]), refs, 1) || runtime_refs_prune(db, prune, NULL);
    }
    else rc = 1;
    runtime_refs_close(db); return rc ? 1 : 0;
}
