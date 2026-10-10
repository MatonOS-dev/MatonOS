/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MATON_SYSVIPC_H
#define MATON_SYSVIPC_H
#include <stdint.h>
struct maton_sysvipc;
struct maton_sysvipc *maton_sysvipc_new(void);
/* One serving thread per app. It serves every listener of the app's sessions
 * (main sandbox, portal-spawned sub-sandboxes, relaunches) against one shared
 * object namespace. listener_fd (may be -1) is borrowed: it stays open and the
 * caller closes it after return. Returns 0 after maton_sysvipc_stop, -1 with
 * errno on transport failure. A listener whose tracees all exited is dropped. */
int maton_sysvipc_serve(struct maton_sysvipc *, int listener_fd);
/* Thread-safe: hand another session's listener to the serving thread, which
 * takes ownership (closes it when its tracees are gone or on free). */
int maton_sysvipc_add_listener(struct maton_sysvipc *, int listener_fd);
void maton_sysvipc_stop(struct maton_sysvipc *);
/* Call only after the serving thread has joined. */
void maton_sysvipc_free(struct maton_sysvipc *);
/* Helper handshake in otherwise unused shmat arguments. Initial shmat returns
 * an fd. Commit reports mmap's actual address; cancel releases the reservation.
 * These calls use the direct syscall on both ABIs, never ipc's output pointer. */
#define MATON_SHM_COMMIT UINT32_C(0x4d534843)
#define MATON_SHM_RESET UINT32_C(0x4d534852)
#define MATON_SHM_INHERIT UINT32_C(0x4d534849)
#define MATON_SHM_CANCEL UINT32_C(0x4d534858)
#endif
