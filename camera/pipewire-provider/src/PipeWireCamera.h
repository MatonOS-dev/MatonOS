/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct pwcam_handle;
struct pwcam_mode { uint32_t width, height, format, fps_num, fps_denom; };
struct pwcam_device { uint32_t id, generation; };
struct pwcam_frame { void *data; size_t size; uint32_t width, height, format; int64_t timestamp; };
int pwcam_start(void);
size_t pwcam_devices(struct pwcam_device **devices);
struct pwcam_handle *pwcam_open(const char *path);
void pwcam_close(struct pwcam_handle *handle);
size_t pwcam_modes(struct pwcam_handle *handle, struct pwcam_mode **modes);
int pwcam_configure(struct pwcam_handle *handle, uint32_t width, uint32_t height, double fps);
void pwcam_stop(struct pwcam_handle *handle);
int pwcam_capture(struct pwcam_handle *handle, struct pwcam_frame *frame);
#ifdef __cplusplus
}
#include <memory>
using CameraHandle = std::shared_ptr<pwcam_handle>;
#endif
