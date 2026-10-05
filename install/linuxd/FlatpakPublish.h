#ifndef MATONOS_FLATPAK_PUBLISH_H
#define MATONOS_FLATPAK_PUBLISH_H

/* Publish one signed app stub and its pinned runtime. */
int flatpak_publish(const char* app_ref, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        const char* staging_dir, const char* system_dir, const char* user_dir,
        char* error, unsigned long error_size);

/* Stage a signed app and its declared runtime, and return the resolved commit
 * pins plus the app's /metadata and exported desktop entry from the staged
 * commit. No deployment happens; deploy re-verifies the returned pins. */
int flatpak_prepare(const char* app_ref, const char* remote, const char* staging_dir,
        char* app_commit, unsigned long app_commit_size,
        char* runtime_ref, unsigned long runtime_ref_size,
        char* runtime_commit, unsigned long runtime_commit_size,
        char* metadata, unsigned long metadata_size,
        char* desktop, unsigned long desktop_size,
        char* error, unsigned long error_size);

#endif
