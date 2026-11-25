/* SPDX-License-Identifier: MIT
 * NSD transport for PipeWire 1.6.9 sink modules. No multicast or Avahi here.
 */
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>
#include <pipewire/impl.h>
#define ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "MatonNetworkAudio", __VA_ARGS__)
static time_t monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) { ERROR("clock_gettime errno=%d", errno); return 0; }
    return now.tv_sec;
}
struct speaker {
    struct speaker *next;
    char *id, *args, *kind;
    struct pw_impl_module *module;
    struct spa_hook listener;
    bool changed, removed;
    time_t refreshed;
};
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static struct speaker *speakers;
static struct pw_loop *loop;
static struct pw_impl_metadata *metadata;
static struct spa_hook metadata_listener, module_listener;
static struct pw_context *context;
static struct spa_source *event, *timer;
static void destroyed(void *data) {
    struct speaker *s = data;
    spa_hook_remove(&s->listener);
    s->module = NULL;
    ERROR("sink module destroyed id=%s; retrying while advertised", s->id);
}
static const struct pw_impl_module_events events = {
    .version = PW_VERSION_IMPL_MODULE_EVENTS, .destroy = destroyed,
};
static void reconcile(void *unused, uint64_t count) {
    (void)unused; (void)count;
    pthread_mutex_lock(&mutex);
    struct speaker **p = &speakers;
    while (*p) {
        struct speaker *s = *p;
        if (monotonic_seconds() - s->refreshed > 15) s->removed = true;
        if ((s->changed || s->removed) && s->module) {
            spa_hook_remove(&s->listener);
            pw_impl_module_destroy(s->module);
            s->module = NULL;
        }
        if (s->removed) {
            *p = s->next;
            free(s->id); free(s->args); free(s->kind); free(s);
            continue;
        }
        s->changed = false;
        if (!s->module) {
            const char *name = strcmp(s->kind, "raop") == 0 ?
                "libpipewire-module-raop-sink" : "libpipewire-module-maton-snapcast";
            s->module = pw_context_load_module(context, name, s->args, NULL);
            if (!s->module) ERROR("optional module=%s id=%s load failed errno=%d (%s)",
                                 name, s->id, errno, strerror(errno));
            else pw_impl_module_add_listener(s->module, &s->listener, &events, s);
        }
        p = &s->next;
    }
    pthread_mutex_unlock(&mutex);
}
static void wake(void *data, uint64_t count) { reconcile(data, count); }
static int update(const char *command, const char *json) {
    struct pw_properties *props = pw_properties_new_string(json);
    if (!props) { ERROR("cannot parse network speaker request errno=%d", errno); return -EINVAL; }
    pw_properties_set(props, "_lease", NULL);
    char *canonical = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&canonical, &size);
    if (!f) { pw_properties_free(props); ERROR("network serialization failed errno=%d", errno); return -ENOMEM; }
    fputc('{', f); pw_properties_serialize_dict(f, &props->dict, 0); fputc('}', f);
    if (fclose(f) < 0) { free(canonical); pw_properties_free(props); ERROR("network serialization close errno=%d", errno); return -EIO; }
    json = canonical;
    const char *id = pw_properties_get(props, "id");
    const char *kind = pw_properties_get(props, "kind");
    bool reset = strcmp(command, "network_reset") == 0;
    bool remove = strcmp(command, "network_remove") == 0 || pw_properties_get_bool(props, "_removed", false);
    if (!reset && (!id || (!remove && (!kind ||
            (strcmp(kind, "raop") && strcmp(kind, "snapcast")))))) {
        ERROR("invalid network speaker command=%s", command);
        free(canonical); pw_properties_free(props); return -EINVAL;
    }
    pthread_mutex_lock(&mutex);
    struct speaker *s;
    for (s = speakers; s; s = s->next) {
        if (reset) s->removed = true;
        else if (!strcmp(s->id, id)) break;
    }
    if (!reset && !remove) {
        if (!s) {
            s = calloc(1, sizeof(*s));
            if (s) s->id = strdup(id);
            if (!s || !s->id) { free(s); goto nomem; }
            s->next = speakers; speakers = s;
        }
        if (!s->args || strcmp(s->args, json)) {
            char *args = strdup(json), *type = strdup(kind);
            if (!args || !type) { free(args); free(type); goto nomem; }
            free(s->args); free(s->kind); s->args = args; s->kind = type; s->changed = true;
        }
        s->removed = false;
        s->refreshed = monotonic_seconds();
    } else if (s && remove) s->removed = true;
    pw_loop_signal_event(loop, event);
    pthread_mutex_unlock(&mutex);
    free(canonical); pw_properties_free(props);
    return 0;
nomem:
    ERROR("network speaker allocation failed errno=%d", errno);
    /* Drop a newly allocated incomplete record before the loop sees it. */
    if (s && !s->args) s->removed = true;
    pw_loop_signal_event(loop, event);
    pthread_mutex_unlock(&mutex); free(canonical); pw_properties_free(props); return -ENOMEM;
}

static int property(void *data, uint32_t subject, const char *key, const char *type, const char *value) {
    (void)data; (void)subject; (void)type;
    if (!key || !value) return 0;
    int res = update(!strcmp(key, "_reset") ? "network_reset" : "network_upsert", value);
    if (res < 0) ERROR("network metadata update key=%s result=%d", key, res);
    return 0;
}
static void cleanup(void *data) {
    (void)data;
    spa_hook_remove(&module_listener);
    if (metadata) { spa_hook_remove(&metadata_listener); pw_impl_metadata_destroy(metadata); metadata = NULL; }
    if (timer) pw_loop_destroy_source(loop, timer);
    if (event) pw_loop_destroy_source(loop, event);
    for (struct speaker *s = speakers; s; s = s->next) s->removed = true;
    reconcile(NULL, 0);
}
SPA_EXPORT int pipewire__module_init(struct pw_impl_module *module, const char *args) {
    (void)args;
    context = pw_impl_module_get_context(module);
    loop = pw_context_get_main_loop(context);
    event = pw_loop_add_event(loop, wake, NULL);
    timer = pw_loop_add_timer(loop, reconcile, NULL);
    if (!event || !timer) goto fail;
    struct timespec delay = {3, 0};
    int res = pw_loop_update_timer(loop, timer, &delay, &delay, false);
    if (res < 0) { errno = -res; goto fail; }
    metadata = pw_context_create_metadata(context, "matonos-network-speakers", NULL, 0);
    if (!metadata) goto fail;
    static const struct pw_impl_metadata_events metadata_events = {
        .version = PW_VERSION_IMPL_METADATA_EVENTS, .property = property,
    };
    pw_impl_metadata_add_listener(metadata, &metadata_listener, &metadata_events, NULL);
    res = pw_impl_metadata_register(metadata, NULL);
    if (res < 0) { errno = -res; goto fail; }
    static const struct pw_impl_module_events module_events = {
        .version = PW_VERSION_IMPL_MODULE_EVENTS, .destroy = cleanup,
    };
    pw_impl_module_add_listener(module, &module_listener, &module_events, NULL);
    return 0;
fail:
    ERROR("network module startup failed errno=%d (%s)", errno, strerror(errno));
    if (metadata) { spa_hook_remove(&metadata_listener); pw_impl_metadata_destroy(metadata); metadata = NULL; }
    if (timer) pw_loop_destroy_source(loop, timer);
    if (event) pw_loop_destroy_source(loop, event);
    return -EIO;
}
