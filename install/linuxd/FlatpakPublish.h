#ifndef MATONOS_FLATPAK_PUBLISH_H
#define MATONOS_FLATPAK_PUBLISH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Publish one signed app stub and its pinned runtime. */
int flatpak_publish(const char* app_ref, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        const char* staging_dir, const char* system_dir, const char* user_dir,
        char* error, unsigned long error_size);

/* Stage a signed app and its declared runtime, and return the resolved commit
 * pins plus the app's /metadata and exported desktop entry from the staged
 * commit. No deployment happens; deploy re-verifies the returned pins. */
int flatpak_prepare(const char* app_ref, const char* remote, int installer_uid,
        int runtime_uid, const char* operation_id,
        char* app_commit, unsigned long app_commit_size,
        char* runtime_ref, unsigned long runtime_ref_size,
        char* runtime_commit, unsigned long runtime_commit_size,
        char* metadata, unsigned long metadata_size,
        char* desktop, unsigned long desktop_size,
        char* error, unsigned long error_size);

/* Remove abandoned installer staging operations during linuxd startup. */
int flatpak_cleanup_stale_staging(void);

/* /data/matonos/linux/apps/<uid>/staging/<operation> (validated). */
int flatpak_staging_dir(int uid, const char* operation, char* path, unsigned long size);
/* Remove one staging operation, as its owning UID. */
int flatpak_remove_staging(int uid, const char* operation);

#ifdef __cplusplus
}
#endif

#endif
