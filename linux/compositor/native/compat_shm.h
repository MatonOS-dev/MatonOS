#pragma once
#include <sys/types.h>

int maton_shm_open(const char* name, int flags, mode_t mode);
int maton_shm_unlink(const char* name);
