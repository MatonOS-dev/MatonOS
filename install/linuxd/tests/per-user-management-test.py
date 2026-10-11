#!/usr/bin/env python3
"""Duplicate refs resolve only inside the selected UID; user inventories stay scoped."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
manager = (root / 'install/linuxd/FlatpakManager.c').read_text()
# Exercise the production resolution functions with a fake stock Flatpak info
# result: the same ref is deployed in two users, but never in a third UID.
start = manager.index('int flatpak_manager_installed_for_uid(')
end = manager.index('static void publish_progress_line(', start)
with tempfile.TemporaryDirectory(prefix='flatpak-management-') as temp:
    work = Path(temp)
    source = work / 'test.c'
    source.write_text(r'''
#define _GNU_SOURCE
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "linux/flatpak/linux-paths.h"
typedef struct ChildResult { int status, truncated; char* output; } ChildResult;
static int valid_install_uid(int uid) { return maton_path_uid_valid((unsigned)uid); }
static int flatpak_manager_valid_ref(const char* ref) { return !strcmp(ref,"app/org.example.App/x86_64/stable"); }
static int remembered_runtime_install(int uid,int* runtime) { *runtime=(int)(maton_user_of((unsigned)uid)*100000+10001); return 0; }
static ChildResult run_cli_in(const char* const* args,size_t count,int progress,const char* system,const char* user) {
    assert(count==3 && !progress && !strcmp(args[0],"--user") && !strcmp(args[1],"info"));
    unsigned uid;assert(sscanf(strrchr(user,'/')+1,"%u",&uid)==1);
    char expected[256];assert(!maton_install_dir(expected,sizeof(expected),uid) && !strcmp(expected,user));
    unsigned runtime;assert(sscanf(strrchr(system,'/')+1,"%u",&runtime)==1);
    assert(!maton_runtime_dir(expected,sizeof(expected),maton_user_of(uid),runtime) && !strcmp(expected,system));
    assert(maton_user_of(runtime)==maton_user_of(uid));
    ChildResult result={.status=(uid==10120 || uid==210120) ? 0 : 1,.output=strdup("")};return result;
}
''' + manager[start:end] + r'''
int main(void) {
    const char* ref="app/org.example.App/x86_64/stable";char system[256],user[256];
    assert(!app_install_roots(ref,10120,system,sizeof(system),user,sizeof(user)));
    assert(strstr(user,"/0/matonos-linux/install/10120") && strstr(system,"/0/matonos-linux/runtime/10001"));
    assert(!app_install_roots(ref,210120,system,sizeof(system),user,sizeof(user)));
    assert(strstr(user,"/2/matonos-linux/install/210120") && strstr(system,"/2/matonos-linux/runtime/210001"));
    assert(app_install_roots(ref,110120,system,sizeof(system),user,sizeof(user)));
    assert(app_install_roots(ref,-1,system,sizeof(system),user,sizeof(user)));
    assert(app_install_roots("app/org.other.App/x86_64/stable",10120,system,sizeof(system),user,sizeof(user)));
    MatonPathScan scan;unsigned uid;
    assert(!maton_scan_open_user(&scan,"install",0));
    assert(maton_scan_next(&scan,&uid)==1 && uid==10120);
    assert(maton_scan_next(&scan,&uid)==0);maton_scan_close(&scan);
    assert(!maton_scan_open_user(&scan,"install",2));
    assert(maton_scan_next(&scan,&uid)==1 && uid==210120);
    assert(maton_scan_next(&scan,&uid)==0);maton_scan_close(&scan);
    assert(!maton_scan_open_user(&scan,"install",1));
    assert(maton_scan_next(&scan,&uid)==0);maton_scan_close(&scan);
    puts("Duplicate-ref UID resolution and per-user inventory passed");
}
''')
    for user, uid in [(0,10120),(2,210120)]:
        install = work / 'data' / str(user) / 'matonos-linux/install'
        (install / str(uid)).mkdir(parents=True)
        (install / str(210120 if user == 0 else 10120)).mkdir()  # wrong-user directory rejected
        (install / ('0' + str(uid))).mkdir()  # noncanonical UID rejected
        (install / '10121').symlink_to(install / str(uid))
    binary = work / 'test'
    subprocess.run(['cc','-Wall','-Wextra','-Werror',f'-I{root}',
                    f'-DMATON_MISC_DE="{work / "data"}"',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
