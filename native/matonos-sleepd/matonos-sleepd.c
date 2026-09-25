/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * matonos-sleepd: MatonOS owns system sleep; Android is kept out of it.
 *
 * Android never suspends (config_useAutoSuspend=false) and never turns the
 * screen off (screen-off timeout "never", short power press ignored). This
 * daemon decides when the PC sleeps and suspends the kernel itself:
 *
 *   - after persist.vendor.maton.sleep_idle_s seconds without input from any
 *     /dev/input device (default 900; 0 = never),
 *   - on a short press of the power button (a long press stays Android's
 *     power menu),
 *   - when a laptop lid closes.
 *
 * Suspending is a write of "mem" to /sys/power/state (s2idle on PCs), which
 * returns once the kernel resumes; power/pc-wakeup.sh has made keyboards and
 * mice wake sources. Android never saw the screen go off, so the display
 * simply carries on; the wake-up key or click reaches Android as normal input.
 *
 * Input devices are only read, never grabbed, so Android sees every event.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <sys/system_properties.h>
#include <stdatomic.h>
#include <matonos_ipc.h>

#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#undef LOG_TAG
#define LOG_TAG "matonos-sleepd"

#define INPUT_DIR "/dev/input"
#define POWER_STATE "/sys/power/state"
#define IDLE_PROP "persist.vendor.maton.sleep_idle_s"
#define DEFAULT_IDLE_S 900
#define SHORT_PRESS_MS 800
#define RETRY_MS 30000
/* The key that woke the PC (e.g. the power button) is delivered after resume. */
#define RESUME_GRACE_MS 2000
#define MAX_DEVICES 64

struct device {
    int fd;
    char name[64]; /* eventN */
};

static struct device devices[MAX_DEVICES];
static int epfd;
static int inotify_fd;
static MatonosIpcServer *ipc_server;
static _Atomic bool is_sleeping;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long idle_limit_ms(void) {
    char value[PROP_VALUE_MAX];
    int s = __system_property_get(IDLE_PROP, value) > 0
                ? (int)strtol(value, NULL, 10) : DEFAULT_IDLE_S;
    return s > 0 ? (long long)s * 1000 : -1;
}

static void add_device(const char *name) {
    if (strncmp(name, "event", 5) != 0) return;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].fd > 0 && strcmp(devices[i].name, name) == 0) return;
    }
    char path[128];
    snprintf(path, sizeof(path), INPUT_DIR "/%s", name);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        ALOGW("open %s: %s", path, strerror(errno));
        return;
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].fd <= 0) {
            devices[i].fd = fd;
            snprintf(devices[i].name, sizeof(devices[i].name), "%s", name);
            struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
            epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
            return;
        }
    }
    ALOGW("too many input devices, ignoring %s", name);
    close(fd);
}

static void remove_device(int i) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, devices[i].fd, NULL);
    close(devices[i].fd);
    devices[i].fd = 0;
    devices[i].name[0] = '\0';
}

static void scan_devices(void) {
    DIR *dir = opendir(INPUT_DIR);
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) add_device(de->d_name);
    closedir(dir);
}

static void handle_inotify(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(inotify_fd, buf, sizeof(buf));
    for (char *p = buf; len > 0 && p < buf + len;) {
        struct inotify_event *ev = (struct inotify_event *)p;
        if ((ev->mask & IN_CREATE) && ev->len) add_device(ev->name);
        p += sizeof(*ev) + ev->len;
    }
}

static void publish_state(void) {
    if (!ipc_server) return;
    char state[256];
    char prop[PROP_VALUE_MAX];
    int timeout = __system_property_get(IDLE_PROP, prop) > 0 ? (int)strtol(prop, NULL, 10) : DEFAULT_IDLE_S;
    int power_fd = open(POWER_STATE, O_RDONLY | O_CLOEXEC);
    char power_states[256] = {0};
    ssize_t power_len = power_fd >= 0 ? read(power_fd, power_states, sizeof(power_states) - 1) : -1;
    if (power_fd >= 0) close(power_fd);
    bool supported = power_len > 0 && strstr(power_states, "mem") != NULL;
    snprintf(state, sizeof(state), "{\"idleTimeoutSeconds\":%d,\"suspendSupported\":%s,\"sleeping\":%s}",
             timeout > 0 ? timeout : 0, supported ? "true" : "false", is_sleeping ? "true" : "false");
    matonos_ipc_publish(ipc_server, "state", state);
}

static int handle_get_state(const char *args, char *result, size_t capacity, void *context) {
    (void)args; (void)context;
    if (capacity < 2) return -1;
    publish_state();
    char prop[PROP_VALUE_MAX];
    int timeout = __system_property_get(IDLE_PROP, prop) > 0 ? (int)strtol(prop, NULL, 10) : DEFAULT_IDLE_S;
    int power_fd = open(POWER_STATE, O_RDONLY | O_CLOEXEC);
    char power_states[256] = {0};
    ssize_t power_len = power_fd >= 0 ? read(power_fd, power_states, sizeof(power_states) - 1) : -1;
    if (power_fd >= 0) close(power_fd);
    bool supported = power_len > 0 && strstr(power_states, "mem") != NULL;
    snprintf(result, capacity, "{\"idleTimeoutSeconds\":%d,\"suspendSupported\":%s,\"sleeping\":%s}",
             timeout > 0 ? timeout : 0, supported ? "true" : "false", is_sleeping ? "true" : "false");
    return 0;
}

/* Returns false if the suspend was refused (e.g. an active wakeup source). */
static bool suspend_system(const char *reason) {
    ALOGI("suspending (%s)", reason);
    is_sleeping = true;
    publish_state();
    int fd = open(POWER_STATE, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        ALOGE("open " POWER_STATE ": %s", strerror(errno));
        is_sleeping = false;
        publish_state();
        return false;
    }
    /* Blocks until the system has resumed. */
    ssize_t n = write(fd, "mem", 3);
    int err = errno;
    close(fd);
    if (n < 0) {
        ALOGW("suspend failed: %s", strerror(err));
        is_sleeping = false;
        publish_state();
        return false;
    }
    ALOGI("resumed");
    is_sleeping = false;
    publish_state();
    return true;
}

int main(void) {
    ipc_server = matonos_ipc_create("sleep");
    if (!ipc_server || matonos_ipc_register(ipc_server, "get_state", handle_get_state, NULL) != 0 ||
        matonos_ipc_start(ipc_server) != 0) {
        ALOGE("cannot register MatonOS channel service");
        return 1;
    }
    epfd = epoll_create1(EPOLL_CLOEXEC);
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (epfd < 0 || inotify_fd < 0) {
        ALOGE("epoll/inotify: %s", strerror(errno));
        return 1;
    }
    inotify_add_watch(inotify_fd, INPUT_DIR, IN_CREATE);
    struct epoll_event iev = {.events = EPOLLIN, .data.u32 = MAX_DEVICES};
    epoll_ctl(epfd, EPOLL_CTL_ADD, inotify_fd, &iev);
    scan_devices();

    long long last_activity = now_ms();
    long long power_down_at = -1;
    long long retry_until = 0;
    long long resumed_at = -RESUME_GRACE_MS;
    ALOGI("started, idle timeout %llds", idle_limit_ms() / 1000);
    publish_state();
    long long announced_limit = idle_limit_ms();

    for (;;) {
        if (idle_limit_ms() != announced_limit) {
            announced_limit = idle_limit_ms();
            publish_state();
        }
        long long limit = idle_limit_ms();
        int timeout = -1;
        if (limit > 0) {
            long long due = last_activity + limit;
            if (due < retry_until) due = retry_until;
            long long left = due - now_ms();
            timeout = left > 0 ? (int)(left > 60000 ? 60000 : left) : 0;
        }

        struct epoll_event events[16];
        int n = epoll_wait(epfd, events, 16, timeout);
        if (n < 0 && errno != EINTR) {
            ALOGE("epoll_wait: %s", strerror(errno));
            return 1;
        }

        const char *reason = NULL;
        for (int e = 0; e < n; e++) {
            uint32_t idx = events[e].data.u32;
            if (idx == MAX_DEVICES) {
                handle_inotify();
                continue;
            }
            struct input_event in[64];
            ssize_t len = read(devices[idx].fd, in, sizeof(in));
            if (len < 0) {
                if (errno == ENODEV) remove_device((int)idx);
                continue;
            }
            for (size_t i = 0; i < (size_t)len / sizeof(in[0]); i++) {
                if (in[i].type == EV_SYN || in[i].type == EV_MSC) continue;
                last_activity = now_ms();
                if (in[i].type == EV_KEY && in[i].code == KEY_POWER) {
                    if (in[i].value == 1) {
                        power_down_at = last_activity;
                    } else if (in[i].value == 0 && power_down_at >= 0) {
                        if (last_activity - power_down_at < SHORT_PRESS_MS &&
                            power_down_at - resumed_at >= RESUME_GRACE_MS) {
                            reason = "power button";
                        }
                        power_down_at = -1;
                    }
                } else if (in[i].type == EV_SW && in[i].code == SW_LID && in[i].value == 1) {
                    reason = "lid closed";
                }
            }
        }

        /* Re-read the timeout: it may have changed while we were waiting. */
        limit = idle_limit_ms();
        if (!reason && limit > 0 && now_ms() >= last_activity + limit &&
            now_ms() >= retry_until) {
            reason = "idle";
        }
        if (reason) {
            if (suspend_system(reason)) {
                retry_until = 0;
                resumed_at = now_ms();
                power_down_at = -1;
            } else {
                retry_until = now_ms() + RETRY_MS;
            }
            last_activity = now_ms();
        }
    }
}
