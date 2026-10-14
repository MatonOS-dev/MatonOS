#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include "../RemotePin.h"
int main(void) {
    char path[]="/tmp/maton-remote-pin-XXXXXX";assert(mkdtemp(path));
    int fd=open(path,O_RDONLY|O_DIRECTORY);assert(fd>=0);
    const char* ref="app/org.example.App/x86_64/stable";
    assert(!remote_pin_check(fd,ref,"flathub"));
    assert(!remote_pin_commit(fd,ref,"flathub"));
    close(fd);fd=open(path,O_RDONLY|O_DIRECTORY);assert(fd>=0);
    assert(!remote_pin_commit(fd,ref,"flathub"));
    assert(remote_pin_commit(fd,ref,"other")==-2);
    assert(remote_pin_check(fd,"app/org.example.App/aarch64/beta","other")==-2);
    assert(!remote_pin_check(fd,"app/org.example.Other/x86_64/stable","other"));
    assert(remote_pin_check(fd,"app/../../bad","other")==-1);
    assert(!unlinkat(fd,"org.example.App",0));
    assert(!symlinkat("/etc/passwd",fd,"org.example.App"));
    assert(remote_pin_check(fd,ref,"flathub")==-1);
    assert(!unlinkat(fd,"org.example.App",0));
    int bad=openat(fd,"org.example.App",O_CREAT|O_WRONLY,0600);assert(bad>=0);close(bad);
    assert(remote_pin_check(fd,ref,"flathub")==-1);
    unlinkat(fd,"org.example.App",0);close(fd);rmdir(path);
    puts("PASS remote pins: persistence, conflict, cross-architecture identity, symlink/corruption rejection");
}
