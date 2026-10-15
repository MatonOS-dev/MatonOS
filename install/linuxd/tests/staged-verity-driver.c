#define _GNU_SOURCE
#include "../FlatpakPublish.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
/* Real ioctl adapter for the retained-mapping regression test. It never
 * publishes or falls back. stdout preserves the kernel errno for the test. */
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    int parent = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) return 2;
    char error[128] = {0};
    int fd = flatpak_seal_staged_file(parent, argv[2], error, sizeof(error));
    int code = fd < 0 ? errno : 0;
    printf("%d\n%s\n", code, error);
    if (fd >= 0) close(fd);
    close(parent);
    return 0;
}
