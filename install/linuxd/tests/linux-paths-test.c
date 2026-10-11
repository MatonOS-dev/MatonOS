#define _GNU_SOURCE
#include "../../../linux/flatpak/linux-paths.h"
#include <assert.h>
int main(void) {
    unsigned uid=0;char path[160];
    assert(!maton_install_dir(path,sizeof(path),10120));
    assert(!strcmp(path,"/data/misc_de/0/matonos-linux/install/10120"));
    assert(maton_parse_install_path(path,&uid)&&uid==10120);
    assert(!maton_install_dir(path,sizeof(path),210120));
    assert(maton_parse_install_path(path,&uid)&&uid==210120);
    assert(!maton_home_dir(path,sizeof(path),210120));
    assert(!strcmp(path,"/data/misc_ce/2/matonos-linux/home/210120"));
    assert(maton_parse_home_path(path,&uid)&&uid==210120);
    assert(!maton_runtime_dir(path,sizeof(path),2,210121));
    assert(maton_parse_path(path,"de","runtime",&uid)&&uid==210121);
    assert(maton_runtime_dir(path,sizeof(path),0,210121));
    assert(!maton_parse_install_path("/data/misc_de/0/matonos-linux/install/210120",&uid));
    assert(!maton_parse_home_path("/data/misc_ce/2/matonos-linux/home/10120",&uid));
    assert(!maton_parse_home_path("/data/matonos/linux/home/10120",&uid));
    assert(!maton_parse_home_path("/data/misc_ce/0/matonos-linux/home/010120",&uid));
    assert(!maton_parse_home_path("/data/misc_ce/00/matonos-linux/home/10120",&uid));
    assert(!maton_parse_home_path("/data/misc_ce/0/matonos-linux/home/+10120",&uid));
    assert(!maton_parse_home_path("/data/misc_ce/0/matonos-linux/home/4294977416",&uid));
    assert(!maton_parse_install_path("/data/misc_de/0/matonos-linux/install/10120/..",&uid));
    assert(!maton_parse_install_path("/data/misc_de/0/matonos-linux/install/9999",&uid));
    assert(maton_home_dir(path,8,10120));
    puts("Per-user path validation passed");return 0;
}
