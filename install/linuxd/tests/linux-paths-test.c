#define _GNU_SOURCE
#include "../../../linux/flatpak/linux-paths.h"
#include <assert.h>
int main(void) {
    unsigned uid;char one[160],two[160];
    assert(!maton_install_dir(one,sizeof(one),10120));
    assert(!maton_install_dir(two,sizeof(two),210120));
    assert(!strcmp(one,"/data/linux-app/install/10120")&&!strcmp(one,two));
    assert(maton_parse_install_path(one,&uid)&&uid==10120);
    assert(maton_parse_install_path("/data/linux-app/install/10120-aZ09",&uid)&&uid==10120);
    const char* invalid[]={"/data/linux-app/install/010120","/data/linux-app/install/+10120","/data/linux-app/install/210120","/data/linux-app/install/10120-","/data/linux-app/install/10120-abc/..","/data/linux-app/install/9999","/data/linux-app/install/4294977416"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++)assert(!maton_parse_install_path(invalid[i],&uid));
    assert(!maton_staging_owner(one,sizeof(one),10120));
    assert(!strcmp(one,"/data/linux-app/staging/10120"));
    assert(maton_parse_staging_owner(one,&uid)&&uid==10120);
    assert(!maton_staging_owner(two,sizeof(two),110120));
    assert(maton_parse_staging_owner("/data/linux-app/staging/110120",&uid)&&uid==110120);
    assert(!maton_parse_staging_owner("/data/linux-app/staging/010120",&uid));
    assert(maton_parse_pending_path("/data/linux-app/install/.pending-net.sourceforge.ExtremeTuxRacer-aZ09xy"));
    assert(!maton_parse_pending_path("/data/linux-app/install/.pending-..-aZ09xy"));
    assert(!maton_parse_pending_path("/data/linux-app/install/.pending-a/b-aZ09xy"));
    assert(!maton_parse_pending_path("/data/linux-app/install/.pending-app-aZ09x"));
    assert(maton_install_dir(one,8,10120));
    puts("PASS shared app-id paths and app-owned staging");
}
