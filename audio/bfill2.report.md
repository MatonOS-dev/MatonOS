# bfill2 verification report

## Commits

- bionic-fill (main): `89f806d01ec47f99a67b6e40111f9bf9466d6678`.
- Device-tree integration (wip/bfill2): `07abef33afde1882c1f0dec94f971f2ad6280e3a`.
- This report is a following documentation commit (see git log for its ID).
- No push, Soong, Gradle, VM, or changes to the original device-tree checkout.

## Changes and semantics

- pthread_setaffinity_np / pthread_getaffinity_np use pthread_gettid_np and
  sched_setaffinity / sched_getaffinity. Return 0 or an errno number, preserving
  the caller's errno. Valid live pthread handles are required, as for glibc.
- fopencookie, cookie_io_functions_t and all four callback typedefs are inline
  adapters over funopen64. Verified NDK stdio.h's int-sized read/write callbacks,
  fpos64_t-valued seek, and int close result. Seek converts the offset pointer
  update to funopen's return offset; close propagates its result and frees the
  adapter. Allocation/open failures free only the adapter, leaving the caller's
  cookie alone. NULL reads return EOF; NULL writes discard, as requested.
  Read/write mode restrictions and append seeks are supported. GNU mode letters
  for descriptor operations have no effect; no descriptor exists. Appending
  without a seek callback relies on the writer, and callbacks must respect their
  buffer bounds. Runtime buffering/error details were not tested against glibc.
- Nine libintl functions provide untranslated strings and English n == 1 plural
  selection. Setters do not retain configuration: queries return defaults, unlike
  a complete libintl implementation. README documents those deliberate limits.
- New affinity/cookie fills are bionic-only, with HAVE_* feature overrides for
  native APIs. Function declarations cannot be discovered using #ifndef alone;
  configure must set these overrides when a future bionic gains native symbols.
- Added shared libintl.h: PipeWire src/pipewire/pipewire.c and WirePlumber
  lib/wp/wp.c and src/tools/wpctl.c unconditionally include <libintl.h>.
  __has_include_next defers to a real header; fallback applies only if absent.
- Removed audio/patches/0002-bionic-no-thread-affinity.patch and
  audio/patches/0005-avoid-glibc-fopencookie-on-bionic.patch. This restores actual
  affinity and ALSA diagnostics rather than silent no-op / /dev/null behavior.
- Removed audio/compat/include/libintl.h and all recipe references to that path
  (it contained no other headers). Both PipeWire and WirePlumber now use
  -I$SRC/bionic-fill and -include $SRC/bionic-fill/bionic-fill.h.
- Retained WirePlumber 0002 solely for Meson's optional intl dependency: a
  header cannot satisfy dependency('intl')'s required library lookup. Its diff
  was already only that one Meson line; updated its explanation.
- Updated immutable recipe pin and audio/pipewire-PROVENANCE.md. Surviving patch
  numbers remain stable, with intentional gaps documented.

## Verification

Passed compile **and link**, NDK r30 x86_64 android-35, every new symbol used:

```sh
cd /home/hanro50/matonos/repos/bionic-fill
/home/hanro50/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang -Wall -Wextra -Werror -I. tests/compile.c -o check
```

Also passed with -D_FILE_OFFSET_BITS=64. The host command
`cc -Wall -Wextra -Werror -I. tests/compile.c -o check` passed (real glibc
symbols and real libintl header). Test executables were removed; not executed.
The first host attempt lacked explicit pthread.h and stdio.h in the test;
added those includes and reran successfully before committing.

Four standalone object builds passed with no diagnostics. Sources came from
`git show HEAD:<path>` in the existing cached repos, not patched working files:
PipeWire HEAD 8fa27cabdc6c0c1350c69c026af5850ef0af1e26 and WirePlumber HEAD
11e501181fb87dc8e72e55156d672109fbad2434 (both match recipe pins).
Cached compile_commands.json supplied configured include/define flags; changed
stale machine paths to current paths, replaced the old compatibility include
with bionic-fill include/force-include, and redirected outputs into private
scratch. No coordinator output/source was mutated. Commands follow below.
`llvm-nm -u` on thread.c.o confirmed pthread_gettid_np and sched_setaffinity
references. Shell syntax (`bash -n tools/build-pipewire.sh`) and git diff
whitespace checks passed. Temporary sources, objects and logs were removed.

Full Meson configure/build and complete stack linking were not run: the recipe
fetches and mutates shared caches and destinations outside the two authorized
write roots. Existing configured outputs allowed the focused object checks.
No runtime/VM, boot, translation-state, cookie I/O runtime, or hardware claim.
The coordinator must push the bionic-fill commit before the pinned fetch,
then perform the server build. No shared-file or kernel change is needed.

## Focused compilation commands

Each input below was the matching upstream HEAD file copied into .bfill2-check.

```sh
/home/hanro50/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang -I/mnt/data/aosp/out/pc-pipewire/pipewire/spa/plugins/alsa/libspa-alsa.so.p -I/mnt/data/aosp/out/pc-pipewire/pipewire/spa/plugins/alsa -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/spa/plugins/alsa -I/mnt/data/aosp/out/pc-pipewire/pipewire -I/mnt/data/aosp/out/pc-pipewire/src/pipewire -I/mnt/data/aosp/out/pc-pipewire/pipewire/spa/include -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/spa/include -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/spa/include-private -I/mnt/data/aosp/out/pc-pipewire/dest/vendor/include -fdiagnostics-color=always -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu11 -O3 -fvisibility=hidden -fno-strict-aliasing -fno-strict-overflow -DSPA_AUDIO_MAX_CHANNELS=128u -Wsign-compare -Wpointer-arith -Wpointer-sign -Werror=format -Wno-error=format-overflow -Wformat-security -Wimplicit-fallthrough -Wmissing-braces -Wtype-limits -Wvariadic-macros -Wno-missing-field-initializers -Wno-unused-parameter -Wno-pedantic -Wdeprecated-declarations -Wunused-result -Werror=return-type -Werror=float-conversion -Werror=constant-conversion -D_GNU_SOURCE -DFASTPATH -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion -Werror=old-style-definition -Werror=strict-prototypes -O2 -fPIC -D_FILE_OFFSET_BITS=64 -fPIC -DHAVE_ALSA_UCM -DHAVE_READLINK -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/spa/plugins/alsa -I/home/hanro50/matonos/repos/bionic-fill -include /home/hanro50/matonos/repos/bionic-fill/bionic-fill.h -Werror=implicit-function-declaration -c /home/hanro50/matonos/build/dt-bfill/.bfill2-check/pipewire-alsa-pcm.c -o /home/hanro50/matonos/build/dt-bfill/.bfill2-check/pipewire-alsa-pcm.c.o
```

```sh
/home/hanro50/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang -I/mnt/data/aosp/out/pc-pipewire/pipewire/src/pipewire/libpipewire-0.3.so.p -I/mnt/data/aosp/out/pc-pipewire/pipewire/src/pipewire -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/src/pipewire -I/mnt/data/aosp/out/pc-pipewire/pipewire/src -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/src -I/mnt/data/aosp/out/pc-pipewire/pipewire -I/mnt/data/aosp/out/pc-pipewire/src/pipewire -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/include -I/mnt/data/aosp/out/pc-pipewire/pipewire/spa/include -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/spa/include -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/spa/include-private -fdiagnostics-color=always -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu11 -O3 -fvisibility=hidden -fno-strict-aliasing -fno-strict-overflow -DSPA_AUDIO_MAX_CHANNELS=128u -Wsign-compare -Wpointer-arith -Wpointer-sign -Werror=format -Wno-error=format-overflow -Wformat-security -Wimplicit-fallthrough -Wmissing-braces -Wtype-limits -Wvariadic-macros -Wno-missing-field-initializers -Wno-unused-parameter -Wno-pedantic -Wdeprecated-declarations -Wunused-result -Werror=return-type -Werror=float-conversion -Werror=constant-conversion -D_GNU_SOURCE -DFASTPATH -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion -Werror=old-style-definition -Werror=strict-prototypes -O2 -fPIC -D_FILE_OFFSET_BITS=64 -fPIC -pthread -DOLD_MEDIA_SESSION_WORKAROUND=1 -D_POSIX_C_SOURCE -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/src/pipewire -I/home/hanro50/matonos/repos/bionic-fill -include /home/hanro50/matonos/repos/bionic-fill/bionic-fill.h -Werror=implicit-function-declaration -c /home/hanro50/matonos/build/dt-bfill/.bfill2-check/pipewire-pipewire.c -o /home/hanro50/matonos/build/dt-bfill/.bfill2-check/pipewire-pipewire.c.o
```

```sh
/home/hanro50/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang -I/mnt/data/aosp/out/pc-pipewire/pipewire/src/pipewire/libpipewire-0.3.so.p -I/mnt/data/aosp/out/pc-pipewire/pipewire/src/pipewire -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/src/pipewire -I/mnt/data/aosp/out/pc-pipewire/pipewire/src -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/src -I/mnt/data/aosp/out/pc-pipewire/pipewire -I/mnt/data/aosp/out/pc-pipewire/src/pipewire -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/include -I/mnt/data/aosp/out/pc-pipewire/pipewire/spa/include -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/spa/include -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/spa/include-private -fdiagnostics-color=always -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu11 -O3 -fvisibility=hidden -fno-strict-aliasing -fno-strict-overflow -DSPA_AUDIO_MAX_CHANNELS=128u -Wsign-compare -Wpointer-arith -Wpointer-sign -Werror=format -Wno-error=format-overflow -Wformat-security -Wimplicit-fallthrough -Wmissing-braces -Wtype-limits -Wvariadic-macros -Wno-missing-field-initializers -Wno-unused-parameter -Wno-pedantic -Wdeprecated-declarations -Wunused-result -Werror=return-type -Werror=float-conversion -Werror=constant-conversion -D_GNU_SOURCE -DFASTPATH -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion -Werror=old-style-definition -Werror=strict-prototypes -O2 -fPIC -D_FILE_OFFSET_BITS=64 -fPIC -pthread -DOLD_MEDIA_SESSION_WORKAROUND=1 -D_POSIX_C_SOURCE -I/mnt/data/aosp/out/pc-pipewire/src/pipewire/src/pipewire -I/home/hanro50/matonos/repos/bionic-fill -include /home/hanro50/matonos/repos/bionic-fill/bionic-fill.h -Werror=implicit-function-declaration -c /home/hanro50/matonos/build/dt-bfill/.bfill2-check/pipewire-thread.c -o /home/hanro50/matonos/build/dt-bfill/.bfill2-check/pipewire-thread.c.o
```

```sh
/home/hanro50/Android/Sdk/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android35-clang -I/mnt/data/aosp/out/pc-pipewire/wireplumber/lib/wp/libwireplumber-0.5.so.p -I/mnt/data/aosp/out/pc-pipewire/wireplumber/lib/wp -I/mnt/data/aosp/out/pc-pipewire/src/wireplumber/lib/wp -I/mnt/data/aosp/out/pc-pipewire/wireplumber/lib -I/mnt/data/aosp/out/pc-pipewire/src/wireplumber/lib -I/mnt/data/aosp/out/pc-pipewire/dest/vendor/include -I/mnt/data/aosp/out/pc-pipewire/dest/vendor/include/glib-2.0 -I/mnt/data/aosp/out/pc-pipewire/dest/vendor/lib64/glib-2.0/include -I/mnt/data/aosp/out/pc-pipewire/dest/vendor/include/pipewire-0.3 -I/mnt/data/aosp/out/pc-pipewire/dest/vendor/include/spa-0.2 -fdiagnostics-color=always -Wall -Winvalid-pch -O3 -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_68 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_68 '-DGETTEXT_PACKAGE="wireplumber"' -fvisibility=hidden -Wsign-compare -Wpointer-arith -Wpointer-sign -Wformat -Wformat-security -Wimplicit-fallthrough -Wmissing-braces -Wtype-limits -Wvariadic-macros -Wno-missing-field-initializers -Wno-unused-parameter -Wno-pedantic -Wunused-result -D_GNU_SOURCE -DG_LOG_USE_STRUCTURED -DWP_USE_LOCAL_LOG_TOPIC_IN_G_LOG -O2 -fPIC -D_FILE_OFFSET_BITS=64 -fPIC -D_REENTRANT -fno-strict-aliasing -fno-strict-overflow -pthread -DBUILDING_WP -I/mnt/data/aosp/out/pc-pipewire/src/wireplumber/lib/wp -I/home/hanro50/matonos/repos/bionic-fill -include /home/hanro50/matonos/repos/bionic-fill/bionic-fill.h -Werror=implicit-function-declaration -c /home/hanro50/matonos/build/dt-bfill/.bfill2-check/wireplumber-wp.c -o /home/hanro50/matonos/build/dt-bfill/.bfill2-check/wireplumber-wp.c.o
```

