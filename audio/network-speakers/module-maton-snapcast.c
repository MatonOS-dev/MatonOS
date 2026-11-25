/* SPDX-License-Identifier: MIT
 * NSD-resolved counterpart of PipeWire 1.6.9 module-snapcast-discover.
 * Uses the same protocol-simple module and Snapcast JSON-RPC stream URI.
 * Unlike upstream discovery, handles short writes and reports RPC errors.
 */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pipewire/impl.h>
#include <spa/utils/json.h>
#define CLOSE(fd) do { pw_log_info("close fd=%d at %s:%d", (fd), __FILE__, __LINE__); \
    if (close(fd) < 0) pw_log_error("close fd=%d errno=%d (%s)", (fd), errno, strerror(errno)); } while (0)
struct sink {
    struct pw_impl_module *module, *pcm;
    struct pw_context *context;
    struct pw_loop *loop;
    struct pw_properties *props;
    struct spa_hook listener, pcm_listener;
    struct spa_source *io, *timeout;
    char *output, *input;
    size_t written, length, used;
    bool connecting;
};
static void destroy(void *data) {
    struct sink *s = data;
    spa_hook_remove(&s->listener);
    if (s->io) {
        int fd = s->io->fd;
        pw_loop_destroy_source(s->loop, s->io);
        CLOSE(fd);
    }
    if (s->timeout) pw_loop_destroy_source(s->loop, s->timeout);
    if (s->pcm) { spa_hook_remove(&s->pcm_listener); pw_impl_module_destroy(s->pcm); }
    pw_properties_free(s->props);
    free(s->output); free(s->input); free(s);
}
static const struct pw_impl_module_events events = {
    .version = PW_VERSION_IMPL_MODULE_EVENTS, .destroy = destroy,
};
static void pcm_destroy(void *data) {
    struct sink *s = data;
    spa_hook_remove(&s->pcm_listener); s->pcm = NULL;
    pw_log_error("Snapcast PCM module disappeared; recreating optional sink");
    pw_impl_module_schedule_destroy(s->module);
}
static const struct pw_impl_module_events pcm_events = {
    .version = PW_VERSION_IMPL_MODULE_EVENTS, .destroy = pcm_destroy,
};
static int create_pcm(struct sink *s, int fd) {
    struct sockaddr_storage addr;
    socklen_t length = sizeof(addr);
    char host[NI_MAXHOST];
    int res;
    if (getsockname(fd, (struct sockaddr *)&addr, &length) < 0) return -errno;
    res = getnameinfo((struct sockaddr *)&addr, length, host, sizeof(host), NULL, 0, NI_NUMERICHOST);
    if (res) { pw_log_error("Snapcast local address: %s", gai_strerror(res)); return -EINVAL; }
    pw_properties_setf(s->props, PW_KEY_NODE_NAME, "snapcast_sink.%s.%s",
        pw_properties_get(s->props, "snapcast.ip"), pw_properties_get(s->props, "snapcast.port"));
    pw_properties_setf(s->props, "server.address", "[ \"tcp:%s:0\" ]", host);
    pw_properties_set(s->props, "local.ifaddress", host);
    pw_properties_set(s->props, "capture", "true");
    pw_properties_set(s->props, "playback", "false");
    pw_properties_set(s->props, "capture.props", "{ media.class = Audio/Sink }");
    pw_properties_set(s->props, "audio.format", "S16LE");
    pw_properties_set(s->props, "audio.rate", "48000");
    pw_properties_set(s->props, "audio.channels", "2");
    pw_properties_set(s->props, "audio.position", "[ FL FR ]");
    char *args = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&args, &size);
    if (!f) return -errno;
    fputc('{', f); pw_properties_serialize_dict(f, &s->props->dict, 0); fputc('}', f);
    if (fclose(f) < 0) { res = -errno; free(args); return res; }
    s->pcm = pw_context_load_module(s->context, "libpipewire-module-protocol-simple", args, NULL);
    free(args);
    if (!s->pcm) return -errno;
    pw_impl_module_add_listener(s->pcm, &s->pcm_listener, &pcm_events, s);
    const struct pw_properties *props = pw_impl_module_get_properties(s->pcm);
    const char *servers = pw_properties_get(props, "server.address");
    if (!servers) return -ENOENT;
    struct spa_json it;
    char server[NI_MAXHOST + 32];
    if (spa_json_begin_array_relax(&it, servers, strlen(servers)) <= 0 ||
        spa_json_get_string(&it, server, sizeof(server)) <= 0) return -EINVAL;
    // Numeric resolved endpoint yields a stable, collision-free URI-safe stream name.
    const char *remote = pw_properties_get(s->props, "snapcast.ip");
    const char *port = pw_properties_get(s->props, "snapcast.port");
    if (asprintf(&s->output,
        "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"Server.GetRPCVersion\"}\r\n"
        "{\"id\":2,\"jsonrpc\":\"2.0\",\"method\":\"Stream.RemoveStream\",\"params\":{\"id\":\"MatonOS-%s-%s\"}}\r\n"
        "{\"id\":3,\"jsonrpc\":\"2.0\",\"method\":\"Stream.AddStream\",\"params\":{\"streamUri\":\"tcp://%s?name=MatonOS-%s-%s&mode=client&sampleformat=48000:16:2&codec=pcm&chunk_ms=20\"}}\r\n",
        remote, port, server, remote, port) < 0) return -ENOMEM;
    s->length = strlen(s->output);
    return 0;
}
static void expired(void *data, uint64_t count) {
    (void)count;
    struct sink *s = data;
    pw_log_error("Snapcast control connection/registration timeout fd=%d", s->io ? s->io->fd : -1);
    pw_impl_module_schedule_destroy(s->module);
}
static void io(void *data, int fd, uint32_t mask) {
    struct sink *s = data;
    int res = 0;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) { res = -EPIPE; goto fail; }
    if (s->connecting && (mask & SPA_IO_OUT)) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) { res = -errno; goto fail; }
        if (error) { res = -error; goto fail; }
        s->connecting = false;
        if ((res = create_pcm(s, fd)) < 0) goto fail;
    }
    if (mask & SPA_IO_OUT) {
        while (s->written < s->length) {
            ssize_t n = send(fd, s->output + s->written, s->length - s->written, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) break;
            if (n <= 0) { res = n ? -errno : -EPIPE; goto fail; }
            s->written += n;
        }
        if (s->written == s->length) {
            res = pw_loop_update_io(s->loop, s->io, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
            if (res < 0) goto fail;
        }
    }
    if (mask & SPA_IO_IN) {
        char chunk[4096];
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
        if (n <= 0) { res = n ? -errno : -EPIPE; goto fail; }
        char *input = realloc(s->input, s->used + (size_t)n + 1);
        if (!input) { res = -ENOMEM; goto fail; }
        s->input = input;
        memcpy(input + s->used, chunk, n); s->used += n; input[s->used] = 0;
        char *line;
        while ((line = strchr(s->input, '\n'))) {
            *line = 0;
            struct pw_properties *response = pw_properties_new_string(s->input);
            if (!response) { pw_log_error("Snapcast malformed RPC response fd=%d", fd); res = -EPROTO; goto fail; }
            const char *id = pw_properties_get(response, "id");
            const char *error = pw_properties_get(response, "error");
            bool registration = id && !strcmp(id, "3");
            // RemoveStream may report that the previous stream did not exist.
            if (error && (!id || strcmp(id, "2"))) pw_log_error("Snapcast RPC fd=%d response=%s", fd, s->input);
            bool failed = registration && error;
            pw_properties_free(response);
            if (failed) { res = -EPROTO; goto fail; }
            if (registration) {
                res = pw_loop_update_timer(s->loop, s->timeout, NULL, NULL, false);
                if (res < 0) goto fail;
            }
            size_t consumed = (size_t)(line - s->input) + 1;
            s->used -= consumed; memmove(s->input, line + 1, s->used + 1);
        }
    }
    return;
fail:
    pw_log_error("Snapcast optional sink failed fd=%d result=%d (%s)", fd, res, strerror(-res));
    pw_impl_module_schedule_destroy(s->module);
}
SPA_EXPORT int pipewire__module_init(struct pw_impl_module *module, const char *args) {
    struct sink *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    s->module = module; s->context = pw_impl_module_get_context(module);
    s->loop = pw_context_get_main_loop(s->context);
    s->props = pw_properties_new_string(args ? args : "{}");
    int res = -EINVAL, fd = -1;
    if (!s->props) goto fail;
    const char *host = pw_properties_get(s->props, "snapcast.ip");
    const char *port = pw_properties_get(s->props, "snapcast.port");
    if (!host || !port) goto fail;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM,
                             .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV }, *address;
    res = getaddrinfo(host, port, &hints, &address);
    if (res) { pw_log_error("Snapcast endpoint=%s:%s: %s", host, port, gai_strerror(res)); res = -EINVAL; goto fail; }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { res = -errno; freeaddrinfo(address); goto fail; }
    res = connect(fd, address->ai_addr, address->ai_addrlen);
    freeaddrinfo(address);
    if (res < 0 && errno != EINPROGRESS) { res = -errno; goto fail; }
    s->connecting = true;
    s->io = pw_loop_add_io(s->loop, fd, SPA_IO_OUT | SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false, io, s);
    if (!s->io) { res = -errno; goto fail; }
    s->timeout = pw_loop_add_timer(s->loop, expired, s);
    if (!s->timeout) { res = -errno; goto fail; }
    struct timespec delay = {10, 0};
    res = pw_loop_update_timer(s->loop, s->timeout, &delay, NULL, false);
    if (res < 0) goto fail;
    pw_impl_module_add_listener(module, &s->listener, &events, s);
    return 0;
fail:
    pw_log_error("Snapcast module init failed fd=%d result=%d", fd, res);
    if (s->io) pw_loop_destroy_source(s->loop, s->io);
    if (s->timeout) pw_loop_destroy_source(s->loop, s->timeout);
    if (fd >= 0) CLOSE(fd);
    if (s->props) pw_properties_free(s->props);
    free(s);
    return res;
}
