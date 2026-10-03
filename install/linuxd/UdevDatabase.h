#pragma once

#ifdef __cplusplus
extern "C" {
#endif
/* Initial snapshot is synchronous; subsequent refreshes run on one worker. */
void maton_udev_start(void);
#ifdef __cplusplus
}
#endif
