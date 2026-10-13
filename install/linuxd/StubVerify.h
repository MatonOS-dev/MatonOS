#ifndef MATONOS_STUB_VERIFY_H
#define MATONOS_STUB_VERIFY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Legacy signer handoff retained for older bridge callers; identity uses PM. */
int stub_store_signer(const unsigned char* der, size_t length);

/* 0 when the bridge's live PackageManager check verifies the generated stub,
 * its binder caller UID and pinned signer; fills ref and remote from its manifest. */
int stub_verify_caller(int uid, const char* package, char* ref, size_t ref_size,
        char* remote, size_t remote_size, char* error, size_t error_size);

/* 1 UID occupied (preserve reused/shared UIDs), 0 PM-confirmed absent, -1 unavailable. */
int stub_uid_has_package(int uid);
/* Generated stub installed for this exact Android user, 1/0/-1. */
int stub_user_has_package(int uid);

#ifdef __cplusplus
}
#endif

#endif
