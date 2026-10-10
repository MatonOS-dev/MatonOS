#ifndef MATONOS_STUB_VERIFY_H
#define MATONOS_STUB_VERIFY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Store the per-device stub signing certificate (DER) the bridge hands over. */
int stub_store_signer(const unsigned char* der, size_t length);

/* 0 when package is a generated stub owned by uid (binder caller), signed
 * with the stored stub certificate; fills ref and remote from its manifest. */
int stub_verify_caller(int uid, const char* package, char* ref, size_t ref_size,
        char* remote, size_t remote_size, char* error, size_t error_size);

/* UID of org.matonos.linuxruntimes in uid's Android user, or -1. */
int stub_runtime_uid(int uid);
/* 1 alive, 0 absent, -1 ownership scan unavailable (e.g. locked user). */
int stub_uid_has_package(int uid);

#ifdef __cplusplus
}
#endif

#endif
