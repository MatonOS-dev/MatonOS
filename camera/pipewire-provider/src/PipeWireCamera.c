/* SPDX-License-Identifier: Apache-2.0
 * Private system PipeWire client. Copies frames before returning SPA buffers,
 * so Android requests and offline sessions never retain a server-owned buffer.
 */
#include "PipeWireCamera.h"
#include <android/log.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/param/video/mjpg-utils.h>
#include <spa/buffer/meta.h>
#define ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "MatonPipeWireCamera", __VA_ARGS__)
struct node {
    struct node *next;
    uint32_t global, id;
    char *identity;
    struct pw_node *proxy;
    struct spa_hook listener;
    struct pwcam_mode *modes;
    size_t count;
};
struct pwcam_handle {
    struct pwcam_handle *next;
    uint32_t global, id;
    struct pw_stream *stream;
    struct spa_hook listener;
    struct spa_video_info info;
    struct pwcam_mode *modes;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool dead, stopped, ready;
    struct pwcam_frame latest;
};
static pthread_mutex_t startup = PTHREAD_MUTEX_INITIALIZER;
static struct pw_thread_loop *loop;
static struct pw_context *context;
static struct pw_core *core;
static struct pw_registry *registry;
static struct spa_hook registry_hook, core_hook;
static struct spa_source *retry;
static struct node *nodes;
static struct pwcam_handle *handles;
static bool broken;
static uint32_t fourcc(const struct spa_video_info *info) {
    if (info->media_subtype == SPA_MEDIA_SUBTYPE_mjpg) return V4L2_PIX_FMT_MJPEG;
    if (info->media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        if (info->info.raw.format == SPA_VIDEO_FORMAT_YUY2) return V4L2_PIX_FMT_YUYV;
        if (info->info.raw.format == SPA_VIDEO_FORMAT_NV12) return V4L2_PIX_FMT_NV12;
    }
    return 0;
}
static void add_mode(struct node *n, const struct spa_video_info *info) {
    uint32_t format = fourcc(info);
    struct spa_rectangle size = info->media_subtype == SPA_MEDIA_SUBTYPE_mjpg ? info->info.mjpg.size : info->info.raw.size;
    struct spa_fraction rate = info->media_subtype == SPA_MEDIA_SUBTYPE_mjpg ? info->info.mjpg.framerate : info->info.raw.framerate;
    if (!format || !size.width || !size.height || !rate.num || !rate.denom || (size.width & 1) || (size.height & 1)) return;
    struct pwcam_mode mode = {size.width, size.height, format, rate.num, rate.denom};
    for (size_t i = 0; i < n->count; i++) if (!memcmp(&mode, &n->modes[i], sizeof(mode))) return;
    void *modes = realloc(n->modes, (n->count + 1) * sizeof(mode));
    if (!modes) { ERROR("mode allocation node=%u errno=%d", n->global, errno); return; }
    n->modes = modes; n->modes[n->count++] = mode;
}
static void param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *pod) {
    (void)seq; (void)index; (void)next;
    struct node *n = data;
    if (id != SPA_PARAM_EnumFormat || !pod) return;
    struct spa_video_info info = {0};
    if (spa_format_parse(pod, &info.media_type, &info.media_subtype) < 0 || info.media_type != SPA_MEDIA_TYPE_video) return;
    if (info.media_subtype == SPA_MEDIA_SUBTYPE_raw) {
        if (spa_format_video_raw_parse(pod, &info.info.raw) < 0) return;
    } else if (info.media_subtype == SPA_MEDIA_SUBTYPE_mjpg) {
        if (spa_format_video_mjpg_parse(pod, &info.info.mjpg) < 0) return;
    } else return; // No IR/depth/metadata-only formats.
    // Enumerated pods contain valid defaults; retain those and explicit enum choices.
    const struct spa_pod_prop *sizeprop = spa_pod_find_prop(pod, NULL, SPA_FORMAT_VIDEO_size);
    const struct spa_pod_prop *rateprop = spa_pod_find_prop(pod, NULL, SPA_FORMAT_VIDEO_framerate);
    const struct spa_pod_prop *fmtprop = spa_pod_find_prop(pod, NULL, SPA_FORMAT_VIDEO_format);
    uint32_t ns = 0, cs = 0, nr = 0, cr = 0, nf = 0, cf = 0;
    const struct spa_pod *sizes = sizeprop ? spa_pod_get_values(&sizeprop->value, &ns, &cs) : NULL;
    const struct spa_pod *rates = rateprop ? spa_pod_get_values(&rateprop->value, &nr, &cr) : NULL;
    const struct spa_pod *fmts = fmtprop ? spa_pod_get_values(&fmtprop->value, &nf, &cf) : NULL;
    if (!sizes || !rates) return;
    if (cs != SPA_CHOICE_Enum) ns = 1;
    if (cr != SPA_CHOICE_Enum) nr = 1;
    if (!fmts || cf != SPA_CHOICE_Enum) nf = 1;
    for (uint32_t f = 0; f < nf; f++) for (uint32_t i = 0; i < ns; i++) for (uint32_t j = 0; j < nr; j++) {
        struct spa_rectangle size = ((const struct spa_rectangle *)SPA_POD_BODY(sizes))[i];
        struct spa_fraction rate = ((const struct spa_fraction *)SPA_POD_BODY(rates))[j];
        if (info.media_subtype == SPA_MEDIA_SUBTYPE_mjpg) { info.info.mjpg.size = size; info.info.mjpg.framerate = rate; }
        else {
            info.info.raw.size = size; info.info.raw.framerate = rate;
            if (fmts) info.info.raw.format = ((const uint32_t *)SPA_POD_BODY(fmts))[f];
        }
        add_mode(n, &info);
    }
}
static const struct pw_node_events node_events = { .version = PW_VERSION_NODE_EVENTS, .param = param };
static void remove_node(uint32_t global) {
    struct node **p = &nodes;
    while (*p) {
        struct node *n = *p;
        if (n->global != global) { p = &n->next; continue; }
        for (struct pwcam_handle *h = handles; h; h = h->next) if (h->global == global) {
            pthread_mutex_lock(&h->mutex); h->dead = true;
            pthread_cond_broadcast(&h->condition); pthread_mutex_unlock(&h->mutex);
        }
        *p = n->next; spa_hook_remove(&n->listener); pw_proxy_destroy((struct pw_proxy *)n->proxy);
        free(n->identity); free(n->modes); free(n); return;
    }
}
static void global_remove(void *unused, uint32_t id) { (void)unused; remove_node(id); }
/* Read one sysfs attribute, newline stripped; empty on failure. */
static void read_attr(const char *dir, const char *name, char *out, size_t size) {
    char path[512]; out[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "re");
    if (!f) return;
    if (!fgets(out, (int)size, f)) out[0] = '\0';
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
}
/* Stable identity across replug and reboot: USB vendor:product:serial plus
 * interface and V4L2 index. PipeWire's node name embeds videoN and the port
 * path, which both change. NULL when the device reports no serial. */
static char *usb_identity(const char *object_path) {
    if (!object_path || strncmp(object_path, "v4l2:/dev/", 10)) return NULL;
    char node[64]; snprintf(node, sizeof(node), "%s", object_path + 10);
    node[strcspn(node, ":")] = '\0';
    char cls[128], vid[16], pid[16], serial[256], ifnum[16], index[16];
    snprintf(cls, sizeof(cls), "/sys/class/video4linux/%s", node);
    read_attr(cls, "device/../idVendor", vid, sizeof(vid));
    read_attr(cls, "device/../idProduct", pid, sizeof(pid));
    read_attr(cls, "device/../serial", serial, sizeof(serial));
    read_attr(cls, "device/bInterfaceNumber", ifnum, sizeof(ifnum));
    read_attr(cls, "index", index, sizeof(index));
    if (!vid[0] || !pid[0] || !serial[0]) return NULL;
    char *out = NULL;
    if (asprintf(&out, "usb:%s:%s:%s:%s:%s", vid, pid, serial, ifnum, index) < 0) return NULL;
    return out;
}
static void global(void *unused, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {
    (void)unused; (void)permissions; (void)version;
    if (!props || strcmp(type, PW_TYPE_INTERFACE_Node)) return;
    const char *media = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *identity = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!media || strcmp(media, "Video/Source") || !identity) return;
    if (description && (strcasestr(description, "infrared") || strcasestr(description, " IR ") || strcasestr(description, "IR Camera") || strcasestr(description, " Camera IR") ||
                       strcasestr(description, "metadata"))) return;
    struct node *n = calloc(1, sizeof(*n));
    if (!n) { ERROR("node allocation global=%u errno=%d", id, errno); return; }
    n->global = id; n->identity = usb_identity(spa_dict_lookup(props, PW_KEY_OBJECT_PATH));
    if (!n->identity) n->identity = strdup(identity);
    if (!n->identity) { ERROR("identity allocation global=%u", id); free(n); return; }
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)n->identity; *p; p++) hash = (hash ^ *p) * 16777619u;
    n->id = hash & 0x3fffffff; // Leave signed Camera2 offset headroom; not a device count limit.
    for (struct node *other = nodes; other; other = other->next) if (other->id == n->id) {
        ERROR("stable camera identity collision name=%s existing=%s", n->identity, other->identity);
        free(n->identity); free(n); return;
    }
    n->proxy = pw_registry_bind(registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    if (!n->identity || !n->proxy) {
        ERROR("bind camera node=%u errno=%d", id, errno);
        if (n->proxy) pw_proxy_destroy((struct pw_proxy *)n->proxy);
        free(n->identity); free(n); return;
    }
    pw_node_add_listener(n->proxy, &n->listener, &node_events, n);
    n->next = nodes; nodes = n;
    int res = pw_node_enum_params(n->proxy, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, NULL);
    if (res < 0) ERROR("enumerate camera node=%u result=%d", id, res);
}
static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS, .global = global, .global_remove = global_remove,
};
static void core_error(void *unused, uint32_t id, int seq, int res, const char *message) {
    (void)unused; (void)seq;
    ERROR("PipeWire camera core error id=%u result=%d detail=%s", id, res, message ? message : "");
    if (id == PW_ID_CORE && res < 0) broken = true;
}
static const struct pw_core_events core_events = { .version = PW_VERSION_CORE_EVENTS, .error = core_error };
static void reconnect(void *unused, uint64_t count) {
    (void)unused; (void)count;
    if (core && !broken) return;
    while (nodes) remove_node(nodes->global);
    if (registry) { spa_hook_remove(&registry_hook); pw_proxy_destroy((struct pw_proxy *)registry); registry = NULL; }
    if (core) { spa_hook_remove(&core_hook); pw_core_disconnect(core); core = NULL; }
    core = pw_context_connect(context, NULL, 0);
    if (!core) { ERROR("camera connect unavailable errno=%d (%s)", errno, strerror(errno)); return; }
    pw_core_add_listener(core, &core_hook, &core_events, NULL);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!registry) { ERROR("camera registry failed errno=%d", errno); broken = true; return; }
    pw_registry_add_listener(registry, &registry_hook, &registry_events, NULL);
    broken = false;
}
int pwcam_start(void) {
    pthread_mutex_lock(&startup);
    if (loop) { pthread_mutex_unlock(&startup); return 0; }
    pw_init(NULL, NULL);
    loop = pw_thread_loop_new("pipewire-camera", NULL);
    if (!loop) goto fail;
    context = pw_context_new(pw_thread_loop_get_loop(loop), NULL, 0);
    if (!context) goto fail;
    retry = pw_loop_add_timer(pw_thread_loop_get_loop(loop), reconnect, NULL);
    if (!retry) goto fail;
    struct timespec first = {0, 1}, interval = {2, 0};
    int res = pw_loop_update_timer(pw_thread_loop_get_loop(loop), retry, &first, &interval, false);
    if (res < 0) { errno = -res; goto fail; }
    res = pw_thread_loop_start(loop);
    if (res < 0) { errno = -res; goto fail; }
    pthread_mutex_unlock(&startup); return 0;
fail:
    ERROR("camera loop startup errno=%d (%s)", errno, strerror(errno));
    if (retry) pw_loop_destroy_source(pw_thread_loop_get_loop(loop), retry);
    if (context) pw_context_destroy(context);
    if (loop) pw_thread_loop_destroy(loop);
    retry = NULL; context = NULL; loop = NULL;
    pthread_mutex_unlock(&startup); return -EIO;
}
size_t pwcam_devices(struct pwcam_device **out) {
    *out = NULL;
    if (pwcam_start() < 0) return 0;
    pw_thread_loop_lock(loop);
    size_t count = 0;
    for (struct node *n = nodes; n; n = n->next) if (n->count) count++;
    if (count) *out = calloc(count, sizeof(**out));
    if (count && !*out) { ERROR("camera snapshot allocation errno=%d", errno); count = 0; }
    size_t index = 0;
    for (struct node *n = nodes; n && index < count; n = n->next) if (n->count) (*out)[index++] = (struct pwcam_device){n->id, n->global};
    pw_thread_loop_unlock(loop); return count;
}
struct pwcam_handle *pwcam_open(const char *path) {
    if (pwcam_start() < 0) return NULL;
    unsigned id; char trailing;
    if (sscanf(path, "/dev/video%u%c", &id, &trailing) != 1) { ERROR("invalid camera identity path=%s", path); return NULL; }
    pw_thread_loop_lock(loop);
    struct node *n;
    for (n = nodes; n; n = n->next) if (n->id == id && n->count) break;
    struct pwcam_handle *h = n ? calloc(1, sizeof(*h)) : NULL;
    if (h) {
        h->modes = malloc(n->count * sizeof(*h->modes));
        if (!h->modes) { free(h); h = NULL; }
        else {
            memcpy(h->modes, n->modes, n->count * sizeof(*h->modes)); h->count = n->count;
            h->global = n->global; h->id = id; h->stopped = true;
            pthread_mutex_init(&h->mutex, NULL); pthread_cond_init(&h->condition, NULL);
            h->next = handles; handles = h;
        }
    }
    if (!h) ERROR("camera open id=%u unavailable/allocation errno=%d", id, errno);
    pw_thread_loop_unlock(loop); return h;
}
size_t pwcam_modes(struct pwcam_handle *h, struct pwcam_mode **out) {
    *out = malloc(h->count * sizeof(**out));
    if (!*out) { ERROR("camera modes allocation id=%u errno=%d", h->id, errno); return 0; }
    memcpy(*out, h->modes, h->count * sizeof(**out)); return h->count;
}
static void state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    (void)old;
    struct pwcam_handle *h = data;
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        ERROR("camera stream id=%u state=%d error=%s", h->id, state, error ? error : "disconnected");
        pthread_mutex_lock(&h->mutex); h->dead = true;
        pthread_cond_broadcast(&h->condition); pthread_mutex_unlock(&h->mutex);
    }
}
static void format(void *data, uint32_t id, const struct spa_pod *pod) {
    struct pwcam_handle *h = data;
    if (id != SPA_PARAM_Format || !pod) return;
    memset(&h->info, 0, sizeof(h->info));
    if (spa_format_parse(pod, &h->info.media_type, &h->info.media_subtype) < 0) return;
    int res = h->info.media_subtype == SPA_MEDIA_SUBTYPE_mjpg ?
        spa_format_video_mjpg_parse(pod, &h->info.info.mjpg) : spa_format_video_raw_parse(pod, &h->info.info.raw);
    if (res < 0 || !fourcc(&h->info)) { ERROR("camera format negotiation id=%u result=%d", h->id, res); return; }
    h->ready = true;
}
static void process(void *data) {
    struct pwcam_handle *h = data;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(h->stream);
    if (!buffer) return;
    struct spa_buffer *b = buffer->buffer;
    uint32_t fmt = fourcc(&h->info);
    struct spa_rectangle size = fmt == V4L2_PIX_FMT_MJPEG ? h->info.info.mjpg.size : h->info.info.raw.size;
    if (!h->ready || !b->n_datas || !b->datas[0].data || !b->datas[0].chunk) goto done;
    struct spa_data *d = &b->datas[0];
    struct spa_chunk *c = d->chunk;
    size_t row = fmt == V4L2_PIX_FMT_YUYV ? size.width * 2u : size.width;
    size_t stride = c->stride > 0 ? (size_t)c->stride : row;
    size_t rows = fmt == V4L2_PIX_FMT_NV12 ? (size_t)size.height * 3u / 2u : size.height;
    if (c->stride < 0 || (row && rows > SIZE_MAX / row)) {
        ERROR("camera unsupported stride/size id=%u stride=%d", h->id, c->stride); goto done;
    }
    size_t bytes = fmt == V4L2_PIX_FMT_MJPEG ? c->size : row * rows;
    if (c->offset > d->maxsize || c->size > d->maxsize - c->offset ||
        (fmt != V4L2_PIX_FMT_MJPEG && (stride < row || rows == 0 ||
         stride * ((fmt == V4L2_PIX_FMT_NV12 && b->n_datas == 2 ? size.height : rows) - 1) + row > c->size))) {
        ERROR("camera malformed buffer id=%u format=%u offset=%u bytes=%u stride=%d", h->id, fmt, c->offset, c->size, c->stride); goto done;
    }
    if (row && rows > SIZE_MAX / row) { ERROR("camera frame size overflow id=%u", h->id); goto done; }
    void *copy = malloc(bytes);
    if (!copy) { ERROR("camera frame allocation id=%u bytes=%zu errno=%d", h->id, bytes, errno); goto done; }
    const unsigned char *src = (const unsigned char *)d->data + c->offset;
    if (fmt == V4L2_PIX_FMT_MJPEG) memcpy(copy, src, bytes);
    else if (fmt == V4L2_PIX_FMT_NV12 && b->n_datas == 2) {
        struct spa_data *uv = &b->datas[1];
        struct spa_chunk *chroma = uv->chunk;
        size_t uvstride = chroma && chroma->stride > 0 ? (size_t)chroma->stride : row;
        if (!uv->data || !chroma || chroma->offset > uv->maxsize ||
            chroma->size > uv->maxsize - chroma->offset || uvstride < row ||
            uvstride * (size.height / 2u - 1u) + row > chroma->size) {
            ERROR("camera invalid NV12 UV plane id=%u", h->id); free(copy); goto done;
        }
        for (size_t i = 0; i < size.height; i++) memcpy((unsigned char *)copy + i * row, src + i * stride, row);
        src = (const unsigned char *)uv->data + chroma->offset;
        for (size_t i = 0; i < size.height / 2u; i++)
            memcpy((unsigned char *)copy + (size.height + i) * row, src + i * uvstride, row);
    } else for (size_t i = 0; i < rows; i++) memcpy((unsigned char *)copy + i * row, src + i * stride, row);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t timestamp = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    struct spa_meta_header *header = spa_buffer_find_meta_data(b, SPA_META_Header, sizeof(*header));
    if (header && header->pts > 0) timestamp = header->pts;
    pthread_mutex_lock(&h->mutex);
    free(h->latest.data);
    h->latest = (struct pwcam_frame){copy, bytes, size.width, size.height, fmt, timestamp};
    pthread_cond_signal(&h->condition); pthread_mutex_unlock(&h->mutex);
done:
    { int res = pw_stream_queue_buffer(h->stream, buffer);
      if (res < 0) ERROR("camera queue id=%u result=%d", h->id, res); }
}
static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS, .state_changed = state, .param_changed = format, .process = process,
};
void pwcam_stop(struct pwcam_handle *h) {
    pw_thread_loop_lock(loop);
    if (h->stream) { spa_hook_remove(&h->listener); pw_stream_destroy(h->stream); h->stream = NULL; }
    pthread_mutex_lock(&h->mutex); h->stopped = true; h->ready = false;
    free(h->latest.data); memset(&h->latest, 0, sizeof(h->latest));
    pthread_cond_broadcast(&h->condition); pthread_mutex_unlock(&h->mutex);
    pw_thread_loop_unlock(loop);
}
int pwcam_configure(struct pwcam_handle *h, uint32_t width, uint32_t height, double fps) {
    pwcam_stop(h);
    const struct pwcam_mode *mode = NULL;
    for (size_t i = 0; i < h->count; i++) if (h->modes[i].width == width && h->modes[i].height == height &&
        (fps <= 0 || __builtin_fabs((double)h->modes[i].fps_num / h->modes[i].fps_denom - fps) < 0.1)) { mode = &h->modes[i]; break; }
    if (!mode) { ERROR("camera unsupported mode id=%u %ux%u fps=%f", h->id, width, height, fps); return -EINVAL; }
    pw_thread_loop_lock(loop);
    if (h->dead || !core || broken) { pw_thread_loop_unlock(loop); return -ENODEV; }
    h->stream = pw_stream_new(core, "MatonOS Camera", pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Camera", NULL));
    int res = -errno;
    if (!h->stream) goto fail;
    pw_stream_add_listener(h->stream, &h->listener, &stream_events, h);
    unsigned char storage[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    struct spa_video_info info = { .media_type = SPA_MEDIA_TYPE_video };
    if (mode->format == V4L2_PIX_FMT_MJPEG) {
        info.media_subtype = SPA_MEDIA_SUBTYPE_mjpg;
        info.info.mjpg.size = SPA_RECTANGLE(width, height);
        info.info.mjpg.framerate = SPA_FRACTION(mode->fps_num, mode->fps_denom);
    } else {
        info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
        info.info.raw.format = mode->format == V4L2_PIX_FMT_YUYV ? SPA_VIDEO_FORMAT_YUY2 : SPA_VIDEO_FORMAT_NV12;
        info.info.raw.size = SPA_RECTANGLE(width, height);
        info.info.raw.framerate = SPA_FRACTION(mode->fps_num, mode->fps_denom);
    }
    const struct spa_pod *pod = spa_format_video_build(&builder, SPA_PARAM_EnumFormat, &info);
    pthread_mutex_lock(&h->mutex); h->stopped = false; pthread_mutex_unlock(&h->mutex);
    res = pw_stream_connect(h->stream, PW_DIRECTION_INPUT, h->global,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT, &pod, 1);
    if (res < 0) goto fail;
    pw_thread_loop_unlock(loop); return 0;
fail:
    ERROR("camera configure id=%u result=%d", h->id, res);
    pw_thread_loop_unlock(loop); pwcam_stop(h); return res;
}
int pwcam_capture(struct pwcam_handle *h, struct pwcam_frame *frame) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 2;
    pthread_mutex_lock(&h->mutex);
    int res = 0;
    while (!h->latest.data && !h->dead && !h->stopped && !res)
        res = pthread_cond_timedwait(&h->condition, &h->mutex, &deadline);
    if (h->dead || h->stopped) res = ENODEV;
    if (!res) { *frame = h->latest; memset(&h->latest, 0, sizeof(h->latest)); }
    pthread_mutex_unlock(&h->mutex);
    if (res) ERROR("camera capture id=%u failed result=%d (%s)", h->id, res, strerror(res));
    return -res;
}
void pwcam_close(struct pwcam_handle *h) {
    if (!h) return;
    pwcam_stop(h);
    pw_thread_loop_lock(loop);
    struct pwcam_handle **p = &handles;
    while (*p && *p != h) p = &(*p)->next;
    if (*p) *p = h->next;
    pw_thread_loop_unlock(loop);
    pthread_cond_destroy(&h->condition); pthread_mutex_destroy(&h->mutex);
    free(h->modes); free(h);
}
