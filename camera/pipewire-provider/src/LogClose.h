/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <errno.h>
#include <unistd.h>
#include <log/log.h>
static inline int maton_camera_close(int fd, const char* file, int line) {
    ALOGI("close fd=%d at %s:%d", fd, file, line);
    int result = close(fd);
    if (result < 0) ALOGE("close fd=%d errno=%d at %s:%d", fd, errno, file, line);
    return result;
}
#define MATON_CLOSE(fd) maton_camera_close(fd, __FILE__, __LINE__)
