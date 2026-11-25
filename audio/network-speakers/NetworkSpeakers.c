/* SPDX-License-Identifier: MIT
 * HAL-side transport only. AOSP forbids network sockets in audio HAL domains.
 * The optional module in the existing PipeWire daemon owns network sinks.
 */
#include "NetworkSpeakers.h"
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#define ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "MatonNetworkAudio", __VA_ARGS__)
static time_t monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) { ERROR("clock_gettime errno=%d", errno); return 0; }
    return now.tv_sec;
}
struct desired { struct desired *next; char *id, *json; bool removed; time_t refreshed; };
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static struct desired *desired;
static struct pw_thread_loop *loop;
static struct pw_context *context;
static struct pw_core *core;
static struct pw_registry *registry;
static struct pw_metadata *metadata;
static struct spa_hook core_listener, registry_listener;
static struct spa_source *timer, *event;
static uint32_t metadata_id;
static uint64_t sequence;
static bool broken, reset;
static void global(void *data, uint32_t id, uint32_t perms, const char *type, uint32_t version, const struct spa_dict *props) {
    (void)data; (void)perms; (void)version;
    if (metadata || strcmp(type, PW_TYPE_INTERFACE_Metadata) || !props) return;
    const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
    if (!name || strcmp(name, "matonos-network-speakers")) return;
    metadata = pw_registry_bind(registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);
    if (!metadata) { ERROR("bind network metadata id=%u errno=%d", id, errno); return; }
    metadata_id = id;
}
static void global_remove(void *data, uint32_t id) {
    (void)data;
    if (metadata && metadata_id == id) {
        pw_proxy_destroy((struct pw_proxy *)metadata); metadata = NULL;
        ERROR("network speaker metadata removed id=%u; awaiting module restart", id);
    }
}
static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS, .global = global, .global_remove = global_remove,
};
static void error(void *data, uint32_t id, int seq, int res, const char *message) {
    (void)data; (void)seq;
    ERROR("network speaker PipeWire error id=%u result=%d detail=%s", id, res, message ? message : "");
    if (id == PW_ID_CORE && res < 0) broken = true;
}
static const struct pw_core_events core_events = { .version = PW_VERSION_CORE_EVENTS, .error = error };
static void publish(void *data, uint64_t count) {
    (void)data; (void)count;
    if (!core || broken) {
        if (metadata) { pw_proxy_destroy((struct pw_proxy *)metadata); metadata = NULL; }
        if (registry) { spa_hook_remove(&registry_listener); pw_proxy_destroy((struct pw_proxy *)registry); registry = NULL; }
        if (core) { spa_hook_remove(&core_listener); pw_core_disconnect(core); core = NULL; }
        core = pw_context_connect(context, NULL, 0);
        if (!core) { ERROR("network metadata connect failed errno=%d (%s)", errno, strerror(errno)); return; }
        pw_core_add_listener(core, &core_listener, &core_events, NULL);
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (!registry) { ERROR("network metadata registry errno=%d", errno); broken = true; return; }
        pw_registry_add_listener(registry, &registry_listener, &registry_events, NULL);
        broken = false;
    }
    if (!metadata) return;
    pthread_mutex_lock(&mutex);
    if (reset) {
        char value[96];
        snprintf(value, sizeof(value), "{\"_lease\":%llu}", (unsigned long long)++sequence);
        int res = pw_metadata_set_property(metadata, PW_ID_CORE, "_reset", "Spa:String:JSON", value);
        if (res < 0) ERROR("network metadata reset result=%d", res);
        else reset = false;
    }
    struct desired **p = &desired;
    while (*p) {
        struct desired *s = *p;
        if (monotonic_seconds() - s->refreshed > 15) s->removed = true;
        if (!s->json) { *p = s->next; free(s->id); free(s); continue; }
        struct pw_properties *props = pw_properties_new_string(s->json);
        if (!props) { ERROR("network desired parse id=%s errno=%d", s->id, errno); p = &s->next; continue; }
        pw_properties_setf(props, "_lease", "%llu", (unsigned long long)++sequence);
        if (s->removed) pw_properties_set(props, "_removed", "true");
        char *value = NULL; size_t size = 0;
        FILE *f = open_memstream(&value, &size);
        int res = -ENOMEM;
        if (f) {
            fputc('{', f); pw_properties_serialize_dict(f, &props->dict, 0); fputc('}', f);
            if (fclose(f) < 0) ERROR("network metadata serialization id=%s errno=%d", s->id, errno);
            else res = pw_metadata_set_property(metadata, PW_ID_CORE, s->id, "Spa:String:JSON", value);
        }
        if (res < 0) ERROR("network metadata publish id=%s result=%d", s->id, res);
        free(value); pw_properties_free(props);
        if (s->removed && res >= 0) {
            res = pw_metadata_set_property(metadata, PW_ID_CORE, s->id, NULL, NULL);
            if (res < 0) ERROR("network metadata tombstone cleanup id=%s result=%d", s->id, res);
            *p = s->next; free(s->id); free(s->json); free(s); }
        else p = &s->next;
    }
    pthread_mutex_unlock(&mutex);
}
static int start(void) {
    loop = pw_thread_loop_new("network-speakers", NULL);
    if (!loop) goto fail;
    context = pw_context_new(pw_thread_loop_get_loop(loop), NULL, 0);
    if (!context) goto fail;
    event = pw_loop_add_event(pw_thread_loop_get_loop(loop), publish, NULL);
    timer = pw_loop_add_timer(pw_thread_loop_get_loop(loop), publish, NULL);
    if (!event || !timer) goto fail;
    struct timespec delay = {3, 0};
    int res = pw_loop_update_timer(pw_thread_loop_get_loop(loop), timer, &delay, &delay, false);
    if (res < 0) { errno = -res; goto fail; }
    res = pw_thread_loop_start(loop);
    if (res < 0) { errno = -res; goto fail; }
    return 0;
fail:
    ERROR("network transport loop startup errno=%d (%s)", errno, strerror(errno));
    if (event) pw_loop_destroy_source(pw_thread_loop_get_loop(loop), event);
    if (timer) pw_loop_destroy_source(pw_thread_loop_get_loop(loop), timer);
    if (context) pw_context_destroy(context);
    if (loop) pw_thread_loop_destroy(loop);
    loop = NULL; context = NULL; event = NULL; timer = NULL;
    return -EIO;
}
int maton_network_speaker(const char *command, const char *json) {
    struct pw_properties *props = pw_properties_new_string(json);
    if (!props) { ERROR("invalid network request errno=%d", errno); return -EINVAL; }
    const char *id = pw_properties_get(props, "id"), *kind = pw_properties_get(props, "kind");
    bool clear = !strcmp(command, "network_reset"), remove = !strcmp(command, "network_remove");
    if (!clear && (!id || (!remove && (!kind || (strcmp(kind, "raop") && strcmp(kind, "snapcast")))))) {
        ERROR("invalid network command=%s", command); pw_properties_free(props); return -EINVAL;
    }
    pthread_mutex_lock(&mutex);
    int res = 0;
    if (!loop && (res = start()) < 0) goto done;
    struct desired *s;
    if (clear) {
        reset = true;
        while (desired) { s = desired; desired = s->next; free(s->id); free(s->json); free(s); }
    } else {
        for (s = desired; s && strcmp(s->id, id); s = s->next) { }
        if (!s) {
            s = calloc(1, sizeof(*s));
            if (s) s->id = strdup(id);
            if (!s || !s->id) { free(s); res = -ENOMEM; goto done; }
            s->next = desired; desired = s;
        }
        char *copy = strdup(json);
        if (!copy) { res = -ENOMEM; s->removed = true; goto done; }
        free(s->json); s->json = copy; s->removed = remove; s->refreshed = monotonic_seconds();
    }
    pw_loop_signal_event(pw_thread_loop_get_loop(loop), event);
done:
    if (res < 0) ERROR("network desired update command=%s result=%d", command, res);
    pthread_mutex_unlock(&mutex); pw_properties_free(props); return res;
}
