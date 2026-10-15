#ifndef MATON_STAGED_VERITY_H
#define MATON_STAGED_VERITY_H
/* Immutable regular-file boundary only. This does NOT authenticate content,
 * capture directory entries, sanitize extra-data, or authorize publication.
 * Callers must own the inode and directory namespace before using this API.
 * Keep in this header so the fixed linuxd Soong source set does not change. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fsverity.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Injectable solely for the host syscall contract tests; production always
 * issues the real ioctl. No runtime switch or copy fallback exists. */
#ifndef MATON_VERITY_IOCTL
#define MATON_VERITY_IOCTL ioctl
#endif

static inline const char* maton_verity_error(int code) {
    if (code == EOPNOTSUPP || code == ENOTTY)
        return "fs-verity unsupported on /data";
    if (code == ETXTBSY)
        return "staged file still open for writing";
    return "cannot seal staged file with fs-verity";
}

/* Returns a pinned, read-only, measured fd on success. The caller reads and
 * authenticates through this SAME fd, then closes it. The verity digest is
 * intentionally not compared with an OSTree checksum: the two hash different
 * structures. OSTree authentication must still hash the sealed file's bytes.
 * EEXIST is accepted only after measuring an existing SHA-256 seal. */
static inline int maton_verity_openat(int dir, const char* name, dev_t device,
        uid_t owner, char* error, size_t error_size) {
    if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        if (error && error_size) snprintf(error, error_size, "invalid staging file component");
        errno = EINVAL;
        return -1;
    }
    struct stat st, before;
    int fd = -1;
    int saved;
    if (fstatat(dir, name, &before, AT_SYMLINK_NOFOLLOW)) {
        saved = errno; goto bad;
    }
    /* Do not even open a special file; device opens can have side effects. */
    if (!S_ISREG(before.st_mode) || before.st_dev != device || before.st_uid != owner) {
        saved = EPERM; goto bad;
    }
    fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { saved = errno; goto bad; }
    if (fstat(fd, &st)) { saved = errno; goto close_bad; }
    if (!S_ISREG(st.st_mode) || st.st_dev != device || st.st_uid != owner ||
            st.st_ino != before.st_ino) {
        saved = EPERM; goto close_bad;
    }
    struct fsverity_enable_arg enable = {0};
    enable.version = 1;
    enable.hash_algorithm = FS_VERITY_HASH_ALG_SHA256;
    enable.block_size = 4096;
    if (MATON_VERITY_IOCTL(fd, FS_IOC_ENABLE_VERITY, &enable) && errno != EEXIST) {
        saved = errno; goto close_bad;
    }
    /* fsverity_digest has a flexible array. Use an aligned buffer rather
     * than embedding that type inside another struct (not valid C++). */
    union {
        uint64_t align;
        unsigned char bytes[sizeof(struct fsverity_digest) + 32];
    } measured = {0};
    struct fsverity_digest* digest = (struct fsverity_digest*)measured.bytes;
    digest->digest_size = 32;
    if (MATON_VERITY_IOCTL(fd, FS_IOC_MEASURE_VERITY, digest)) {
        saved = errno; goto close_bad;
    }
    if (digest->digest_algorithm != FS_VERITY_HASH_ALG_SHA256 || digest->digest_size != 32) {
        saved = EINVAL; goto close_bad;
    }
    return fd;
close_bad:
    close(fd);
bad:
    if (error && error_size) snprintf(error, error_size, "%s", maton_verity_error(saved));
    errno = saved;
    return -1;
}
#endif
