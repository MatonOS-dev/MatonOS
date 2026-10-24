#ifndef MATONOS_MOUNTD_CORE_H
#define MATONOS_MOUNTD_CORE_H

#include <sys/types.h>

#define MOUNTD_ID_MAX 80
#define MOUNTD_PACKAGE_MAX 256

typedef struct {
    pid_t pid;
    uid_t uid;
    char package[MOUNTD_PACKAGE_MAX];
    char deployment[MOUNTD_ID_MAX];
    char signer[65];
    char request_nonce[37];
} MountdRequest;

#ifdef __cplusplus
extern "C" {
#endif

int mountd_validate_request(const MountdRequest *request);
int mountd_derive_paths(const MountdRequest *request, char *app, size_t app_size,
                        char *runtime, size_t runtime_size, char *home,
                        size_t home_size, char *target_home,
                        size_t target_home_size);
int mountd_mount_request(const MountdRequest *request, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
