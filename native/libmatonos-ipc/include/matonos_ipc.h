#ifndef MATONOS_IPC_H
#define MATONOS_IPC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define MATONOS_IPC_API __attribute__((visibility("default")))
#else
#define MATONOS_IPC_API
#endif

typedef struct MatonosIpcServer MatonosIpcServer;
typedef int (*matonos_ipc_handler)(const char *json_args, char *json_result,
                                    size_t result_capacity, void *context);

/* target is the VINTF-declared vendor.matonos.channel instance suffix.
 * The caller owns the returned server and may destroy it after stopping its
 * own publish/callback activity. Calls and publishes may run concurrently.
 * Destroy returns 0 when freed. For a started server it clears handlers and
 * listeners and returns -1: Binder's stable NDK API cannot unregister the
 * published instance, so the inert server must live until process exit.
 */
MATONOS_IPC_API MatonosIpcServer *matonos_ipc_create(const char *target);
MATONOS_IPC_API int matonos_ipc_destroy(MatonosIpcServer *server);
MATONOS_IPC_API int matonos_ipc_register(MatonosIpcServer *server, const char *command,
                         matonos_ipc_handler handler, void *context);
MATONOS_IPC_API int matonos_ipc_start(MatonosIpcServer *server);
MATONOS_IPC_API int matonos_ipc_publish(MatonosIpcServer *server, const char *topic,
                        const char *json_event);

#ifdef __cplusplus
}
#endif
#endif
