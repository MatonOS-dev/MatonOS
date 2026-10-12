/* Stub identity is verified by PackageManager through the authenticated bridge. */
#include "StubVerify.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define SIGNER_PATH "/data/matonos/linux/store/stub-signer.der"
#define SIGNER_TEMP SIGNER_PATH ".tmp"
#define MAX_CERT 8192

static void set_error(char* error, size_t size, const char* message) {
    if (error && size) snprintf(error, size, "%s", message);
}

int stub_store_signer(const unsigned char* der, size_t length) {
    if (!der || length < 16 || length > MAX_CERT || der[0] != 0x30) { errno = EINVAL; return -1; }
    int fd = open(SIGNER_TEMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, der, length) == (ssize_t)length && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(SIGNER_TEMP, SIGNER_PATH)) { unlink(SIGNER_TEMP); return -1; }
    return 0;
}

/* The C++ binder shim queries the bridge's live PackageManager provider. */
extern int stub_identity_query(const char* command, int uid, const char* package,
        char* ref, size_t ref_size, char* remote, size_t remote_size);

int stub_runtime_uid(int uid) {
    return stub_identity_query("runtime", uid, "", NULL, 0, NULL, 0);
}
int stub_uid_has_package(int uid) {
    return stub_identity_query("live", uid, "", NULL, 0, NULL, 0);
}
int stub_verify_caller(int uid, const char* package, char* ref, size_t ref_size,
        char* remote, size_t remote_size, char* error, size_t error_size) {
    if (!package || uid < 10000 || uid % 100000 < 10000 || uid % 100000 > 19999 ||
            stub_identity_query("verify", uid, package, ref, ref_size, remote, remote_size) != uid) {
        set_error(error, error_size, "bridge could not verify stub identity");
        return -1;
    }
    return 0;
}
