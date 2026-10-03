#include "dmabuf_import.h"
#include "gralloc_handle.h"

#include <android/log.h>
#include <dlfcn.h>
#include <drm/drm_fourcc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/addon.h>

/* Client dma-bufs reach SurfaceFlinger as AHardwareBuffers. gralloc can only
 * import its own handle type, so the dma-buf is wrapped in a cros_gralloc
 * handle with id 0: the MatonOS minigbm fork treats id 0 as an external
 * buffer, keyed by the dma-buf's identity, with process-local metadata. */

/* vndk/hardware_buffer.h and minigbm drv.h / gralloc usage bits. */
#define CREATE_FROM_HANDLE_METHOD_CLONE 3
#define BO_USE_TEXTURE (1ull << 5)
#define GRALLOC_USAGE_HW_TEXTURE 0x00000100
#define GRALLOC_USAGE_HW_COMPOSER 0x00000800

typedef int (*CreateFromHandle)(const AHardwareBuffer_Desc*, const native_handle_t*, int32_t, AHardwareBuffer**);
static CreateFromHandle create_from_handle;
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static void resolve(void) {
  void* lib = dlopen("libnativewindow.so", RTLD_NOW);
  if (lib) create_from_handle = (CreateFromHandle)dlsym(lib, "AHardwareBuffer_createFromHandle");
}

struct Format { uint32_t drm; uint32_t ahb; uint32_t bytes; bool opaque; };
/* DRM fourccs name channels from the most significant bit: ABGR8888 is
 * R,G,B,A in memory, Android's RGBA_8888. */
static const struct Format formats[] = {
  { DRM_FORMAT_ABGR8888, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 4, false },
  { DRM_FORMAT_XBGR8888, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 4, true },
  { DRM_FORMAT_ARGB8888, 5 /* HAL_PIXEL_FORMAT_BGRA_8888 */, 4, false },
  { DRM_FORMAT_XRGB8888, 5 /* BGRA, alpha ignored via opaque */, 4, true },
  { DRM_FORMAT_RGB565, AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, 2, true },
};
static const struct Format* find_format(uint32_t drm) {
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
    if (formats[i].drm == drm) return &formats[i];
  return NULL;
}
bool maton_dmabuf_format_supported(uint32_t drm_format) { return find_format(drm_format) != NULL; }
bool maton_dmabuf_format_opaque(uint32_t drm_format) {
  const struct Format* f = find_format(drm_format);
  return f && f->opaque;
}

static AHardwareBuffer* import(const struct wlr_dmabuf_attributes* d) {
  pthread_once(&resolve_once, resolve);
  const struct Format* f = find_format(d->format);
  if (!create_from_handle || !f || d->n_planes != 1 || d->width <= 0 || d->height <= 0 ||
      d->stride[0] == 0 || d->stride[0] % f->bytes) return NULL;
  struct maton_cros_gralloc_handle h;
  memset(&h, 0, sizeof(h));
  h.version = (int32_t)sizeof(native_handle_t);
  h.numFds = 1;
  h.numInts = (int32_t)((sizeof(h) - sizeof(native_handle_t)) / sizeof(int)) - h.numFds;
  for (int i = 0; i < MATON_DRV_MAX_PLANES + 1; ++i) h.fds[i] = -1;
  h.fds[0] = dup(d->fd[0]);
  if (h.fds[0] < 0) return NULL;
  h.strides[0] = d->stride[0];
  h.offsets[0] = d->offset[0];
  h.sizes[0] = d->stride[0] * (uint32_t)d->height;
  h.id = 0;
  h.width = (uint32_t)d->width;
  h.height = (uint32_t)d->height;
  h.format = d->format;
  h.format_modifier = d->modifier;
  h.use_flags = BO_USE_TEXTURE;
  h.magic = MATON_CROS_GRALLOC_MAGIC;
  h.pixel_stride = d->stride[0] / f->bytes;
  h.droid_format = (int32_t)f->ahb;
  h.usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_COMPOSER;
  h.num_planes = 1;
  h.total_size = h.offsets[0] + h.sizes[0];
  AHardwareBuffer_Desc desc = {
    .width = h.width, .height = h.height, .layers = 1, .format = f->ahb,
    .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
    .stride = h.pixel_stride,
  };
  AHardwareBuffer* ahb = NULL;
  int rc = create_from_handle(&desc, (const native_handle_t*)&h, CREATE_FROM_HANDLE_METHOD_CLONE, &ahb);
  close(h.fds[0]);
  if (rc != 0) {
    __android_log_print(ANDROID_LOG_WARN, "MatonCompositor",
        "dma-buf import failed (rc=%d, %dx%d format 0x%x modifier 0x%llx stride %u)", rc,
        d->width, d->height, d->format, (unsigned long long)d->modifier, d->stride[0]);
    return NULL;
  }
  return ahb;
}

bool maton_dmabuf_importable(const struct wlr_dmabuf_attributes* attrs) {
  AHardwareBuffer* ahb = attrs ? import(attrs) : NULL;
  if (!ahb) return false;
  AHardwareBuffer_release(ahb);
  return true;
}

struct Imported { struct wlr_addon addon; AHardwareBuffer* ahb; };
static void imported_destroy(struct wlr_addon* addon) {
  struct Imported* imp = (struct Imported*)addon;
  if (imp->ahb) AHardwareBuffer_release(imp->ahb);
  wlr_addon_finish(addon);
  free(imp);
}
static const struct wlr_addon_interface imported_impl = {
  .name = "maton_dmabuf_ahb",
  .destroy = imported_destroy,
};

AHardwareBuffer* maton_dmabuf_ahb(struct wlr_buffer* buffer) {
  if (!buffer) return NULL;
  struct wlr_addon* found = wlr_addon_find(&buffer->addons, NULL, &imported_impl);
  if (found) return ((struct Imported*)found)->ahb;
  struct wlr_dmabuf_attributes attrs;
  if (!wlr_buffer_get_dmabuf(buffer, &attrs)) return NULL;
  struct Imported* imp = calloc(1, sizeof(*imp));
  if (!imp) return NULL;
  /* A failed import is cached too (ahb NULL), so it is not retried per frame. */
  imp->ahb = import(&attrs);
  wlr_addon_init(&imp->addon, &buffer->addons, NULL, &imported_impl);
  return imp->ahb;
}
