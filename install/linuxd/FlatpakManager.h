#ifndef MATONOS_FLATPAK_MANAGER_H
#define MATONOS_FLATPAK_MANAGER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlatpakResult {
    int ok;
    int exit_code;
    int output_truncated;
    char* output;
    char* error;
} FlatpakResult;

void flatpak_manager_init(void);
void flatpak_manager_reconcile_runtime_refs(void);
int flatpak_manager_valid_ref(const char* value);
int flatpak_manager_valid_app_id(const char* value);
void flatpak_manager_call(const char* command, const char* ref,
        int delete_data, const char* operation_id, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        int app_uid, FlatpakResult* result);

struct FlatpakStagedManifest;
/* Private bridge transport; compositor UID is resolved by the bridge from PM. */
void flatpak_manager_staged(const char* command, const char* operation,
        const char* ref, const char* remote, int compositor_uid,
        const struct FlatpakStagedManifest* manifest, FlatpakResult* result);

int flatpak_manager_installed_for_uid(const char* ref, int uid);

/* Consume a successful binding exactly once before install_complete. */
int flatpak_manager_complete_handoff(const char* operation,const char* ref,const char* remote);
void flatpak_manager_handoff(const char* command, const char* ref, const char* remote,
        const char* operation, int uid, FlatpakResult* result);

void flatpak_manager_result_clear(FlatpakResult* result);

#ifdef __cplusplus
}
#endif

#endif
