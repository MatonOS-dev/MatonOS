// Prints the GPU contexts a virtio-gpu render node offers, one per line:
//   native:<amdgpu|msm|i915|asahi|panfrost>  DRM native context
//   venus                                    Vulkan passthrough
// pc-gpu-detect.sh picks the Vulkan HAL from this instead of a PCI-ID guess.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <drm/virtgpu_drm.h>

#define CAPSET_VENUS 4
#define CAPSET_DRM 6

struct capset_drm {  // virglrenderer struct virgl_renderer_capset_drm prefix
    uint32_t wire_format_version, version_major, version_minor,
             version_patchlevel, context_type, pad;
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) return 1;
    uint64_t ids = 0;
    struct drm_virtgpu_getparam gp = {
        .param = VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs, .value = (uintptr_t)&ids};
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp)) return 1;
    if (ids & (1ull << CAPSET_DRM)) {
        struct capset_drm caps = {0};
        struct drm_virtgpu_get_caps gc = {.cap_set_id = CAPSET_DRM,
            .addr = (uintptr_t)&caps, .size = sizeof(caps)};
        static const char *names[] = {"", "msm", "amdgpu", "i915", "asahi", "panfrost"};
        if (!ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &gc) && caps.context_type &&
            caps.context_type < sizeof(names) / sizeof(names[0]))
            printf("native:%s\n", names[caps.context_type]);
    }
    if (ids & (1ull << CAPSET_VENUS)) printf("venus\n");
    return 0;
}
