#ifndef MATON_FLATPAK_ICON_H
#define MATON_FLATPAK_ICON_H

#include "SafePath.h"
#include <limits.h>
#include <stdlib.h>

/* Read only PNGs belonging to the verified, system-owned app deployment.
 * AppStream parsing is not needed to read artwork bundled by the app itself. */
static inline FILE* flatpak_icon_candidate(const char* root, const char* path) {
    char resolved[PATH_MAX];
    size_t length = strlen(root);
    if (!realpath(path, resolved) || strncmp(resolved, root, length) || resolved[length] != '/') return NULL;
    FILE* file = safe_fopen_absolute(resolved);
    if (!file) return NULL;
    struct stat st;
    unsigned char signature[8];
    if (fstat(fileno(file), &st) || st.st_size < 8 || st.st_size > 256 * 1024 ||
            fread(signature, 1, sizeof(signature), file) != sizeof(signature) ||
            memcmp(signature, "\x89PNG\r\n\x1a\n", sizeof(signature))) {
        fclose(file); return NULL;
    }
    rewind(file);
    return file;
}

static inline FILE* flatpak_icon_open(const char* root, const char* app_id) {
    if (!root || !app_id || !*app_id || strlen(app_id) > 255 || strchr(app_id, '/') || strchr(app_id, '\\')) return NULL;
    const char* sizes[] = {"256x256", "128x128", "64x64", "48x48", "512x512", "32x32"};
    const char* flat_sizes[] = {"512x512", "256x256", "128x128", "128x128@2", "64x64", "64x64@2", "48x48", "32x32"};
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        int n = snprintf(path, sizeof(path), "%s/export/share/icons/hicolor/%s/apps/%s.png", root, sizes[i], app_id);
        if (n < 0 || n >= (int)sizeof(path)) continue;
        FILE* file = flatpak_icon_candidate(root, path);
        if (file) return file;
    }
    // Brave exports SVG but bundles PNG artwork under app-info/icons/flatpak.
    for (size_t i = 0; i < sizeof(flat_sizes) / sizeof(flat_sizes[0]); ++i) {
        int n = snprintf(path, sizeof(path), "%s/files/share/app-info/icons/flatpak/%s/%s.png", root, flat_sizes[i], app_id);
        if (n < 0 || n >= (int)sizeof(path)) continue;
        FILE* file = flatpak_icon_candidate(root, path);
        if (file) return file;
    }
    // Older deployments can bundle the same thumbnail in an app-info media tree.
    char media[PATH_MAX], components[256];
    strcpy(components, app_id);
    for (char* p = components; *p; ++p) if (*p == '.') *p = '/';
    int n = snprintf(media, sizeof(media), "%s/files/share/app-info/media/%s", root, components);
    if (n < 0 || n >= (int)sizeof(media)) return NULL;
    DIR* directory = safe_opendir_absolute(media);
    if (!directory) return NULL;
    FILE* file = NULL;
    struct dirent* entry;
    unsigned visited = 0;
    const char* thumbnails[] = {"128x128@2", "128x128", "64x64"};
    while (!file && visited++ < 256 && (entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        for (size_t i = 0; i < sizeof(thumbnails) / sizeof(thumbnails[0]); ++i) {
            n = snprintf(path, sizeof(path), "%s/%s/icons/%s/%s.png", media, entry->d_name, thumbnails[i], app_id);
            if (n < 0 || n >= (int)sizeof(path)) continue;
            file = flatpak_icon_candidate(root, path);
            if (file) break;
        }
    }
    closedir(directory);
    return file;
}
#endif
