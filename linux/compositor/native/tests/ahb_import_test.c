/* On-device check for minigbm's external dma-buf import (handles with id 0).
 *
 * Allocates an AHardwareBuffer, fills it, wraps duplicates of its dma-buf
 * fds in a cros_gralloc handle with id 0 and no reserved region, and imports
 * that with AHardwareBuffer_createFromHandle. Verifies the pixels, a second
 * import of the same dma-buf, and that the metadata path does not fail.
 * Build with the NDK (API 35) and run on the device as any user. */
#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DRV_MAX_PLANES 4
struct handle {
  int32_t version, numFds, numInts;
  int32_t fds[DRV_MAX_PLANES + 1];
  uint32_t strides[DRV_MAX_PLANES];
  uint32_t offsets[DRV_MAX_PLANES];
  uint32_t sizes[DRV_MAX_PLANES];
  uint32_t id, width, height, format, tiling;
  uint64_t format_modifier, use_flags;
  uint32_t magic, pixel_stride;
  int32_t droid_format;
  int64_t usage;
  uint32_t num_planes;
  uint64_t reserved_region_size, total_size;
} __attribute__((packed));

typedef const native_handle_t* (*GetNativeHandle)(const AHardwareBuffer*);
typedef int (*CreateFromHandle)(const AHardwareBuffer_Desc*, const native_handle_t*, int32_t, AHardwareBuffer**);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

/* Builds the external handle; the caller owns it and its duplicated fds. */
static struct handle* external_handle(const struct handle* source) {
  int ints = (int)((sizeof(struct handle) - sizeof(native_handle_t)) / sizeof(int)) - (int)source->num_planes;
  struct handle* h = malloc(sizeof(*h));
  if (!h) return NULL;
  memcpy(h, source, sizeof(*h));
  h->version = (int32_t)sizeof(native_handle_t);
  for (uint32_t i = 0; i < DRV_MAX_PLANES + 1; ++i) h->fds[i] = -1;
  for (uint32_t i = 0; i < source->num_planes; ++i) h->fds[i] = dup(source->fds[i]);
  h->numFds = (int32_t)source->num_planes;
  h->numInts = ints;
  h->id = 0;
  h->reserved_region_size = 0;
  return h;
}
static void free_handle(struct handle* h) {
  if (!h) return;
  for (uint32_t i = 0; i < h->num_planes; ++i) if (h->fds[i] >= 0) close(h->fds[i]);
  free(h);
}

int main(void) {
  void* lib = dlopen("libnativewindow.so", RTLD_NOW);
  GetNativeHandle get_handle = lib ? (GetNativeHandle)dlsym(lib, "AHardwareBuffer_getNativeHandle") : NULL;
  CreateFromHandle create = lib ? (CreateFromHandle)dlsym(lib, "AHardwareBuffer_createFromHandle") : NULL;
  if (!get_handle || !create) { printf("FAIL: libnativewindow handle API missing\n"); return 1; }

  AHardwareBuffer_Desc desc = {
    .width = 256, .height = 128, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
    .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
             AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
  };
  AHardwareBuffer* original = NULL;
  if (AHardwareBuffer_allocate(&desc, &original)) { printf("FAIL: allocate\n"); return 1; }
  AHardwareBuffer_Desc actual; AHardwareBuffer_describe(original, &actual);

  uint32_t* pixels = NULL;
  CHECK(AHardwareBuffer_lock(original, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, (void**)&pixels) == 0, "lock original");
  if (pixels) {
    for (uint32_t y = 0; y < desc.height; ++y)
      for (uint32_t x = 0; x < desc.width; ++x) pixels[y * actual.stride + x] = (y << 16) | x;
    AHardwareBuffer_unlock(original, NULL);
  }

  const struct handle* source = (const struct handle*)get_handle(original);
  CHECK(source && source->magic == 0xABCDDCBA, "original is not a cros_gralloc handle");
  if (!source || failures) return 1;
  printf("original: id=%u planes=%u format=0x%x modifier=0x%llx region=%llu\n", source->id,
         source->num_planes, source->format, (unsigned long long)source->format_modifier,
         (unsigned long long)source->reserved_region_size);

  struct handle* first = external_handle(source);
  struct handle* second = external_handle(source);
  AHardwareBuffer* imported = NULL; AHardwareBuffer* again = NULL;
  /* METHOD_CLONE (1): the import gets its own copy; we keep and free ours. */
  CHECK(first && create(&actual, (const native_handle_t*)first, 1, &imported) == 0 && imported, "import id-0 handle");
  CHECK(second && create(&actual, (const native_handle_t*)second, 1, &again) == 0 && again, "second import of the same dma-buf");
  free_handle(first); free_handle(second);

  if (imported) {
    uint32_t* seen = NULL;
    CHECK(AHardwareBuffer_lock(imported, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, (void**)&seen) == 0 && seen, "lock imported");
    if (seen) {
      int bad = 0;
      for (uint32_t y = 0; y < desc.height && !bad; ++y)
        for (uint32_t x = 0; x < desc.width; ++x)
          if (seen[y * actual.stride + x] != ((y << 16) | x)) { bad = 1; break; }
      CHECK(!bad, "imported pixels differ from the original");
      AHardwareBuffer_unlock(imported, NULL);
    }
    /* Exercises gralloc metadata (name, dataspace, ...) on the import. */
    AHardwareBuffer_Desc described; AHardwareBuffer_describe(imported, &described);
    CHECK(described.width == desc.width && described.height == desc.height, "describe imported");
  }
  if (again) AHardwareBuffer_release(again);
  if (imported) AHardwareBuffer_release(imported);
  AHardwareBuffer_release(original);
  printf(failures ? "%d FAILED\n" : "PASS\n", failures);
  return failures ? 1 : 0;
}
