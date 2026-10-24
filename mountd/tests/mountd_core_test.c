#include "../mountd_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    MountdRequest request = {.pid = 1234, .uid = 10123};
    strcpy(request.package, "org.matonos.linux.firefox");
    strcpy(request.deployment, "app-0123abcd");
    assert(mountd_validate_request(&request) == 0);

    char app[512], runtime[512], home[512], target[64];
    assert(mountd_derive_paths(&request, app, sizeof(app), runtime, sizeof(runtime),
                               home, sizeof(home), target, sizeof(target)) == 0);
    assert(strcmp(app, "/data/misc_ce/0/matonos-linux/deployments/app-0123abcd/app") == 0);
    assert(strcmp(runtime, "/data/misc_ce/0/matonos-linux/deployments/app-0123abcd/usr") == 0);
    assert(strcmp(home, "/data/misc_ce/0/matonos-linux/home/10123") == 0);
    assert(strcmp(target, "/home/10123") == 0);

    request.uid = 110123;
    assert(mountd_derive_paths(&request, app, sizeof(app), runtime, sizeof(runtime),
                              home, sizeof(home), target, sizeof(target)) == 0);
    assert(strncmp(app, "/data/misc_ce/1/", 15) == 0);
    assert(strncmp(home, "/data/misc_ce/1/", 15) == 0);

    strcpy(request.deployment, "../escape");
    assert(mountd_validate_request(&request) != 0);
    strcpy(request.deployment, "valid/deployment");
    assert(mountd_validate_request(&request) != 0);
    strcpy(request.deployment, "valid");
    strcpy(request.package, "org.example..bad");
    assert(mountd_validate_request(&request) != 0);
    strcpy(request.package, "org.example.good");
    request.pid = 1;
    assert(mountd_validate_request(&request) != 0);
    request.pid = 1234;
    request.uid = 999;
    assert(mountd_validate_request(&request) != 0);

    puts("mountd_core_test: passed");
    return 0;
}
