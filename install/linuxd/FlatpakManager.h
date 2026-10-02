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

void flatpak_manager_launch_graphical(const char* ref, int runtime_directory_fd, const char* dns_servers, int x11_directory_fd, const char* x11_display, FlatpakResult* result);
void flatpak_manager_init(void);
void flatpak_manager_set_callbacks(FlatpakProgressCallback progress,
        FlatpakCompleteCallback complete, void* context);
int flatpak_manager_valid_ref(const char* value);
int flatpak_manager_valid_app_id(const char* value);
void flatpak_manager_call(const char* command, const char* ref, const char* app_id,
        const char* const* run_args, size_t run_arg_count, int delete_data,
        const char* operation_id, FlatpakResult* result);
void flatpak_manager_result_clear(FlatpakResult* result);

#ifdef __cplusplus
}
#endif

#endif
