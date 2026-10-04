#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Initial snapshot is synchronous; subsequent refreshes run on one worker. */
void maton_udev_start(void);
/* Synchronous serialized snapshot for pad creation/destruction. */
int maton_udev_refresh(void);
/* Called only after destroying a session-owned kernel device. */
void maton_udev_forget_pad(dev_t device);
#ifdef __cplusplus
}
#endif
