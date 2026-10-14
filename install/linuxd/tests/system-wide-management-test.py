#!/usr/bin/env python3
"""Production queries resolve the same code for multiple users; pointers pin versions."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'install/linuxd/FlatpakManager.c').read_text()
start=s.index('int flatpak_manager_installed_for_uid(')
end=s.index('/* Existing installations are migrated',start)
with tempfile.TemporaryDirectory(prefix='linux-app-query-') as tmp:
    work=Path(tmp);store=work/'store';install=store/'install';install.mkdir(parents=True)
    (install/'10120-Ab09').mkdir();(install/'10120').symlink_to('10120-Ab09')
    source=work/'test.c'
    source.write_text(r'''
#define _GNU_SOURCE
#include <assert.h>
#include "linux/flatpak/linux-paths.h"
typedef struct ChildResult { int status,truncated;char* output; } ChildResult;
static int valid_install_uid(int uid){return uid>=0&&maton_path_uid_valid((unsigned)uid);}
static int flatpak_manager_valid_ref(const char* ref){return !strcmp(ref,"app/org.example.App/x86_64/stable");}
static ChildResult run_cli_in(const char* const* args,size_t count,int progress,const char* system,const char* user){
    assert(count==3&&!progress&&!strcmp(args[0],"--user")&&!strcmp(args[1],"info"));
    assert(!strcmp(system,MATON_RUNTIME_DIR));unsigned appid;
    assert(maton_parse_install_path(user,&appid));
    ChildResult result={.status=appid==10120?0:1,.output=strdup("")};return result;
}
''' +s[start:end]+r'''
int main(void){
    char system[256],one[256],two[256];const char* ref="app/org.example.App/x86_64/stable";
    assert(!app_install_roots(ref,10120,system,sizeof(system),one,sizeof(one)));
    assert(!app_install_roots(ref,210120,system,sizeof(system),two,sizeof(two)));
    assert(!strcmp(one,two)&&strstr(one,"10120-Ab09"));
    assert(app_install_roots(ref,10121,system,sizeof(system),two,sizeof(two)));
    assert(app_install_roots(ref,-1,system,sizeof(system),two,sizeof(two)));
    assert(app_install_roots("app/org.other.App/x86_64/stable",10120,system,sizeof(system),two,sizeof(two)));
    puts("PASS duplicate-user queries share code and resolve immutable version paths");
}
''')
    binary=work/'test'
    subprocess.run(['cc','-Wall','-Wextra','-Werror',f'-I{root}',f'-DMATON_LINUX_ROOT="{store}"',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
