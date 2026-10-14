#define _GNU_SOURCE
#include <sys/stat.h>
#include <assert.h>
static int test_lstat(const char* path,struct stat* st) {
    (void)path;st->st_mode=S_IFDIR|0700;st->st_uid=2902;return 0;
}
#define lstat test_lstat
#define MATON_STORE_NO_MAIN
#include "../FlatpakStore.c"
#undef lstat
int main(void) {
    char* args[]={"store","stage",MATON_STAGING_ROOT "/2902/op","2902","flathub","org.example.App//stable"};
    assert(stage_args_valid(6,args));
    args[3]="10120";assert(!stage_args_valid(6,args));args[3]="2902";
    args[2]=MATON_STAGING_ROOT "/10120/op";assert(!stage_args_valid(6,args));
    args[2]=MATON_STAGING_ROOT "/2902/op/escape";assert(!stage_args_valid(6,args));
    args[2]=MATON_STAGING_ROOT "/2902/op";
    args[4]="--user";assert(!stage_args_valid(6,args));args[4]="flathub";
    args[5]="--runtime";assert(!stage_args_valid(6,args));
    puts("PASS helper accepts FPINSTALL only, rejects app UIDs, paths and CLI flags");
}
