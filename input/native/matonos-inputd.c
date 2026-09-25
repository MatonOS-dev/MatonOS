/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MatonOS input proxy. Physical keyboards and pointer-like devices are
 * grabbed and merged into stable uinput keyboard and mouse devices. Relative
 * devices pass through unchanged; absolute tablet and touchpad motion is
 * converted to relative mouse deltas because stock InputReader does not
 * classify a bare ABS_X/ABS_Y mouse as a cursor.
 */
#define _GNU_SOURCE
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <matonos_ipc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <stdatomic.h>
#include <unistd.h>

#define LOG_TAG "matonos-inputd"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define INPUT_DIR "/dev/input"
#define UINPUT_NODE "/dev/uinput"
#define MAX_DEVICES 128
#define MAX_EVENTS 32
#define PROP_PRESENT "vendor.maton.input.present"
#define PROP_KEYBOARD "vendor.maton.input.keyboard"
#define PROP_POINTER "vendor.maton.input.pointer"
#define DRM_DIR "/sys/class/drm"

enum kind { KIND_NONE = 0, KIND_KEYBOARD = 1, KIND_POINTER = 2 };
struct source {
    int fd;
    char node[32];
    char name[128];
    enum kind kind;
    bool absolute;
    bool mt;
    bool have_x, have_y;
    int x, y, min_x, max_x, min_y, max_y;
    int slot;
    bool finger_down;
    bool left_down;
    bool has_hardware_buttons;
    bool has_wheel_hi_res, has_hwheel_hi_res;
    bool wheel_hi_res_this_frame, hwheel_hi_res_this_frame;
    int wheel_legacy_pending, hwheel_legacy_pending;
    int wheel_hi_res_remainder, hwheel_hi_res_remainder;
    bool wheel_logged, hwheel_logged;
    unsigned long keys[(KEY_MAX + sizeof(unsigned long) * 8) / (sizeof(unsigned long) * 8)];
};

static struct source sources[MAX_DEVICES];
static int epfd = -1, inotify_fd = -1, keyboard_fd = -1, pointer_fd = -1;
static _Atomic int touchpad_fd = -1;
static _Atomic unsigned int keyboards, pointers;
static MatonosIpcServer *ipc;
static bool pointer_wheel_hi_res, pointer_hwheel_hi_res;
static _Atomic uint64_t requested_display_size;
static int display_width = 1200, display_height = 1200;
static float density_correction = 1.0f;
static float pointer_pos_x = 600.0f, pointer_pos_y = 600.0f;

/* Connector `mode` is the currently programmed DRM mode. `modes` is only the
 * advertised list and can disagree with Android's scanout after boot. */
static bool read_display_geometry(int *width, int *height) {
    DIR *dir = opendir(DRM_DIR);
    if (!dir) return false;
    bool found = false;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "card", 4) || !strchr(de->d_name, '-')) continue;
        char path[256], status[32] = {0}, mode[64] = {0};
        snprintf(path, sizeof(path), DRM_DIR "/%s/status", de->d_name);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        (void)fgets(status, sizeof(status), f); fclose(f);
        if (strncmp(status, "connected", 9)) continue;
        snprintf(path, sizeof(path), DRM_DIR "/%s/mode", de->d_name);
        f = fopen(path, "re");
        if (!f) continue;
        if (fgets(mode, sizeof(mode), f)) {
            int w = 0, h = 0;
            if (sscanf(mode, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                *width = w; *height = h; found = true;
                fclose(f); break;
            }
        }
        fclose(f);
    }
    closedir(dir);
    if (!found) {
        char size[64] = {0};
        FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "re");
        if (f) {
            int w = 0, h = 0;
            if (fgets(size, sizeof(size), f) && sscanf(size, "%d,%d", &w, &h) == 2 && w > 0 && h > 0) {
                *width = w; *height = h; found = true;
            }
            fclose(f);
        }
    }
    return found;
}

static void refresh_display_geometry(void) {
    int width = display_width, height = display_height;
    uint64_t request = atomic_load(&requested_display_size);
    bool have_size = false;
    if (request) {
        width = (int)(request >> 32);
        height = (int)(request & UINT32_MAX);
        have_size = width > 0 && height > 0;
    } else {
        have_size = read_display_geometry(&width, &height);
    }
    if (have_size && (width != display_width || height != display_height)) {
        pointer_pos_x = pointer_pos_x * (float)width / (float)display_width;
        pointer_pos_y = pointer_pos_y * (float)height / (float)display_height;
        display_width = width; display_height = height;
        ALOGI("active scanout changed to %dx%d", width, height);
    }
    char density[32] = {0};
    if (__system_property_get("ro.sf.lcd_density", density) > 0) {
        int dpi = atoi(density);
        if (dpi > 0) density_correction = 240.0f / (float)dpi;
    }
}

static bool bit_test(const unsigned long *bits, unsigned int bit) {
    return (bits[bit / (sizeof(unsigned long) * 8)] &
            (1UL << (bit % (sizeof(unsigned long) * 8)))) != 0;
}

static bool get_bits(int fd, unsigned int type, unsigned long *bits, size_t size) {
    memset(bits, 0, size);
    return ioctl(fd, EVIOCGBIT(type, size), bits) >= 0;
}

static int emit(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event ev = {.type = type, .code = code, .value = value};
    ssize_t n;
    do { n = write(fd, &ev, sizeof(ev)); } while (n < 0 && errno == EINTR);
    return n == sizeof(ev) ? 0 : -1;
}

static void sync_out(int fd) { (void)emit(fd, EV_SYN, SYN_REPORT, 0); }

static void state_update(void) {
    const bool present = keyboards != 0 || pointers != 0;
    (void)__system_property_set(PROP_PRESENT, present ? "1" : "0");
    (void)__system_property_set(PROP_KEYBOARD, keyboards ? "1" : "0");
    (void)__system_property_set(PROP_POINTER, pointers ? "1" : "0");
    if (present && keyboards && pointers && touchpad_fd < 0) {
        int fd = open(UINPUT_NODE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            if (ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0 &&
                ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT) == 0 &&
                ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID) == 0 &&
                ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X) == 0 &&
                ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y) == 0 &&
                ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER) == 0) {
                struct uinput_abs_setup abs = {.code = ABS_MT_SLOT, .absinfo = {.minimum=0,.maximum=0}};
                (void)ioctl(fd, UI_ABS_SETUP, &abs);
                abs.code = ABS_MT_TRACKING_ID; abs.absinfo.minimum = 0; abs.absinfo.maximum = 1;
                (void)ioctl(fd, UI_ABS_SETUP, &abs);
                abs.code = ABS_MT_POSITION_X; abs.absinfo.minimum = 0; abs.absinfo.maximum = 32767;
                (void)ioctl(fd, UI_ABS_SETUP, &abs);
                abs.code = ABS_MT_POSITION_Y; abs.absinfo.maximum = 32767;
                (void)ioctl(fd, UI_ABS_SETUP, &abs);
                struct uinput_setup setup = {0};
                setup.id.bustype = BUS_VIRTUAL; setup.id.vendor = 0x4d54;
                setup.id.product = 3; setup.id.version = 1;
                snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "MatonOS Desktop Touchpad");
                if (ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0) {
                    touchpad_fd = fd;
                    ALOGI("desktop touchpad present (keyboard and mouse available)");
                    fd = -1;
                }
            }
            if (fd >= 0) close(fd);
        }
    } else if (!(present && keyboards && pointers) && touchpad_fd >= 0) {
        (void)ioctl(touchpad_fd, UI_DEV_DESTROY);
        close(touchpad_fd);
        touchpad_fd = -1;
        ALOGI("desktop touchpad removed");
    }
    if (ipc) {
        char json[160];
        snprintf(json, sizeof(json), "{\"keyboards\":%u,\"pointers\":%u,\"desktopTouchpad\":%s}",
                 keyboards, pointers, touchpad_fd >= 0 ? "true" : "false");
        (void)matonos_ipc_publish(ipc, "state", json);
    }
}

static bool configure_keyboard(int fd) {
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_MSC) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) return false;
    for (unsigned int k = 1; k <= KEY_MAX; k++) {
        if (k >= BTN_MISC && k < BTN_MOUSE) continue;
        if (ioctl(fd, UI_SET_KEYBIT, k) < 0) return false;
    }
    if (ioctl(fd, UI_SET_MSCBIT, MSC_SCAN) < 0) return false;
    struct uinput_setup setup = {0};
    setup.id.bustype = BUS_VIRTUAL; setup.id.vendor = 0x4d54; setup.id.product = 1; setup.id.version = 1;
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "MatonOS Keyboard");
    return ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
}

static bool configure_pointer(int fd) {
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) return false;
    const unsigned int buttons[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA,
                                    BTN_FORWARD, BTN_BACK, BTN_TASK};
    for (size_t i = 0; i < sizeof(buttons)/sizeof(buttons[0]); i++)
        if (ioctl(fd, UI_SET_KEYBIT, buttons[i]) < 0) return false;
    const unsigned int required_axes[] = {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL};
    for (size_t i = 0; i < sizeof(required_axes)/sizeof(required_axes[0]); i++)
        if (ioctl(fd, UI_SET_RELBIT, required_axes[i]) < 0) return false;
    pointer_wheel_hi_res = ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES) == 0;
    pointer_hwheel_hi_res = ioctl(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES) == 0;
    struct uinput_setup setup = {0};
    setup.id.bustype = BUS_VIRTUAL; setup.id.vendor = 0x4d54; setup.id.product = 2; setup.id.version = 1;
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "MatonOS Pointer");
    return ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
}

static bool is_keyboard(const unsigned long *keys) {
    bool letters = false;
    for (unsigned int k = KEY_Q; k <= KEY_P; k++) if (bit_test(keys, k)) { letters = true; break; }
    return letters && (bit_test(keys, KEY_ENTER) || bit_test(keys, KEY_SPACE) || bit_test(keys, KEY_LEFTSHIFT));
}

static bool has_abs_range(int fd, unsigned int code, int *min, int *max) {
    struct input_absinfo a;
    if (ioctl(fd, EVIOCGABS(code), &a) < 0 || a.maximum <= a.minimum) return false;
    *min = a.minimum; *max = a.maximum; return true;
}

static int allocate_source(const char *node) {
    for (int i = 0; i < MAX_DEVICES; i++) if (sources[i].fd < 0) {
        memset(&sources[i], 0, sizeof(sources[i]));
        sources[i].fd = -1;
        snprintf(sources[i].node, sizeof(sources[i].node), "%s", node);
        return i;
    }
    return -1;
}

static void release_keys(struct source *s) {
    int out = s->kind == KIND_KEYBOARD ? keyboard_fd : pointer_fd;
    for (unsigned int k = 1; k <= KEY_MAX; k++) {
        if (bit_test(s->keys, k)) {
            (void)emit(out, EV_KEY, (unsigned short)k, 0);
            s->keys[k / (sizeof(unsigned long)*8)] &= ~(1UL << (k % (sizeof(unsigned long)*8)));
        }
    }
    sync_out(out);
}

static void remove_source(int i) {
    struct source *s = &sources[i];
    if (s->fd < 0) return;
    (void)ioctl(s->fd, EVIOCGRAB, 0);
    release_keys(s);
    epoll_ctl(epfd, EPOLL_CTL_DEL, s->fd, NULL);
    close(s->fd);
    if (s->kind == KIND_KEYBOARD && keyboards) keyboards--;
    if (s->kind == KIND_POINTER && pointers) pointers--;
    ALOGI("removed %s (%s)", s->node, s->name);
    s->fd = -1;
    state_update();
}

static void add_source(const char *node) {
    if (strncmp(node, "event", 5) != 0) return;
    for (int i = 0; i < MAX_DEVICES; i++) if (sources[i].fd >= 0 && !strcmp(sources[i].node, node)) return;
    char path[160]; snprintf(path, sizeof(path), INPUT_DIR "/%s", node);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct input_id id;
    char name[128] = {0};
    unsigned long key_bits[(KEY_MAX + sizeof(unsigned long)*8)/(sizeof(unsigned long)*8)];
    unsigned long rel_bits[(REL_MAX + sizeof(unsigned long)*8)/(sizeof(unsigned long)*8)];
    unsigned long abs_bits[(ABS_MAX + sizeof(unsigned long)*8)/(sizeof(unsigned long)*8)];
    unsigned long prop_bits[(INPUT_PROP_MAX + sizeof(unsigned long)*8)/(sizeof(unsigned long)*8)];
    if (ioctl(fd, EVIOCGID, &id) < 0 || ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 ||
        !get_bits(fd, EV_KEY, key_bits, sizeof(key_bits)) ||
        !get_bits(fd, EV_REL, rel_bits, sizeof(rel_bits)) ||
        !get_bits(fd, EV_ABS, abs_bits, sizeof(abs_bits)) ||
        ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits) < 0) { close(fd); return; }
    if (id.bustype == BUS_VIRTUAL || !strncmp(name, "MatonOS ", 8)) { close(fd); return; }
    bool kb = is_keyboard(key_bits);
    int min_x=0,max_x=0,min_y=0,max_y=0;
    bool abs_xy = bit_test(abs_bits, ABS_X) && bit_test(abs_bits, ABS_Y) &&
                  has_abs_range(fd, ABS_X, &min_x, &max_x) && has_abs_range(fd, ABS_Y, &min_y, &max_y);
    bool pointer_property = bit_test(prop_bits, INPUT_PROP_POINTER);
    bool direct_touchscreen = bit_test(prop_bits, INPUT_PROP_DIRECT) &&
                              !bit_test(key_bits, BTN_MOUSE) && !bit_test(key_bits, BTN_LEFT);
    bool mt = bit_test(abs_bits, ABS_MT_POSITION_X) && bit_test(abs_bits, ABS_MT_POSITION_Y) &&
              (bit_test(key_bits, BTN_TOUCH) || bit_test(key_bits, BTN_TOOL_FINGER)) &&
              (pointer_property || bit_test(key_bits, BTN_TOOL_FINGER));
    if (mt) {
        (void)has_abs_range(fd, ABS_MT_POSITION_X, &min_x, &max_x);
        (void)has_abs_range(fd, ABS_MT_POSITION_Y, &min_y, &max_y);
    }
    bool rel = bit_test(rel_bits, REL_X) && bit_test(rel_bits, REL_Y);
    bool mouse = bit_test(key_bits, BTN_MOUSE) || bit_test(key_bits, BTN_LEFT) ||
                 bit_test(key_bits, BTN_TOOL_FINGER) || bit_test(key_bits, BTN_TOUCH);
    bool ptr = mouse && (rel || abs_xy || mt) && !direct_touchscreen;
    if (!kb && !ptr) { close(fd); return; }
    if (ioctl(fd, EVIOCGRAB, 1) < 0) { ALOGW("cannot grab %s (%s): %s", node, name, strerror(errno)); close(fd); return; }
    int i = allocate_source(node);
    if (i < 0) { (void)ioctl(fd, EVIOCGRAB, 0); close(fd); return; }
    struct source *s = &sources[i];
    s->fd = fd; s->kind = kb ? KIND_KEYBOARD : KIND_POINTER;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->absolute = !rel && (abs_xy || mt); s->mt = mt;
    s->has_wheel_hi_res = bit_test(rel_bits, REL_WHEEL_HI_RES);
    s->has_hwheel_hi_res = bit_test(rel_bits, REL_HWHEEL_HI_RES);
    s->has_hardware_buttons = bit_test(key_bits, BTN_MOUSE) || bit_test(key_bits, BTN_LEFT);
    s->min_x=min_x; s->max_x=max_x; s->min_y=min_y; s->max_y=max_y;
    struct epoll_event ev = {.events = EPOLLIN | EPOLLHUP | EPOLLERR, .data.u32 = (unsigned int)i};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) { (void)ioctl(fd, EVIOCGRAB,0); close(fd); s->fd=-1; return; }
    if (kb) keyboards++;
    if (ptr) pointers++;
    ALOGI("grabbed %s (%s)%s%s wheel=%s%s hwheel=%s%s (virtual hi-res=%s/%s)",
          node, name, kb ? " keyboard" : "", ptr ? " pointer" : "",
          bit_test(rel_bits, REL_WHEEL) ? "REL_WHEEL" : "none",
          s->has_wheel_hi_res ? "+HI_RES" : "",
          bit_test(rel_bits, REL_HWHEEL) ? "REL_HWHEEL" : "none",
          s->has_hwheel_hi_res ? "+HI_RES" : "",
          pointer_wheel_hi_res ? "yes" : "no", pointer_hwheel_hi_res ? "yes" : "no");
    state_update();
}

static int scaled_delta(int delta, int min, int max, int extent) {
    if (max <= min) return delta;
    float v = (float)delta * (float)extent * density_correction / (float)(max - min);
    if (v > 32767) v = 32767; if (v < -32767) v = -32767;
    return (int)v;
}

static void emit_scroll(struct source *s, bool horizontal, bool high_res, int value) {
    bool output_high_res = horizontal ? pointer_hwheel_hi_res : pointer_wheel_hi_res;
    int legacy_code = horizontal ? REL_HWHEEL : REL_WHEEL;
    int high_res_code = horizontal ? REL_HWHEEL_HI_RES : REL_WHEEL_HI_RES;
    int *remainder = horizontal ? &s->hwheel_hi_res_remainder : &s->wheel_hi_res_remainder;
    if (!value) return;
    if (high_res && output_high_res) {
        (void)emit(pointer_fd, EV_REL, (unsigned short)high_res_code, value);
    } else if (output_high_res) {
        (void)emit(pointer_fd, EV_REL, (unsigned short)high_res_code,
                   high_res ? value : value * 120);
    } else {
        int units = high_res ? value : value * 120;
        *remainder += units;
        int detents = *remainder / 120;
        *remainder -= detents * 120;
        if (detents) (void)emit(pointer_fd, EV_REL, (unsigned short)legacy_code, detents);
    }
    bool *logged = horizontal ? &s->hwheel_logged : &s->wheel_logged;
    if (!*logged) {
        ALOGI("wheel forwarded from %s: axis=%s input=%s value=%d output=%s",
              s->node, horizontal ? "horizontal" : "vertical",
              high_res ? "hi-res" : "legacy", value,
              output_high_res ? "hi-res/120-units-per-detent" : "legacy-detents");
        *logged = true;
    }
}

static void flush_legacy_wheel(struct source *s) {
    if (s->wheel_legacy_pending && !s->wheel_hi_res_this_frame)
        emit_scroll(s, false, false, s->wheel_legacy_pending);
    if (s->hwheel_legacy_pending && !s->hwheel_hi_res_this_frame)
        emit_scroll(s, true, false, s->hwheel_legacy_pending);
    s->wheel_legacy_pending = s->hwheel_legacy_pending = 0;
    s->wheel_hi_res_this_frame = s->hwheel_hi_res_this_frame = false;
}

/* Absolute VM/tablet coordinates describe the desired guest position, not a
 * velocity. Track the last requested position and emit only its difference;
 * this corrects accumulated integer rounding and edge-clamp drift. */
static void emit_absolute_axis(unsigned int code, int value, int min, int max) {
    if (max <= min) return;
    int extent = code == ABS_X ? display_width : display_height;
    float *position = code == ABS_X ? &pointer_pos_x : &pointer_pos_y;
    if (extent < 2) return;
    float target_f = (float)(value - min) * (float)(extent - 1) /
                     (float)(max - min);
    int target = (int)(target_f + 0.5f);
    if (target < 0) target = 0;
    if (target >= extent) target = extent - 1;
    float scaled_delta = (float)(target - *position) * density_correction;
    int delta = scaled_delta >= 0.0f ? (int)(scaled_delta + 0.5f) : (int)(scaled_delta - 0.5f);
    *position += (float)delta / density_correction;
    if (*position < 0.0f) *position = 0.0f;
    if (*position > (float)(extent - 1)) *position = (float)(extent - 1);
    if (delta) (void)emit(pointer_fd, EV_REL, code == ABS_X ? REL_X : REL_Y, delta);
}

static void forward_event(struct source *s, const struct input_event *e) {
    if (e->type == EV_SYN) {
        if (e->code == SYN_REPORT) {
            if (s->kind == KIND_POINTER) flush_legacy_wheel(s);
            sync_out(s->kind == KIND_KEYBOARD ? keyboard_fd : pointer_fd);
        }
        return;
    }
    if (s->kind == KIND_KEYBOARD) {
        if (e->type == EV_KEY && e->code <= KEY_MAX) {
            if (e->value) s->keys[e->code/(sizeof(unsigned long)*8)] |= 1UL << (e->code%(sizeof(unsigned long)*8));
            else s->keys[e->code/(sizeof(unsigned long)*8)] &= ~(1UL << (e->code%(sizeof(unsigned long)*8)));
            (void)emit(keyboard_fd, EV_KEY, e->code, e->value);
        } else if (e->type == EV_MSC && e->code == MSC_SCAN) (void)emit(keyboard_fd, EV_MSC, MSC_SCAN, e->value);
        return;
    }
    if (e->type == EV_REL) {
        if (e->code == REL_WHEEL) {
            if (s->has_wheel_hi_res && !s->wheel_hi_res_this_frame)
                s->wheel_legacy_pending += e->value;
            else if (!s->has_wheel_hi_res)
                emit_scroll(s, false, false, e->value);
            return;
        }
        if (e->code == REL_HWHEEL) {
            if (s->has_hwheel_hi_res && !s->hwheel_hi_res_this_frame)
                s->hwheel_legacy_pending += e->value;
            else if (!s->has_hwheel_hi_res)
                emit_scroll(s, true, false, e->value);
            return;
        }
        if (e->code == REL_WHEEL_HI_RES) {
            s->wheel_hi_res_this_frame = true;
            s->wheel_legacy_pending = 0;
            emit_scroll(s, false, true, e->value);
            return;
        }
        if (e->code == REL_HWHEEL_HI_RES) {
            s->hwheel_hi_res_this_frame = true;
            s->hwheel_legacy_pending = 0;
            emit_scroll(s, true, true, e->value);
            return;
        }
        (void)emit(pointer_fd, EV_REL, e->code, e->value);
        if (e->code == REL_X) pointer_pos_x += (float)e->value / density_correction;
        if (e->code == REL_Y) pointer_pos_y += (float)e->value / density_correction;
        if (pointer_pos_x < 0.0f) pointer_pos_x = 0.0f;
        if (pointer_pos_y < 0.0f) pointer_pos_y = 0.0f;
        if (pointer_pos_x > display_width - 1) pointer_pos_x = (float)(display_width - 1);
        if (pointer_pos_y > display_height - 1) pointer_pos_y = (float)(display_height - 1);
        return;
    }
    if (e->type == EV_ABS) {
        if (e->code == ABS_MT_SLOT) { s->slot = e->value; return; }
        if (s->mt && e->code == ABS_MT_POSITION_X && s->slot == 0) {
            int old=s->x; bool had=s->have_x; s->x=e->value; s->have_x=true;
            if (had && s->finger_down) {
                int delta = scaled_delta(s->x-old,s->min_x,s->max_x, display_width);
                (void)emit(pointer_fd, EV_REL, REL_X, delta);
                pointer_pos_x += (float)delta / density_correction;
                if (pointer_pos_x < 0.0f) pointer_pos_x = 0.0f;
                if (pointer_pos_x > display_width - 1) pointer_pos_x = (float)(display_width - 1);
            }
        } else if (s->mt && e->code == ABS_MT_POSITION_Y && s->slot == 0) {
            int old=s->y; bool had=s->have_y; s->y=e->value; s->have_y=true;
            if (had && s->finger_down) {
                int delta = scaled_delta(s->y-old,s->min_y,s->max_y, display_height);
                (void)emit(pointer_fd, EV_REL, REL_Y, delta);
                pointer_pos_y += (float)delta / density_correction;
                if (pointer_pos_y < 0.0f) pointer_pos_y = 0.0f;
                if (pointer_pos_y > display_height - 1) pointer_pos_y = (float)(display_height - 1);
            }
        } else if (!s->mt && (e->code == ABS_X || e->code == ABS_Y)) {
            int *coord = e->code == ABS_X ? &s->x : &s->y;
            int *min = e->code == ABS_X ? &s->min_x : &s->min_y;
            int *max = e->code == ABS_X ? &s->max_x : &s->max_y;
            bool *have = e->code == ABS_X ? &s->have_x : &s->have_y;
            *coord=e->value; *have=true;
            emit_absolute_axis(e->code, *coord, *min, *max);
        }
        return;
    }
    if (e->type == EV_KEY && e->code <= KEY_MAX) {
        if (e->code == BTN_TOUCH || e->code == BTN_TOOL_FINGER) {
            bool down = e->value != 0;
            if (down != s->finger_down && !s->has_hardware_buttons) {
                /* Touchpad contact acts as a left-button press/release. */
                if (down && !s->left_down) {
                    (void)emit(pointer_fd, EV_KEY, BTN_LEFT, 1);
                    s->keys[BTN_LEFT/(sizeof(unsigned long)*8)] |= 1UL << (BTN_LEFT%(sizeof(unsigned long)*8));
                }
                if (!down && s->left_down) {
                    (void)emit(pointer_fd, EV_KEY, BTN_LEFT, 0);
                    s->keys[BTN_LEFT/(sizeof(unsigned long)*8)] &= ~(1UL << (BTN_LEFT%(sizeof(unsigned long)*8)));
                }
                s->left_down = down;
            }
            s->finger_down = down;
            return;
        }
        if (e->code >= BTN_MOUSE) {
            if (e->value) s->keys[e->code/(sizeof(unsigned long)*8)] |= 1UL << (e->code%(sizeof(unsigned long)*8));
            else s->keys[e->code/(sizeof(unsigned long)*8)] &= ~(1UL << (e->code%(sizeof(unsigned long)*8)));
            (void)emit(pointer_fd, EV_KEY, e->code, e->value);
        }
    }
}

static void scan_devices(void) {
    DIR *dir = opendir(INPUT_DIR);
    if (!dir) { ALOGE("cannot open %s: %s", INPUT_DIR, strerror(errno)); return; }
    struct dirent *de;
    while ((de = readdir(dir))) add_source(de->d_name);
    closedir(dir);
}

static void handle_inotify(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(inotify_fd, buf, sizeof(buf));
    for (char *p = buf; len > 0 && p < buf + len;) {
        struct inotify_event *ev = (struct inotify_event *)p;
        if (ev->len && (ev->mask & IN_CREATE)) add_source(ev->name);
        if (ev->len && (ev->mask & IN_DELETE)) {
            for (int i=0;i<MAX_DEVICES;i++) if (sources[i].fd >= 0 && !strcmp(sources[i].node, ev->name)) remove_source(i);
        }
        p += sizeof(*ev) + ev->len;
    }
}

static int get_state(const char *args, char *result, size_t capacity, void *context) {
    (void)args; (void)context;
    if (capacity < 2) return -1;
    snprintf(result, capacity, "{\"keyboards\":%u,\"pointers\":%u,\"desktopTouchpad\":%s}",
             keyboards, pointers, touchpad_fd >= 0 ? "true" : "false");
    return 0;
}

static bool json_int_field(const char *json, const char *key, int *value) {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p || !(p = strchr(p + strlen(needle), ':'))) return false;
    char *end = NULL;
    long parsed = strtol(p + 1, &end, 10);
    if (end == p + 1 || parsed < 64 || parsed > 32768) return false;
    *value = (int)parsed;
    return true;
}

static int set_display_size(const char *args, char *result, size_t capacity, void *context) {
    (void)context;
    int width, height;
    if (!args || !json_int_field(args, "width", &width) ||
        !json_int_field(args, "height", &height) || capacity < 2) return -1;
    atomic_store(&requested_display_size,
                 ((uint64_t)(uint32_t)width << 32) | (uint32_t)height);
    snprintf(result, capacity, "{\"ok\":true,\"width\":%d,\"height\":%d}", width, height);
    return 0;
}

int main(void) {
    refresh_display_geometry();
    for (int i=0;i<MAX_DEVICES;i++) sources[i].fd=-1;
    keyboard_fd = open(UINPUT_NODE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    pointer_fd = open(UINPUT_NODE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (keyboard_fd < 0 || pointer_fd < 0 || !configure_keyboard(keyboard_fd) || !configure_pointer(pointer_fd)) {
        ALOGE("cannot create stable uinput devices: %s", strerror(errno)); return 1;
    }
    ipc = matonos_ipc_create("input");
    if (!ipc || matonos_ipc_register(ipc, "get_state", get_state, NULL) ||
        matonos_ipc_register(ipc, "set_display_size", set_display_size, NULL) ||
        matonos_ipc_start(ipc)) {
        ALOGE("cannot register input channel service"); return 1;
    }
    epfd = epoll_create1(EPOLL_CLOEXEC);
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (epfd < 0 || inotify_fd < 0 || inotify_add_watch(inotify_fd, INPUT_DIR, IN_CREATE | IN_DELETE) < 0) {
        ALOGE("cannot watch input nodes: %s", strerror(errno)); return 1;
    }
    struct epoll_event watch = {.events=EPOLLIN,.data.u32=MAX_DEVICES};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, inotify_fd, &watch) < 0) return 1;
    scan_devices();
    ALOGI("started; stable keyboard and pointer are active");
    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        refresh_display_geometry();
        if (n == 0) continue;
        if (n < 0) { if (errno == EINTR) continue; ALOGE("epoll_wait: %s", strerror(errno)); return 1; }
        for (int e=0;e<n;e++) {
            unsigned int idx=events[e].data.u32;
            if (idx == MAX_DEVICES) { handle_inotify(); continue; }
            if (idx >= MAX_DEVICES || sources[idx].fd < 0) continue;
            if (events[e].events & (EPOLLHUP|EPOLLERR)) { remove_source((int)idx); continue; }
            struct input_event buf[32];
            ssize_t bytes;
            while ((bytes=read(sources[idx].fd, buf, sizeof(buf))) > 0) {
                size_t count=(size_t)bytes/sizeof(buf[0]);
                for (size_t j=0;j<count;j++) forward_event(&sources[idx], &buf[j]);
            }
            if (bytes < 0 && errno != EAGAIN && errno != EINTR) remove_source((int)idx);
        }
    }
}
