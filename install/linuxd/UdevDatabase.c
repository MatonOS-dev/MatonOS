#define _GNU_SOURCE
/* Android has ueventd, but no udev property database. This is metadata only:
 * never open/chmod input nodes or change the controller permission gate. */
#include "UdevDatabase.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef MATON_UDEV_ROOT
#define MATON_UDEV_ROOT "/data/matonos/linux/udev"
#endif
#ifndef MATON_SYS_ROOT
#define MATON_SYS_ROOT "/sys"
#endif
#define DB_SIZE 8192
#define WORD_BITS (8 * sizeof(unsigned long))
#define MASK_WORDS ((KEY_MAX + WORD_BITS) / WORD_BITS)
typedef struct { unsigned long word[MASK_WORDS]; } Mask;
typedef struct Seen { char id[64]; struct Seen *next; } Seen;
static int monitor_fd = -1;
static pthread_once_t started = PTHREAD_ONCE_INIT;

static bool read_text(const char *dir, const char *attr, char *out, size_t size) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, attr) >= (int)sizeof(path)) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n;
    do { n = read(fd, out, size - 1); } while (n < 0 && errno == EINTR);
    close(fd);
    if (n < 0) return false;
    out[n] = 0;
    while (n && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = 0;
    return true;
}

/* sysfs prints native kernel words most significant first, including zero
 * words. This target has a 64-bit kernel and a 64-bit linuxd. */
static Mask capabilities(const char *dir, const char *attr) {
    Mask mask = {0};
    char text[4096];
    if (!read_text(dir, attr, text, sizeof(text))) return mask;
    char *end = text + strlen(text);
    for (size_t i = 0; i < MASK_WORDS && end > text; i++) {
        while (end > text && end[-1] == ' ') --end;
        *end = 0;
        char *begin = end;
        while (begin > text && begin[-1] != ' ') --begin;
        mask.word[i] = strtoul(begin, NULL, 16);
        end = begin;
    }
    return mask;
}
static bool bit(const Mask *mask, unsigned n) {
    return n <= KEY_MAX && ((mask->word[n / WORD_BITS] >> (n % WORD_BITS)) & 1);
}
static unsigned count_bits(const Mask *mask, unsigned first, unsigned last) {
    unsigned count = 0;
    for (unsigned n = first; n <= last; n++) count += bit(mask, n);
    return count;
}
static void append(char *db, const char *format, ...) {
    size_t length = strlen(db);
    va_list ap;
    va_start(ap, format);
    vsnprintf(db + length, DB_SIZE - length, format, ap);
    va_end(ap);
}
static void flag(char *db, const char *name, bool set) {
    if (set) append(db, "E:%s=1\n", name);
}

/* Capability heuristics follow input_id's categories, including the keyboard
 * and touch-device exclusions which prevent bogus controller discovery.
 * No device-name, vendor or application allowlist. */
static void classify(char *db, const char *dir) {
    Mask ev = capabilities(dir, "capabilities/ev");
    Mask key = capabilities(dir, "capabilities/key");
    Mask abs = capabilities(dir, "capabilities/abs");
    Mask rel = capabilities(dir, "capabilities/rel");
    Mask prop = capabilities(dir, "properties");
    bool keys = bit(&ev, EV_KEY);
    bool xy = bit(&abs, ABS_X) && bit(&abs, ABS_Y);
    bool mt = bit(&abs, ABS_MT_POSITION_X) && bit(&abs, ABS_MT_POSITION_Y);
    if (bit(&abs, ABS_MT_SLOT) && bit(&abs, ABS_MT_SLOT - 1)) mt = false;
    bool accel = bit(&prop, INPUT_PROP_ACCELEROMETER) ||
        (!keys && xy && bit(&abs, ABS_Z));
    bool direct = bit(&prop, INPUT_PROP_DIRECT);
    bool pen = bit(&key, BTN_TOOL_PEN) || bit(&key, BTN_STYLUS);
    bool finger = bit(&key, BTN_TOOL_FINGER) && !pen;
    bool mouse_button = keys && count_bits(&key, BTN_MOUSE, BTN_JOYSTICK - 1);
    bool relative = bit(&ev, EV_REL) && bit(&rel, REL_X) && bit(&rel, REL_Y);
    bool touchpad = !accel && (xy || mt) && finger && !direct;
    bool tablet = !accel && (xy || mt) && pen;
    bool abs_mouse = !accel && xy && !pen && !touchpad && mouse_button;
    bool touchscreen = !accel && (xy || mt) && !pen && !touchpad &&
        (bit(&key, BTN_TOUCH) || direct) && !abs_mouse;
    unsigned buttons = 0;
    if (keys && !bit(&key, BTN_JOYSTICK - 1)) {
        buttons = count_bits(&key, BTN_JOYSTICK, BTN_DIGI - 1) +
            count_bits(&key, BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY40) +
            count_bits(&key, BTN_DPAD_UP, BTN_DPAD_RIGHT);
    }
    unsigned axes = bit(&ev, EV_ABS) ? count_bits(&abs, ABS_RX, ABS_PRESSURE - 1) : 0;
    static const unsigned keyboard_keys[] = {KEY_LEFTCTRL, KEY_CAPSLOCK, KEY_NUMLOCK,
        KEY_INSERT, KEY_MUTE, KEY_CALC, KEY_FILE, KEY_MAIL, KEY_PLAYPAUSE, KEY_BRIGHTNESSDOWN};
    unsigned keyboard_sets = 0;
    for (size_t i = 0; i < sizeof(keyboard_keys)/sizeof(keyboard_keys[0]); i++)
        keyboard_sets += keys && bit(&key, keyboard_keys[i]);
    bool pad_buttons = bit(&key, BTN_0) && bit(&key, BTN_1) && !pen;
    bool wheel = bit(&ev, EV_REL) && (bit(&rel, REL_WHEEL) || bit(&rel, REL_HWHEEL));
    bool tablet_pad = !accel && pad_buttons && (tablet || (wheel && !relative));
    tablet |= tablet_pad;
    bool joystick = !accel && !tablet && !touchpad && !touchscreen && !abs_mouse &&
        buttons + axes >= 2 && keyboard_sets < 4 && !(wheel && pad_buttons);
    bool mouse = !accel && (abs_mouse || (!tablet && !touchpad && !joystick &&
        mouse_button && (relative || !xy)));
    bool any_key = keys && (count_bits(&key, 1, BTN_MISC - 1) ||
        count_bits(&key, KEY_OK, BTN_DPAD_UP - 1) ||
        count_bits(&key, KEY_ALS_TOGGLE, BTN_TRIGGER_HAPPY - 1));
    flag(db, "ID_INPUT", true);
    flag(db, "ID_INPUT_KEY", any_key);
    flag(db, "ID_INPUT_KEYBOARD", keys && (key.word[0] & 0xfffffffeUL) == 0xfffffffeUL);
    flag(db, "ID_INPUT_MOUSE", mouse);
    flag(db, "ID_INPUT_JOYSTICK", joystick);
    flag(db, "ID_INPUT_TOUCHPAD", touchpad);
    flag(db, "ID_INPUT_TOUCHSCREEN", touchscreen);
    flag(db, "ID_INPUT_TABLET", tablet);
    flag(db, "ID_INPUT_TABLET_PAD", tablet_pad);
    flag(db, "ID_INPUT_ACCELEROMETER", accel);
    flag(db, "ID_INPUT_POINTINGSTICK", bit(&prop, INPUT_PROP_POINTING_STICK));
}

static const char *bus_name(unsigned bus) {
    switch (bus) {
    case BUS_USB: return "usb";
    case BUS_BLUETOOTH: return "bluetooth";
    case BUS_PCI: return "pci";
    case BUS_I8042: return "i8042";
    case BUS_I2C: return "i2c";
    case BUS_SPI: return "spi";
    case BUS_VIRTUAL: return "virtual";
    default: return NULL;
    }
}
static bool hex_attr(const char *dir, const char *attr, unsigned *value) {
    char text[64], *end;
    if (!read_text(dir, attr, text, sizeof(text))) return false;
    unsigned long v = strtoul(text, &end, 16);
    if (end == text || *end || v > 0xffff) return false;
    *value = (unsigned)v;
    return true;
}
static void identity(char *db, const char *node) {
    char parent[PATH_MAX], text[4096];
    snprintf(parent, sizeof(parent), "%s", node);
    bool have_vendor = false, have_model = false, have_bus = false;
    while (strlen(parent) > strlen(MATON_SYS_ROOT)) {
        unsigned vendor, model, bus;
        bool v = hex_attr(parent, "id/vendor", &vendor) || hex_attr(parent, "idVendor", &vendor);
        bool m = hex_attr(parent, "id/product", &model) || hex_attr(parent, "idProduct", &model);
        bool b = hex_attr(parent, "id/bustype", &bus);
        /* hidraw -> HID ancestor carries HID_ID=bus:vendor:product. */
        if (read_text(parent, "uevent", text, sizeof(text))) {
            char *hid = strstr(text, "HID_ID=");
            unsigned hb, hv, hm;
            if (hid && sscanf(hid, "HID_ID=%x:%x:%x", &hb, &hv, &hm) == 3) {
                if (!v) { vendor = hv & 0xffff; v = true; }
                if (!m) { model = hm & 0xffff; m = true; }
                if (!b) { bus = hb; b = true; }
            }
            if (!b && strstr(text, "DEVTYPE=usb_device")) { bus = BUS_USB; b = true; }
        }
        if (v && !have_vendor) { append(db, "E:ID_VENDOR_ID=%04x\n", vendor); have_vendor = true; }
        if (m && !have_model) { append(db, "E:ID_MODEL_ID=%04x\n", model); have_model = true; }
        if (b && !have_bus && bus_name(bus)) {
            append(db, "E:ID_BUS=%s\n", bus_name(bus)); have_bus = true;
        }
        char *slash = strrchr(parent, '/');
        if (!slash) break;
        *slash = 0;
    }
}

/* MurmurHash2, seed zero: libudev's classic subsystem/tag BPF wire hash. */
static uint32_t wire_hash(const char *s) {
    size_t n = strlen(s);
    uint32_t h = (uint32_t)n;
    const unsigned char *p = (const unsigned char *)s;
    while (n >= 4) {
        uint32_t k = p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
        k *= 0x5bd1e995; k ^= k >> 24; k *= 0x5bd1e995;
        h = h * 0x5bd1e995 ^ k; p += 4; n -= 4;
    }
    if (n == 3) h ^= (uint32_t)p[2] << 16;
    if (n >= 2) h ^= (uint32_t)p[1] << 8;
    if (n >= 1) { h ^= p[0]; h *= 0x5bd1e995; }
    h ^= h >> 13; h *= 0x5bd1e995; return h ^ (h >> 15);
}
static void broadcast(const char *db, const char *action, const char *subsystem) {
    if (monitor_fd < 0) return;
    struct {
        char prefix[8];
        uint32_t magic, header_size, properties_off, properties_len;
        uint32_t subsystem_hash, devtype_hash, tag_hi, tag_lo;
        char properties[DB_SIZE];
    } msg = {.prefix = "libudev", .magic = htonl(0xfeedcafe),
        .header_size = 40, .properties_off = 40,
        .subsystem_hash = htonl(wire_hash(subsystem))};
    size_t n = (size_t)snprintf(msg.properties, sizeof(msg.properties), "ACTION=%s", action) + 1;
    for (const char *p = db; *p;) {
        const char *end = strchr(p, '\n');
        if (!end) break;
        size_t len = (size_t)(end - p);
        if (len > 2 && p[0] == 'E' && p[1] == ':' && n + len - 1 < sizeof(msg.properties)) {
            memcpy(msg.properties + n, p + 2, len - 2);
            n += len - 2; msg.properties[n++] = 0;
        }
        p = end + 1;
    }
    uint32_t hash = wire_hash("seat");
    uint64_t bloom = (UINT64_C(1) << (hash & 63)) | (UINT64_C(1) << ((hash >> 6) & 63)) |
        (UINT64_C(1) << ((hash >> 12) & 63)) | (UINT64_C(1) << ((hash >> 18) & 63));
    if (strstr(db, "G:seat\n")) { msg.tag_hi = htonl(bloom >> 32); msg.tag_lo = htonl((uint32_t)bloom); }
    msg.properties_len = (uint32_t)n;
    struct sockaddr_nl dest = {.nl_family = AF_NETLINK, .nl_groups = 2};
    if (sendto(monitor_fd, &msg, 40 + n, MSG_DONTWAIT, (struct sockaddr *)&dest, sizeof(dest)) < 0 &&
        errno != ECONNREFUSED) {
        static bool reported;
        if (!reported) { perror("linuxd: udev multicast (SDL uses inotify)"); reported = true; }
    }
}

static bool publish(const char *id, const char *db, const char *subsystem, bool seat) {
    char path[PATH_MAX], tmp[PATH_MAX], old[DB_SIZE];
    snprintf(path, sizeof(path), MATON_UDEV_ROOT "/data/%s", id);
    bool exists = read_text(MATON_UDEV_ROOT "/data", id, old, sizeof(old));
    /* read_text strips the final newline. Compare without it. */
    size_t length = strlen(db);
    bool changed = !exists || strlen(old) + 1 != length || memcmp(old, db, length - 1);
    if (changed) {
        snprintf(tmp, sizeof(tmp), MATON_UDEV_ROOT "/data/.%s.tmp", id);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) return false;
        size_t offset = 0;
        while (offset < length) {
            ssize_t n = write(fd, db + offset, length - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            offset += (size_t)n;
        }
        bool ok = fchmod(fd, 0644) == 0;
        if (close(fd)) ok = false;
        if (offset != length || !ok || rename(tmp, path)) { unlink(tmp); return false; }
    }
    snprintf(path, sizeof(path), MATON_UDEV_ROOT "/tags/seat/%s", id);
    if (seat) {
        int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) return false;
        close(fd);
    } else unlink(path);
    if (changed) broadcast(db, exists ? "change" : "add", subsystem);
    return true;
}

static bool scan_class(const char *subsystem, Seen **seen) {
    char dirpath[PATH_MAX];
    snprintf(dirpath, sizeof(dirpath), MATON_SYS_ROOT "/class/%s", subsystem);
    DIR *dir = opendir(dirpath);
    if (!dir) return errno == ENOENT;
    struct dirent *entry;
    bool ok = true;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        bool input = !strcmp(subsystem, "input");
        if (input ? (strncmp(name, "event", 5) && strncmp(name, "js", 2)) :
            (!strcmp(subsystem, "hidraw") ? strncmp(name, "hidraw", 6) : strcmp(name, "uinput"))) continue;
        char link[PATH_MAX], node[PATH_MAX], dev[64], db[DB_SIZE] = {0};
        if (snprintf(link, sizeof(link), "%s/%s", dirpath, name) >= (int)sizeof(link)) { ok = false; continue; }
        unsigned major, minor;
        if (!realpath(link, node) || !read_text(node, "dev", dev, sizeof(dev)) ||
            sscanf(dev, "%u:%u", &major, &minor) != 2) { ok = false; continue; }
        Seen *item = calloc(1, sizeof(*item));
        if (!item) { ok = false; continue; }
        snprintf(item->id, sizeof(item->id), "c%u:%u", major, minor);
        item->next = *seen; *seen = item;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        append(db, "I:%llu\nE:DEVPATH=%s\nE:SUBSYSTEM=%s\nE:DEVNAME=/dev/%s%s\nE:MAJOR=%u\nE:MINOR=%u\n",
            (unsigned long long)now.tv_sec * 1000000 + now.tv_nsec / 1000,
            node + strlen(MATON_SYS_ROOT), subsystem, input ? "input/" : "", name, major, minor);
        if (input) {
            char parent[PATH_MAX];
            snprintf(parent, sizeof(parent), "%s", node);
            /* event/js capabilities live on the inputN parent. */
            char *slash = strrchr(parent, '/');
            if (slash) *slash = 0;
            classify(db, parent);
            append(db, "E:ID_SEAT=seat0\nE:TAGS=:seat:\nE:CURRENT_TAGS=:seat:\nG:seat\nQ:seat\n");
        }
        identity(db, node);
        /* Keep the initialization timestamp stable across refreshes. */
        char old[DB_SIZE];
        if (read_text(MATON_UDEV_ROOT "/data", item->id, old, sizeof(old)) && old[0] == 'I') {
            unsigned long long stamp;
            if (sscanf(old, "I:%llu", &stamp) == 1) {
                char *rest = strchr(db, '\n');
                char stable[DB_SIZE];
                snprintf(stable, sizeof(stable), "I:%llu%s", stamp, rest ? rest : "");
                strcpy(db, stable);
            }
        }
        if (!publish(item->id, db, subsystem, input)) ok = false;
    }
    closedir(dir);
    return ok;
}
static void refresh(void) {
    Seen *seen = NULL;
    bool ok = scan_class("input", &seen);
    ok = scan_class("hidraw", &seen) && ok;
    ok = scan_class("misc", &seen) && ok;
    static bool reported;
    if (!ok && !reported) fprintf(stderr, "linuxd: incomplete udev snapshot; preserving stale entries until retry\n");
    reported = !ok;
    DIR *dir = ok ? opendir(MATON_UDEV_ROOT "/data") : NULL;
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        if (entry->d_name[0] != 'c') continue;
        Seen *item;
        for (item = seen; item && strcmp(item->id, entry->d_name); item = item->next) {}
        if (item) continue;
        char path[PATH_MAX], old[DB_SIZE];
        if (read_text(MATON_UDEV_ROOT "/data", entry->d_name, old, sizeof(old))) {
            /* read_text strips newline; restore it for the wire conversion. */
            size_t n = strlen(old);
            if (n + 1 < sizeof(old)) { old[n] = '\n'; old[n+1] = 0; }
            const char *subsystem = strstr(old, "E:SUBSYSTEM=input\n") ? "input" :
                (strstr(old, "E:SUBSYSTEM=hidraw\n") ? "hidraw" : "misc");
            snprintf(path, sizeof(path), MATON_UDEV_ROOT "/data/%s", entry->d_name);
            unlink(path);
            snprintf(path, sizeof(path), MATON_UDEV_ROOT "/tags/seat/%s", entry->d_name);
            unlink(path);
            broadcast(old, "remove", subsystem);
        }
    }
    if (dir) closedir(dir);
    while (seen) { Seen *next = seen->next; free(seen); seen = next; }
}

static bool relevant(const char *buf, size_t n) {
    for (size_t pos = 0; pos < n;) {
        size_t len = strnlen(buf + pos, n - pos);
        if (len == n - pos) return false;
        const char *p = buf + pos;
        if (!strcmp(p, "SUBSYSTEM=input") || !strcmp(p, "SUBSYSTEM=hidraw") || !strcmp(p, "SUBSYSTEM=misc")) return true;
        pos += len + 1;
    }
    return false;
}
static void *watch(void *unused) {
    (void)unused;
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    for (;;) {
        struct pollfd pfd = {.fd = monitor_fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 2000);
        if (ready < 0 && errno == EINTR) continue;
        bool dirty = ready <= 0;
        if (ready > 0) {
            char buf[65536];
            struct sockaddr_nl sender = {0};
            socklen_t size = sizeof(sender);
            ssize_t n = recvfrom(monitor_fd, buf, sizeof(buf), MSG_DONTWAIT,
                (struct sockaddr *)&sender, &size);
            /* Accept only kernel messages, never user-supplied paths. */
            dirty = n < 0 || (sender.nl_pid == 0 && relevant(buf, (size_t)n));
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(monitor_fd); monitor_fd = -1; dirty = true;
            }
        }
        /* Retry sysfs/ueventd races and recover dropped events even when
         * unrelated kernel traffic keeps the socket continuously readable. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (dirty || now.tv_sec - last.tv_sec >= 2) { refresh(); last = now; }
    }
    return NULL;
}
static void start_once(void) {
    const char *dirs[] = {MATON_UDEV_ROOT, MATON_UDEV_ROOT "/data",
        MATON_UDEV_ROOT "/tags", MATON_UDEV_ROOT "/tags/seat"};
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        if ((mkdir(dirs[i], 0755) && errno != EEXIST) || chmod(dirs[i], 0755)) {
            perror("linuxd: udev database directory"); return;
        }
    }
    /* Compatibility existence marker, not a udevadm control server. */
    int fd = open(MATON_UDEV_ROOT "/control", O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) { perror("linuxd: udev control marker"); return; }
    close(fd);
    monitor_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (monitor_fd >= 0) {
        int bytes = 1024 * 1024;
        setsockopt(monitor_fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
        if (bind(monitor_fd, (struct sockaddr *)&address, sizeof(address))) {
            perror("linuxd: kernel uevent listener"); close(monitor_fd); monitor_fd = -1;
        }
    } else perror("linuxd: kernel uevent socket");
    /* Subscribe before scanning: queued hotplugs cannot be lost at startup. */
    refresh();
    pthread_t thread;
    int error = pthread_create(&thread, NULL, watch, NULL);
    if (error) { errno = error; perror("linuxd: udev worker"); }
    else pthread_detach(thread);
}
void maton_udev_start(void) { pthread_once(&started, start_once); }
