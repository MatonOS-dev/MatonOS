#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Copies the desired state; all module and network work runs on a private loop. */
int maton_network_speaker(const char *command, const char *json);
#ifdef __cplusplus
}
#endif
