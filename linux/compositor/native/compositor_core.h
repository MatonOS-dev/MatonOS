#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "android_output.h"

#ifdef __cplusplus
extern "C" {
#endif

bool maton_core_start(const char* socket_name, const char* runtime_dir);
void maton_core_close(int id);
bool maton_core_add_session(int session, const char* socket_path);
void maton_core_stop(void);
void maton_core_launch_demo(void);
void maton_core_attach(int id, struct MatonSurfaceOutput* output, int width, int height);
void maton_core_detach(int id);
void maton_core_resize(int id, int width, int height);
void maton_core_key(int id, int keycode, int scan_code, int action, int meta_state, int64_t event_time_ns);
void maton_core_motion(int id, float x, float y, float vertical_scroll,
                       float horizontal_scroll, int action, int buttons, int64_t event_time_ns);

void maton_java_request_window(int session, int id, int width, int height);
void maton_java_close_window(int id);

#ifdef __cplusplus
}
#endif
