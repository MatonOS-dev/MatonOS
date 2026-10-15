#ifndef MATONOS_FLATPAK_PUBLISH_H
#define MATONOS_FLATPAK_PUBLISH_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Seal one regular file in a pinned, exclusively linuxd-owned directory.
 * Return a measured read-only fd (caller closes), or -1 with an error. This
 * primitive alone does not authenticate or publish a staged deployment. */
int flatpak_seal_staged_file(int directory, const char* component,
        char* error, unsigned long error_size);

#define MATON_STAGED_DEPLOYMENTS_MAX 64
#define MATON_STAGED_SUBPATHS_MAX 32
struct FlatpakStagedDeployment {
    char ref[512], commit[65];
    size_t subpath_count;
    char subpaths[MATON_STAGED_SUBPATHS_MAX][256];
};
struct FlatpakStagedManifest {
    unsigned version;
    char operation_id[65], app_ref[512], app_commit[65], remote[129];
    size_t deployment_count;
    struct FlatpakStagedDeployment deployments[MATON_STAGED_DEPLOYMENTS_MAX];
};
int flatpak_begin_staged_install(int uid,const char* operation,const char* remote,
        char* error,unsigned long error_size);
int flatpak_verify_publish_staged_tracked(const struct FlatpakStagedManifest*,
        const char* staging,const char* system,const char* pending,
        char* error,unsigned long error_size,int (*record)(const char*,void*),
        int (*complete)(const char* const*,size_t,void*),void* context);

/* Per-thread staging output observer; exec children never invoke it. */
void flatpak_publish_progress(void (*callback)(const char*));

int flatpak_remote_verified(const char* remote);

/* Remove abandoned installer staging operations during linuxd startup. */
int flatpak_cleanup_stale_staging(void);

/* /data/linux-app/staging/<uid>/<operation> (validated). */
int flatpak_staging_dir(int uid, const char* operation, char* path, unsigned long size);
/* Remove one staging operation, as its owning UID. */
int flatpak_remove_staging(int uid, const char* operation);

#ifdef __cplusplus
}
#endif

#endif
