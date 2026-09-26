#define _GNU_SOURCE
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/loop.h>
#include <poll.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WAIT_MS 9000
#define SCAN_BUDGET_MS 8000
#define MAX_DEPTH 3
#define MAX_GPT_ENTRIES 256
#define GPT_ENTRY_MAX 4096
#define IMAGE_MIN_BYTES (128ULL * 1024 * 1024)

static char boot_uuid[37];
static int logfd = -1;
extern char **environ;

static void logmsg(const char *fmt, ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (logfd < 0) {
        logfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (logfd < 0) {
            mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
            logfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        }
    }
    if (logfd >= 0) {
        dprintf(logfd, "<6>matonos-ventoyboot: %s\n", msg);
    }
}

static uint64_t monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void sleep_ms(unsigned ms) {
    struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

static void setup_pseudofs(void) {
    mkdir("/dev", 0755); mkdir("/proc", 0755); mkdir("/sys", 0755);
    (void)mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    (void)mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    mkdir("/dev/block", 0755);
    mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
}

static void read_boot_uuid(void) {
    FILE *f = fopen("/proc/cmdline", "re");
    if (!f) return;
    char line[8192];
    if (fgets(line, sizeof(line), f)) {
        char *save = NULL;
        for (char *tok = strtok_r(line, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
            if (strncmp(tok, "androidboot.boot_part_uuid=", 27) == 0) {
                const char *v = tok + 27;
                if (strlen(v) == 36) {
                    for (int i = 0; i < 36; ++i) boot_uuid[i] = (char)tolower((unsigned char)v[i]);
                    boot_uuid[36] = 0;
                }
            }
        }
    }
    fclose(f);
}

static int sysfs_has_boot_uuid(void) {
    DIR *d = opendir("/sys/class/block");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", e->d_name);
        FILE *f = fopen(path, "re");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "PARTUUID=", 9)) {
                for (char *p = line + 9; *p; ++p) *p = (char)tolower((unsigned char)*p);
                line[strcspn(line, "\r\n")] = 0;
                if (!strcmp(line + 9, boot_uuid)) found = 1;
                break;
            }
        }
        fclose(f);
        if (found) break;
    }
    closedir(d);
    return found;
}

static int module_load(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) { close(fd); return -1; }
    if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return -1; }
    int rc = syscall(__NR_finit_module, fd, "", 0);
    int saved = errno;
    close(fd);
    if (rc && saved != EEXIST) { errno = saved; return -1; }
    return 0;
}

static int uuid_from_gpt(const unsigned char *p, char out[37]) {
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             p[3],p[2],p[1],p[0],p[5],p[4],p[7],p[6],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
    return 0;
}

static uint64_t le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static int image_has_uuid(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    unsigned char hdr[512];
    if (fstat(fd, &st) || (uint64_t)st.st_size < IMAGE_MIN_BYTES || pread(fd, hdr, sizeof(hdr), 512) != sizeof(hdr) || memcmp(hdr, "EFI PART", 8)) {
        close(fd); return 0;
    }
    uint64_t entries_lba = le64(hdr + 72);
    uint32_t count = (uint32_t)hdr[80] | (uint32_t)hdr[81] << 8 | (uint32_t)hdr[82] << 16 | (uint32_t)hdr[83] << 24;
    uint32_t size = (uint32_t)hdr[84] | (uint32_t)hdr[85] << 8 | (uint32_t)hdr[86] << 16 | (uint32_t)hdr[87] << 24;
    if (!count || count > MAX_GPT_ENTRIES || size < 128 || size > GPT_ENTRY_MAX || entries_lba > (uint64_t)st.st_size / 512) { close(fd); return 0; }
    int match = 0, has_super = 0;
    static const unsigned char efi_system_type[16] = {
        0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,
        0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b
    };
    unsigned char entry[GPT_ENTRY_MAX];
    for (uint32_t i = 0; i < count; ++i) {
        off_t off = (off_t)(entries_lba * 512 + (uint64_t)i * size);
        if ((uint64_t)off + size > (uint64_t)st.st_size || pread(fd, entry, size, off) != (ssize_t)size) break;
        if (entry[0] == 0 && entry[1] == 0) continue;
        char uuid[37];
        uuid_from_gpt(entry + 16, uuid);
        if (!strcmp(uuid, boot_uuid) && !memcmp(entry, efi_system_type, sizeof(efi_system_type))) match = 1;
        char name[37] = {0};
        for (int j = 0; j < 36; ++j) {
            unsigned c = entry[56 + j * 2] | (unsigned)entry[57 + j * 2] << 8;
            name[j] = (c < 128 && c >= 32) ? (char)tolower(c) : 0;
        }
        if (!strcmp(name, "super")) has_super = 1;
    }
    close(fd);
    return match && has_super;
}

static int ensure_node(const char *path, unsigned maj, unsigned min) {
    struct stat st;
    if (!stat(path, &st) && S_ISBLK(st.st_mode)) return 0;
    unlink(path);
    return mknod(path, S_IFBLK | 0600, makedev(maj, min));
}

static int mount_candidates(void) {
    DIR *d = opendir("/sys/class/block");
    if (!d) return -1;
    mkdir("/mnt", 0755); mkdir("/mnt/ventoyboot", 0700);
    struct dirent *e;
    int mounted = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char part[PATH_MAX], devpath[PATH_MAX], mnt[PATH_MAX], dev[PATH_MAX];
        snprintf(part, sizeof(part), "/sys/class/block/%s/partition", e->d_name);
        if (access(part, F_OK)) continue;
        snprintf(devpath, sizeof(devpath), "/sys/class/block/%s/dev", e->d_name);
        FILE *f = fopen(devpath, "re");
        unsigned maj = 0, min = 0;
        if (!f || fscanf(f, "%u:%u", &maj, &min) != 2) { if (f) fclose(f); continue; }
        fclose(f);
        snprintf(dev, sizeof(dev), "/dev/%s", e->d_name);
        if (ensure_node(dev, maj, min)) continue;
        snprintf(mnt, sizeof(mnt), "/mnt/ventoyboot/%s", e->d_name);
        mkdir(mnt, 0700);
        const char *types[] = { "exfat", "ntfs3", "vfat" };
        for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); ++i) {
            if (!mount(dev, mnt, types[i], MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) {
                mounted++;
                break;
            }
        }
    }
    closedir(d);
    return mounted;
}

static int find_image_at(const char *dir, int depth, char *out, size_t outsz) {
    if (depth > MAX_DEPTH) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "System Volume Information") || !strcmp(e->d_name, "$RECYCLE.BIN")) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISREG(st.st_mode) && (uint64_t)st.st_size >= IMAGE_MIN_BYTES && image_has_uuid(path)) {
            if (strlen(path) + 1 <= outsz) strcpy(out, path);
            closedir(d);
            return 1;
        }
        if (S_ISDIR(st.st_mode) && depth < MAX_DEPTH && find_image_at(path, depth + 1, out, outsz)) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static int locate_image(char *path, size_t n) {
    DIR *d = opendir("/mnt/ventoyboot");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char root[PATH_MAX];
        snprintf(root, sizeof(root), "/mnt/ventoyboot/%s", e->d_name);
        if (find_image_at(root, 0, path, n)) { found = 1; break; }
    }
    closedir(d);
    return found;
}

static int activate_image(const char *path) {
    if (access("/dev/loop-control", F_OK)) mknod("/dev/loop-control", S_IFCHR | 0600, makedev(10, 237));
    int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (ctl < 0) return -1;
    int idx = ioctl(ctl, LOOP_CTL_GET_FREE);
    close(ctl);
    if (idx < 0) return -1;
    char loop[64];
    snprintf(loop, sizeof(loop), "/dev/loop%d", idx);
    if (access(loop, F_OK)) mknod(loop, S_IFBLK | 0600, makedev(7, idx));
    int loopfd = open(loop, O_RDWR | O_CLOEXEC);
    int imagefd = open(path, O_RDONLY | O_CLOEXEC);
    if (loopfd < 0 || imagefd < 0) { if (loopfd >= 0) close(loopfd); if (imagefd >= 0) close(imagefd); return -1; }
    struct loop_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = (uint32_t)imagefd;
    cfg.block_size = 512;
    cfg.info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_PARTSCAN | LO_FLAGS_AUTOCLEAR;
    int rc = ioctl(loopfd, LOOP_CONFIGURE, &cfg);
    int saved = errno;
    close(loopfd); close(imagefd);
    if (rc) { errno = saved; return -1; }
    return idx;
}

static void start_android(void) {
    (void)umount2("/sys", MNT_DETACH);
    (void)umount2("/proc", MNT_DETACH);
    char *argv[] = { "/init", NULL };
    execve("/init.android", argv, environ);
    logmsg("could not exec saved Android init: %s", strerror(errno));
    _exit(127);
}

static int ventoy_work(void) {
    if (module_load("/ventoy/modules/nls_utf8.ko") && errno != EEXIST) logmsg("UTF-8 NLS module unavailable: %s", strerror(errno));
    if (module_load("/ventoy/modules/exfat.ko") && errno != EEXIST) logmsg("exFAT module unavailable: %s", strerror(errno));
    if (module_load("/ventoy/modules/ntfs3.ko") && errno != EEXIST) logmsg("NTFS3 module unavailable: %s", strerror(errno));

    int mounted = mount_candidates();
    char image[PATH_MAX] = {0};
    if (mounted <= 0 || !locate_image(image, sizeof(image))) {
        logmsg("no mounted Ventoy data partition contains a GPT image with requested PARTUUID");
        return 0;
    }

    logmsg("matched Ventoy image by embedded ESP PARTUUID: %s", image);
    int loop = activate_image(image);
    if (loop < 0) {
        logmsg("loop setup failed: %s", strerror(errno));
        return 0;
    }
    logmsg("attached image to /dev/loop%d with read-only partition scan", loop);
    uint64_t wait_for_part = monotonic_ms() + 3000;
    while (monotonic_ms() < wait_for_part && !sysfs_has_boot_uuid()) sleep_ms(100);
    if (sysfs_has_boot_uuid()) {
        logmsg("embedded boot PARTUUID appeared; continuing Android first-stage init");
        return 1;
    }
    DIR *d = opendir("/sys/class/block");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "loop", 4)) continue;
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", e->d_name);
            FILE *f = fopen(path, "re");
            if (!f) continue;
            char line[256];
            char details[512] = {0};
            while (fgets(line, sizeof(line), f)) {
                if (!strncmp(line, "PARTUUID=", 9) || !strncmp(line, "PARTNAME=", 9) || !strncmp(line, "DEVTYPE=", 8)) {
                    size_t used = strlen(details);
                    snprintf(details + used, sizeof(details) - used, "%s", line);
                }
            }
            fclose(f);
            if (details[0]) logmsg("%s uevent: %s", e->d_name, details);
        }
        closedir(d);
    }
    logmsg("loop partition scan did not expose requested PARTUUID; continuing Android init for fallback");
    return 0;
}

int main(void) {
    setup_pseudofs();
    read_boot_uuid();
    if (!boot_uuid[0]) {
        logmsg("boot_part_uuid missing; continuing normal boot");
        start_android();
    }
    if (sysfs_has_boot_uuid()) start_android();

    uint64_t until = monotonic_ms() + WAIT_MS;
    while (monotonic_ms() < until) {
        if (sysfs_has_boot_uuid()) start_android();
        sleep_ms(100);
    }
    if (sysfs_has_boot_uuid()) start_android();

    logmsg("boot PARTUUID %s absent after %d ms; probing Ventoy data partitions", boot_uuid, WAIT_MS);
    int result_pipe[2];
    if (pipe2(result_pipe, O_CLOEXEC)) {
        logmsg("could not start bounded Ventoy scan: %s", strerror(errno));
        start_android();
    }
    pid_t worker = fork();
    if (worker == 0) {
        close(result_pipe[0]);
        int result = ventoy_work();
        (void)write(result_pipe[1], &result, sizeof(result));
        _exit(0);
    }
    close(result_pipe[1]);
    if (worker < 0) {
        close(result_pipe[0]);
        logmsg("could not fork bounded Ventoy scan: %s", strerror(errno));
        start_android();
    }
    struct pollfd pfd = { .fd = result_pipe[0], .events = POLLIN };
    int wait_ms = (int)SCAN_BUDGET_MS;
    int rc;
    do { rc = poll(&pfd, 1, wait_ms); } while (rc < 0 && errno == EINTR);
    int result = 0;
    ssize_t got = rc > 0 ? read(result_pipe[0], &result, sizeof(result)) : -1;
    close(result_pipe[0]);
    if (got != sizeof(result)) {
        (void)kill(worker, SIGKILL);
        logmsg("Ventoy scan exceeded %d ms or failed; continuing Android init", SCAN_BUDGET_MS);
    } else if (result) {
        logmsg("Ventoy image attached; starting Android first-stage init");
    }
    (void)waitpid(worker, NULL, WNOHANG);
    start_android();
}
