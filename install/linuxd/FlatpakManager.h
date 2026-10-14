#ifndef MATONOS_FLATPAK_MANAGER_H
#define MATONOS_FLATPAK_MANAGER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlatpakResult {
    int ok;
    int accepted;
    int exit_code;
    int output_truncated;
    int has_pid;
    long pid;
    char* output;
    char* error;
    int has_unused_cleanup;
    int unused_cleanup_ok;
    int unused_cleanup_exit_code;
    char* operation_id;
} FlatpakResult;

typedef void (*FlatpakProgressCallback)(const char* line, void* context);
typedef void (*FlatpakCompleteCallback)(const FlatpakResult* result, void* context);

void flatpak_manager_init(void);
void flatpak_manager_reconcile_runtime_refs(void);
void flatpak_manager_set_callbacks(FlatpakProgressCallback progress,
        FlatpakCompleteCallback complete, void* context);
int flatpak_manager_valid_ref(const char* value);
int flatpak_manager_valid_app_id(const char* value);
void flatpak_manager_call(const char* command, const char* ref,
        int delete_data, const char* operation_id, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        int app_uid, FlatpakResult* result);

/* Stage a signed app from its configured remote, as the installing UID (the
 * stub), into /data/linux-app/staging/<uid>/<operation>, before
 * publishing it into that stub UID's Flatpak installation. */
int flatpak_manager_prepare(const char* ref, const char* remote, int installer_uid,
        const char* operation_id,
        char* app_commit, unsigned long app_commit_size,
        char* runtime_ref, unsigned long runtime_ref_size,
        char* runtime_commit, unsigned long runtime_commit_size,
        char* metadata, unsigned long metadata_size,
        char* desktop, unsigned long desktop_size,
        char* error, unsigned long error_size);
int flatpak_manager_installed_for_uid(const char* ref, int uid);

/* A replacement bridge identity means its old PM callbacks cannot return. */
void flatpak_manager_abort_handoffs(void);
void flatpak_manager_handoff(const char* command, const char* ref, const char* remote,
        const char* operation, int uid, FlatpakResult* result);

void flatpak_manager_result_clear(FlatpakResult* result);

#ifdef __cplusplus
}
#endif

#endif
