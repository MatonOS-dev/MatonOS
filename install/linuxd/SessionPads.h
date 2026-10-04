#pragma once
#include <stddef.h>
#include <stdint.h>
#include <linux/input.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MATON_PAD_LIMIT 4
#define MATON_PAD_AXES 8
/* Codes/ranges are Linux evdev units; the future relay normalizes Android axes.
 * Only the eight Xbox axes are accepted. Omitted axes use Xbox defaults. */
typedef struct { unsigned code; struct input_absinfo info; } MatonPadAxis;
typedef struct {
    char descriptor[128];
    int rumble;
    size_t axis_count;
    MatonPadAxis axes[MATON_PAD_AXES];
} MatonPadInventory;
typedef struct MatonSessionPads MatonSessionPads;
typedef void (*MatonPadRumble)(void *context, unsigned slot, uint16_t strong, uint16_t weak);
/* Caller serializes send/nodes/destroy. Callback runs on the FF worker; it
 * must not destroy/reenter the session. NULL callback disables FF. */
MatonSessionPads *maton_pads_create(const MatonPadInventory *pads, size_t count, unsigned owner_uid,
        MatonPadRumble rumble, void *context);
int maton_pads_send(MatonSessionPads *session, unsigned slot,
        const struct input_event *events, size_t count);
int maton_pads_release(MatonSessionPads *session, unsigned slot);
/* Comma-separated absolute event paths, owned by session, valid until destroy. */
const char *maton_pads_nodes(const MatonSessionPads *session);
void maton_pads_destroy(MatonSessionPads *session);
#ifdef __cplusplus
}
#endif
