#ifndef MATONOS_FLATPAK_PUBLISH_H
#define MATONOS_FLATPAK_PUBLISH_H

/* Publish one signed app stub and its pinned runtime. */
int flatpak_publish(const char* app_ref, const char* app_commit,
        const char* runtime_ref, const char* runtime_commit, const char* remote,
        const char* staging_dir, const char* system_dir, const char* user_dir,
        char* error, unsigned long error_size);

#endif
