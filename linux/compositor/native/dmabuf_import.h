#pragma once
#include <android/hardware_buffer.h>
#include <stdbool.h>
#include <stdint.h>

struct wlr_buffer;

/* Formats a client dma-buf may use to be shown without a copy. */
bool maton_dmabuf_format_supported(uint32_t drm_format);
/* True if the format has no alpha channel (SurfaceFlinger must treat the
 * layer as opaque; the X byte is undefined). */
bool maton_dmabuf_format_opaque(uint32_t drm_format);
/* The AHardwareBuffer wrapping a client's dma-buf, imported on first use and
 * cached for the buffer's lifetime (borrowed). NULL for non-dma-buf buffers
 * and buffers gralloc cannot import. */
AHardwareBuffer* maton_dmabuf_ahb(struct wlr_buffer* buffer);
