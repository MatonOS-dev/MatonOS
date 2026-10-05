#include "../FlatpakPublish.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc != 9) {
        fprintf(stderr, "usage: %s APP_REF APP_COMMIT RUNTIME_REF RUNTIME_COMMIT REMOTE R S U\n", argv[0]);
        return 2;
    }
    char error[512] = {0};
    int rc = flatpak_publish(argv[1], argv[2], argv[3], argv[4], argv[5],
            argv[6], argv[7], argv[8], error, sizeof(error));
    if (rc) fprintf(stderr, "%s\n", error[0] ? error : "publish failed");
    return rc ? 1 : 0;
}
