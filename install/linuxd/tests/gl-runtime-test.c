#define MATONOS_PUBLISH_HOST_TEST
#include "../FlatpakPublish.c"
#include <assert.h>

int main(void) {
    char ref[512];
    const char* modern = "[Runtime]\nname=org.freedesktop.Platform\n"
        "[Extension org.freedesktop.Platform.GL]\nversions = 25.08;25.08-extra;1.4\nversion=1.4\n";
    assert(gl_ref_from_metadata(modern, "runtime/org.freedesktop.Platform/x86_64/25.08", ref, sizeof(ref)) == 0);
    assert(!strcmp(ref, "runtime/org.freedesktop.Platform.GL.default/x86_64/25.08"));
    assert(gl_ref_from_metadata(modern, "runtime/org.kde.Platform/x86_64/5.15-25.08", ref, sizeof(ref)) == 0);
    assert(!strcmp(ref, "runtime/org.freedesktop.Platform.GL.default/x86_64/25.08"));
    assert(gl_ref_from_metadata("[Extension org.freedesktop.Platform.GL]\nversion=24.08\n",
        "runtime/org.gnome.Platform/aarch64/48", ref, sizeof(ref)) == 0);
    assert(!strcmp(ref, "runtime/org.freedesktop.Platform.GL.default/aarch64/24.08"));
    assert(gl_ref_from_metadata("[Runtime]\nname=Console\n", "runtime/Console/x86_64/stable", ref, sizeof(ref)) == 1);
    assert(gl_ref_from_metadata("[Extension org.freedesktop.Platform.GL]\nversions=../../escape\n",
        "runtime/Test/x86_64/stable", ref, sizeof(ref)) == -1);
    assert(gl_ref_from_metadata("[Extension org.freedesktop.Platform.GL]\n", "runtime/Test/x86_64/stable", ref, sizeof(ref)) == -1);
    assert(gl_ref_from_metadata(modern, "runtime/Test/x86_64/stable", ref, 8) == -1);
    puts("PASS: standard Mesa ref follows runtime metadata, architecture and modern GL versions");
    const char* steam = "[Application]\nname=com.valvesoftware.Steam\n"
        "[Extension org.freedesktop.Platform.Compat.i386]\ndirectory=lib/i386-linux-gnu\nversion=26.08\n"
        "[Extension org.freedesktop.Platform.GL32]\nversion=1.4\nversions=26.08;26.08-extra;1.4\n";
    assert(ext_ref_from_metadata(steam, "runtime/org.freedesktop.Platform/x86_64/26.08",
            "org.freedesktop.Platform.Compat.i386", "org.freedesktop.Platform.Compat.i386", ref, sizeof(ref)) == 0);
    assert(!strcmp(ref, "runtime/org.freedesktop.Platform.Compat.i386/x86_64/26.08"));
    assert(ext_ref_from_metadata(steam, "runtime/org.freedesktop.Platform/x86_64/26.08",
            "org.freedesktop.Platform.GL32", "org.freedesktop.Platform.GL32.default", ref, sizeof(ref)) == 0);
    assert(!strcmp(ref, "runtime/org.freedesktop.Platform.GL32.default/x86_64/26.08"));
    assert(ext_ref_from_metadata(modern, "runtime/org.freedesktop.Platform/x86_64/26.08",
            "org.freedesktop.Platform.GL32", "org.freedesktop.Platform.GL32.default", ref, sizeof(ref)) == 1);
    return 0;
}
