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

#define LOG_TAG "matonos-sleepd"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define INPUT_DIR "/dev/input"
#define POWER_STATE "/sys/power/state"
#define IDLE_PROP "persist.vendor.maton.sleep_idle_s"
#define DEFAULT_IDLE_S 900
#define SHORT_PRESS_MS 800
#define RETRY_MS 30000
#define ANDROID_WAKE_STATE_TTL_MS 10000
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
static _Atomic bool android_awake_blocked;
static _Atomic int android_wake_lock_count;
static _Atomic bool android_audio_active;
static _Atomic bool android_stay_awake;
static _Atomic long long android_wake_state_updated_ms;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long idle_limit_ms(void) {
    char value[PROP_VALUE_MAX];
    int s = DEFAULT_IDLE_S;
    if (__system_property_get(IDLE_PROP, value) > 0) {
        char *end = NULL;
        errno = 0;
        long parsed = strtol(value, &end, 10);
        if (errno || end == value || *end || parsed < 0 || parsed > 86400) parsed = DEFAULT_IDLE_S;
        s = (int)parsed;
    }
    return s > 0 ? (long long)s * 1000 : -1;
}

static void add_device(const char *name) {
    if (strncmp(name, "event", 5) != 0) return;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].fd >= 0 && strcmp(devices[i].name, name) == 0) return;
    }
    char path[128];
    snprintf(path, sizeof(path), INPUT_DIR "/%s", name);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        ALOGW("open %s: %s", path, strerror(errno));
        return;
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].fd < 0) {
            devices[i].fd = fd;
            snprintf(devices[i].name, sizeof(devices[i].name), "%s", name);
            struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0) return;
            ALOGW("epoll add %s: %s", name, strerror(errno));
            devices[i].fd = -1;
            devices[i].name[0] = '\0';
            close(fd);
            return;
        }
    }
    ALOGW("too many input devices, ignoring %s", name);
    close(fd);
}

static void remove_device(int i) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, devices[i].fd, NULL);
    close(devices[i].fd);
    devices[i].fd = -1;
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

static int make_state(char *state, size_t capacity) {
    long long idle_ms = idle_limit_ms();
    int timeout = idle_ms > 0 ? (int)(idle_ms / 1000) : 0;
    int power_fd = open(POWER_STATE, O_RDONLY | O_CLOEXEC);
    char power_states[256] = {0};
    ssize_t power_len = power_fd >= 0 ? read(power_fd, power_states, sizeof(power_states) - 1) : -1;
    if (power_fd >= 0) close(power_fd);
    bool supported = power_len > 0 && strstr(power_states, "mem") != NULL;
    return snprintf(state, capacity, "{\"idleTimeoutSeconds\":%d,\"suspendSupported\":%s,\"sleeping\":%s,\"androidWakeBlocked\":%s,\"wakeLockCount\":%d,\"audioActive\":%s,\"stayAwake\":%s}",
             timeout > 0 ? timeout : 0, supported ? "true" : "false", is_sleeping ? "true" : "false",
             android_awake_blocked ? "true" : "false", android_wake_lock_count,
             android_audio_active ? "true" : "false", android_stay_awake ? "true" : "false");
}

static void publish_state(void) {
    if (!ipc_server) return;
    char state[256];
    make_state(state, sizeof(state));
    matonos_ipc_publish(ipc_server, "state", state);
}

static int handle_get_state(const char *args, char *result, size_t capacity, void *context) {
    (void)args; (void)context;
    if (capacity < 2) return -1;
    return make_state(result, capacity) < (int)capacity ? 0 : -1;
}

static bool json_bool(const char *args, const char *key, bool *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(args, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool json_int(const char *args, const char *key, int *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(args, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return false;
    char *end = NULL;
    long value = strtol(p, &end, 10);
    if (end == p || value > 100000) return false;
    *out = (int)value;
    return true;
}

/* Called only by the privileged System Bridge over the fixed Binder channel. */
static int handle_set_wake_state(const char *args, char *result, size_t capacity, void *context) {
    (void)context;
    bool blocked, audio, stay_awake;
    int locks;
    if (!json_bool(args, "blocked", &blocked) || !json_bool(args, "audioActive", &audio) ||
        !json_bool(args, "stayAwake", &stay_awake) || !json_int(args, "wakeLockCount", &locks)) {
        return -1;
    }
    bool old = android_awake_blocked;
    int old_locks = android_wake_lock_count;
    bool old_audio = android_audio_active;
    bool old_stay_awake = android_stay_awake;
    android_wake_lock_count = locks;
    android_audio_active = audio;
    android_stay_awake = stay_awake;
    android_awake_blocked = blocked;
    android_wake_state_updated_ms = now_ms();
    if (old != blocked || old_locks != locks || old_audio != audio || old_stay_awake != stay_awake) {
        ALOGI("Android wake state %s (wake locks=%d audio=%s stay-awake=%s)",
              blocked ? "holds suspend" : "released suspend", locks,
              audio ? "active" : "inactive", stay_awake ? "enabled" : "disabled");
        publish_state();
    }
    if (capacity < 12) return -1;
    snprintf(result, capacity, "{\"ok\":true,\"blocked\":%s}", blocked ? "true" : "false");
    return 0;
}

/* Returns false if the suspend was refused (e.g. an active wakeup source). */
static bool suspend_system(const char *reason) {
    if (android_awake_blocked) {
        ALOGI("suspend deferred (%s): Android wake state is active (wake locks=%d audio=%s stay-awake=%s)",
              reason, android_wake_lock_count, android_audio_active ? "active" : "inactive",
              android_stay_awake ? "enabled" : "disabled");
        publish_state();
        return false;
    }
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
    if (android_awake_blocked) {
        close(fd);
        ALOGI("suspend cancelled (%s): Android wake state became active", reason);
        is_sleeping = false;
        publish_state();
        return false;
    }
    /* Blocks until the system has resumed. */
    ssize_t n;
    do { n = write(fd, "mem", 3); } while (n < 0 && errno == EINTR);
    int err = errno;
    close(fd);
    if (n != 3) {
        if (n < 0) ALOGW("suspend failed: %s", strerror(err));
        else ALOGW("suspend failed: short write (%zd of 3 bytes)", n);
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
        matonos_ipc_register(ipc_server, "set_wake_state", handle_set_wake_state, NULL) != 0 ||
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
    if (inotify_add_watch(inotify_fd, INPUT_DIR, IN_CREATE) < 0) {
        ALOGW("inotify watch unavailable; initial devices still work: %s", strerror(errno));
    }
    struct epoll_event iev = {.events = EPOLLIN, .data.u32 = MAX_DEVICES};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, inotify_fd, &iev) < 0) {
        ALOGW("inotify epoll add unavailable; initial devices still work: %s", strerror(errno));
    }
    for (int i = 0; i < MAX_DEVICES; i++) devices[i].fd = -1;
    scan_devices();

    long long last_activity = now_ms();
    long long power_down_at = -1;
    long long retry_until = 0;
    long long resumed_at = -RESUME_GRACE_MS;
    long long startup_limit = idle_limit_ms();
    if (startup_limit < 0) ALOGI("started, idle timeout never");
    else ALOGI("started, idle timeout %llds", startup_limit / 1000);
    publish_state();
    long long announced_limit = idle_limit_ms();

    for (;;) {
        if (android_awake_blocked && now_ms() - android_wake_state_updated_ms > ANDROID_WAKE_STATE_TTL_MS) {
            android_awake_blocked = false;
            android_wake_lock_count = 0;
            android_audio_active = false;
            android_stay_awake = false;
            ALOGW("Android wake-state updates expired; allowing sleep until the bridge reconnects");
            publish_state();
        }
        if (idle_limit_ms() != announced_limit) {
            announced_limit = idle_limit_ms();
            publish_state();
        }
        long long limit = idle_limit_ms();
        int timeout = 1000; /* Also retries nodes missed during a udev create race. */
        if (limit > 0) {
            long long due = last_activity + limit;
            if (due < retry_until) due = retry_until;
            long long left = due - now_ms();
            timeout = left > 0 ? (int)(left > 60000 ? 60000 : left) : 0;
        }
        if (android_awake_blocked && (timeout < 0 || timeout > 1000)) timeout = 1000;

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
            if (idx >= MAX_DEVICES || devices[idx].fd < 0) continue;
            struct input_event in[64];
            ssize_t len = read(devices[idx].fd, in, sizeof(in));
            if (len == 0) { remove_device((int)idx); continue; }
            if (len < 0) {
                if (errno != EAGAIN && errno != EINTR) remove_device((int)idx);
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
            /* The Binder callback may have blocked sleep after the loop's prior check. */
            if (android_awake_blocked) reason = NULL;
        }
        if (reason) {
            if (suspend_system(reason)) {
                retry_until = 0;
                resumed_at = now_ms();
                power_down_at = -1;
            } else if (android_awake_blocked) {
                retry_until = 0;
            } else {
                retry_until = now_ms() + RETRY_MS;
            }
            last_activity = now_ms();
        }
    }
}
