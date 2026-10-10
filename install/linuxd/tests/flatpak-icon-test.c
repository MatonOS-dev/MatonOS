#define _GNU_SOURCE
#include "../FlatpakIcon.h"
#include <assert.h>

static const char* id = "com.brave.Browser";
static const char* exported = "export/share/icons/hicolor/256x256/apps/com.brave.Browser.png";
static const char* bundled = "files/share/app-info/icons/flatpak/256x256/com.brave.Browser.png";

static void write_candidate(int root, const char* relative, int marker, size_t size) {
    char copy[PATH_MAX]; snprintf(copy, sizeof(copy), "%s", relative);
    char* name = strrchr(copy, '/'); assert(name); *name++ = '\0';
    int dir = safe_directory_at(root, copy, 1); assert(dir >= 0);
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    assert(fd >= 0);
    const unsigned char bytes[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', (unsigned char)marker};
    assert(write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    assert(ftruncate(fd, (off_t)size) == 0);
    close(fd); close(dir);
}

static void expect_icon(const char* root, int marker) {
    FILE* file = flatpak_icon_open(root, id); assert(file);
    assert(fseek(file, 8, SEEK_SET) == 0);
    assert(fgetc(file) == marker);
    fclose(file);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    char root[PATH_MAX]; assert(realpath(argv[1], root));
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC); assert(dir >= 0);
    // Regression: SVG-only exported artwork plus a deployment-bundled PNG.
    assert(flatpak_icon_open(root, id) == NULL);
    write_candidate(dir, "export/share/icons/hicolor/scalable/apps/com.brave.Browser.svg", 0, 9);
    write_candidate(dir, bundled, 'B', 9);
    expect_icon(root, 'B');
    // Prefer an exported PNG when one exists; ignore oversized or non-PNG files.
    write_candidate(dir, exported, 'E', 9);
    expect_icon(root, 'E');
    write_candidate(dir, exported, 'E', 256 * 1024 + 1);
    expect_icon(root, 'B');
    int fd = openat(dir, exported, O_WRONLY | O_TRUNC | O_CLOEXEC); assert(fd >= 0);
    assert(write(fd, "not a PNG", 9) == 9); close(fd);
    expect_icon(root, 'B');
    // A symlink to a matching PNG outside the deployment must never be read.
    assert(unlinkat(dir, exported, 0) == 0);
    char outside[PATH_MAX];
    int n = snprintf(outside, sizeof(outside), "%s/../outside.png", root); assert(n > 0 && n < (int)sizeof(outside));
    FILE* escape = fopen(outside, "w"); assert(escape);
    assert(fwrite("\x89PNG\r\n\x1a\nX", 1, 9, escape) == 9); fclose(escape);
    assert(symlinkat(outside, dir, exported) == 0);
    expect_icon(root, 'B');
    assert(unlinkat(dir, bundled, 0) == 0);
    assert(flatpak_icon_open(root, id) == NULL);
    // The other former fallback also uses artwork inside this deployment.
    write_candidate(dir, "files/share/app-info/media/com/brave/Browser/hash/icons/128x128/com.brave.Browser.png", 'M', 9);
    expect_icon(root, 'M');
    assert(flatpak_icon_open(root, "../../other") == NULL);
    close(dir);
    puts("PASS: Brave bundled PNG, export precedence, media thumbnails, size/signature checks and deployment boundary");
    return 0;
}
