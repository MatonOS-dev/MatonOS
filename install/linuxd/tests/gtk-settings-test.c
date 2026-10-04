#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include "../GtkSettings.h"

static void put(int dirfd, const char* name, const char* text) {
    int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(close(fd) == 0);
}
static void expect(int dirfd, const char* name, const char* text) {
    char contents[512] = {0};
    int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW);
    assert(fd >= 0); assert(read(fd, contents, sizeof(contents)-1) >= 0); close(fd);
    assert(!strcmp(contents, text));
}
int main(void) {
    char root[] = "/tmp/maton-gtk-XXXXXX";
    assert(mkdtemp(root));
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(fd >= 0);
    assert(mkdirat(fd, "outside", 0700) == 0);
    int outside = openat(fd, "outside", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(outside >= 0);
    put(outside, "settings.ini", "victim\n");
    /* A pre-existing app-controlled parent symlink must never be traversed. */
    assert(symlinkat("outside", fd, "config") == 0);
    set_gtk_settings_key(fd, "config/gtk-3.0");
    struct stat st;
    assert(fstatat(outside, "gtk-3.0", &st, 0) < 0 && errno == ENOENT);
    expect(outside, "settings.ini", "victim\n");
    assert(unlinkat(fd, "config", 0) == 0);
    set_gtk_settings_key(fd, "config/gtk-3.0");
    int gtk = safe_directory_at(fd, "config/gtk-3.0", 0); assert(gtk >= 0);
    expect(gtk, "settings.ini", "[Settings]\ngtk-decoration-layout=:\n");
    put(gtk, "settings.ini", "[Settings]\ngtk-theme-name=Adwaita\ngtk-decoration-layout=close\n[Other]\nx=y\n");
    set_gtk_settings_key(fd, "config/gtk-3.0");
    expect(gtk, "settings.ini", "[Settings]\ngtk-theme-name=Adwaita\ngtk-decoration-layout=:\n[Other]\nx=y\n");
    assert(unlinkat(gtk, "settings.ini", 0) == 0);
    assert(symlinkat("../../outside/settings.ini", gtk, "settings.ini") == 0);
    set_gtk_settings_key(fd, "config/gtk-3.0");
    expect(outside, "settings.ini", "victim\n");
    assert(fstatat(gtk, "settings.ini", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode));
    assert(safe_directory_at(fd, "config/../outside", 1) < 0);
    /* Replacement always creates a new inode: an app's hard link is untouched. */
    assert(unlinkat(gtk, "settings.ini", 0) == 0);
    assert(linkat(outside, "settings.ini", gtk, "settings.ini", 0) == 0);
    set_gtk_settings_key(fd, "config/gtk-3.0");
    expect(outside, "settings.ini", "victim\n");
    assert(unlinkat(gtk, "settings.ini", 0) == 0); close(gtk);
    assert(unlinkat(fd, "config/gtk-3.0", AT_REMOVEDIR) == 0);
    assert(unlinkat(fd, "config", AT_REMOVEDIR) == 0);
    assert(unlinkat(outside, "settings.ini", 0) == 0); close(outside);
    assert(unlinkat(fd, "outside", AT_REMOVEDIR) == 0); close(fd);
    assert(rmdir(root) == 0);
    puts("GTK parent/file symlink rejection, atomic replacement and preservation passed");
    return 0;
}
