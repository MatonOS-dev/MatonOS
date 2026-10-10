/*
 * Identity of a generated stub calling linuxd directly ("install me",
 * "update me"). The stub names its package; binder gives its UID, which must
 * own /data/user/<user>/<package>. The installed APK's v3/v2 signer
 * certificate must be the per-device stub certificate the bridge handed
 * over, and the ref and remote come from that APK's own manifest.
 *
 * The certificate is read from the APK Signing Block without re-checking the
 * digests: PackageManager verified the APK when it was installed, and only
 * installd/system can write /data/app afterwards.
 */
#include "StubVerify.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SIGNER_PATH "/data/matonos/linux/store/stub-signer.der"
#define SIGNER_TEMP SIGNER_PATH ".tmp"
#define MAX_CERT 8192
#define MAX_APK (64L * 1024 * 1024)

static void set_error(char* error, size_t size, const char* message) {
    if (error && size) snprintf(error, size, "%s", message);
}

int stub_store_signer(const unsigned char* der, size_t length) {
    if (!der || length < 16 || length > MAX_CERT || der[0] != 0x30) { errno = EINVAL; return -1; }
    int fd = open(SIGNER_TEMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, der, length) == (ssize_t)length && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(SIGNER_TEMP, SIGNER_PATH)) { unlink(SIGNER_TEMP); return -1; }
    return 0;
}

static int read_signer(unsigned char* out, size_t* length) {
    int fd = open(SIGNER_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, MAX_CERT);
    close(fd);
    if (n <= 0) return -1;
    *length = (size_t)n;
    return 0;
}

/* Owner of /data/user/<user>/<package>: only installd sets it, so it ties
 * a package name to its UID without asking PackageManager. */
static int package_uid(int user, const char* package) {
    char path[400];
    struct stat st;
    if (snprintf(path, sizeof(path), "/data/user/%d/%s", user, package) >= (int)sizeof(path)) return -1;
    if (lstat(path, &st) || !S_ISDIR(st.st_mode)) return -1;
    return (int)st.st_uid;
}

int stub_runtime_uid(int uid) {
    return package_uid(uid / 100000, "org.matonos.linuxruntimes");
}

static int valid_package(const char* package) {
    size_t length = strlen(package);
    if (length < 10 || length > 255 || strncmp(package, "flatpak.", 8) || strstr(package, "..")) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = package[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '_')) return 0;
    }
    return 1;
}

/* /data/app/<random>/<package>-<random>/base.apk */
static int find_base_apk(const char* package, char* path, size_t size) {
    DIR* top = opendir("/data/app");
    if (!top) return -1;
    size_t plen = strlen(package);
    int found = -1;
    struct dirent* a;
    while (found && (a = readdir(top))) {
        if (a->d_name[0] == '.') continue;
        char dir[512];
        snprintf(dir, sizeof(dir), "/data/app/%s", a->d_name);
        DIR* inner = opendir(dir);
        if (!inner) continue;
        struct dirent* b;
        while ((b = readdir(inner))) {
            if (strncmp(b->d_name, package, plen) || b->d_name[plen] != '-') continue;
            if (snprintf(path, size, "%s/%s/base.apk", dir, b->d_name) < (int)size) found = 0;
            break;
        }
        closedir(inner);
    }
    closedir(top);
    return found;
}

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const unsigned char* p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

/* Length-prefixed (uint32) slice inside [*p, end); advances *p past it. */
static int slice(const unsigned char** p, const unsigned char* end,
        const unsigned char** begin, const unsigned char** stop) {
    if (end - *p < 4) return -1;
    uint32_t length = le32(*p);
    if ((uint64_t)(end - *p - 4) < length) return -1;
    *begin = *p + 4;
    *stop = *begin + length;
    *p = *stop;
    return 0;
}

/* The only signer's first certificate from a v3 (preferred) or v2 block. */
static int apk_signer(const unsigned char* apk, size_t size,
        const unsigned char** cert, size_t* cert_length) {
    if (size < 22) return -1;
    size_t lowest = size > 22 + 65535 ? size - 22 - 65535 : 0;
    size_t eocd = size - 22;
    while (le32(apk + eocd) != 0x06054b50) {
        if (eocd == lowest) return -1;
        eocd--;
    }
    uint64_t cd = le32(apk + eocd + 16);
    if (cd < 32 || cd > eocd || memcmp(apk + cd - 16, "APK Sig Block 42", 16)) return -1;
    uint64_t block_size = le64(apk + cd - 24);
    if (block_size < 24 || block_size > cd - 8) return -1;
    const unsigned char* block = apk + cd - block_size - 8;
    if (le64(block) != block_size) return -1;
    const unsigned char* pairs = block + 8;
    const unsigned char* pairs_end = apk + cd - 24;
    const unsigned char* v2 = NULL; const unsigned char* v2_end = NULL;
    const unsigned char* v3 = NULL; const unsigned char* v3_end = NULL;
    while (pairs_end - pairs >= 12) {
        uint64_t length = le64(pairs);
        if (length < 4 || length > (uint64_t)(pairs_end - pairs - 8)) return -1;
        uint32_t id = le32(pairs + 8);
        const unsigned char* value = pairs + 12;
        const unsigned char* value_end = pairs + 8 + length;
        if (id == 0xf05368c0u) { v3 = value; v3_end = value_end; }
        else if (id == 0x7109871au) { v2 = value; v2_end = value_end; }
        pairs = value_end;
    }
    const unsigned char* p = v3 ? v3 : v2;
    const unsigned char* end = v3 ? v3_end : v2_end;
    if (!p) return -1;
    const unsigned char *signers, *signers_end, *signer, *signer_end, *data, *data_end;
    const unsigned char *skip, *skip_end, *certs, *certs_end, *der, *der_end;
    if (slice(&p, end, &signers, &signers_end)) return -1;
    const unsigned char* s = signers;
    if (slice(&s, signers_end, &signer, &signer_end) || s != signers_end) return -1;  /* exactly one */
    const unsigned char* q = signer;
    if (slice(&q, signer_end, &data, &data_end)) return -1;
    const unsigned char* d = data;
    if (slice(&d, data_end, &skip, &skip_end) || slice(&d, data_end, &certs, &certs_end)) return -1;
    const unsigned char* c = certs;
    if (slice(&c, certs_end, &der, &der_end) || der_end == der) return -1;
    *cert = der;
    *cert_length = (size_t)(der_end - der);
    return 0;
}

/* Uncompressed AndroidManifest.xml from the APK (the generator stores it). */
static int apk_manifest(const unsigned char* apk, size_t size,
        const unsigned char** xml, size_t* xml_length) {
    if (size < 22) return -1;
    size_t lowest = size > 22 + 65535 ? size - 22 - 65535 : 0;
    size_t eocd = size - 22;
    while (le32(apk + eocd) != 0x06054b50) {
        if (eocd == lowest) return -1;
        eocd--;
    }
    size_t cd = le32(apk + eocd + 16), entries = apk[eocd + 10] | apk[eocd + 11] << 8;
    for (size_t n = 0; n < entries; n++) {
        if (cd + 46 > eocd || le32(apk + cd) != 0x02014b50) return -1;
        unsigned method = apk[cd + 10] | apk[cd + 11] << 8;
        uint32_t csize = le32(apk + cd + 20), usize = le32(apk + cd + 24);
        unsigned nlen = apk[cd + 28] | apk[cd + 29] << 8, xlen = apk[cd + 30] | apk[cd + 31] << 8,
                clen = apk[cd + 32] | apk[cd + 33] << 8;
        uint32_t local = le32(apk + cd + 42);
        if (cd + 46 + nlen > eocd) return -1;
        if (nlen == 19 && !memcmp(apk + cd + 46, "AndroidManifest.xml", 19)) {
            if (method != 0 || csize != usize || (uint64_t)local + 30 > size ||
                    le32(apk + local) != 0x04034b50) return -1;
            uint64_t data = (uint64_t)local + 30 + (apk[local + 26] | apk[local + 27] << 8) +
                    (apk[local + 28] | apk[local + 29] << 8);
            if (data + usize > size) return -1;
            *xml = apk + data;
            *xml_length = usize;
            return 0;
        }
        cd += 46 + nlen + xlen + clen;
    }
    return -1;
}

typedef struct { const unsigned char* base; size_t count; const unsigned char* offsets;
        const unsigned char* data; const unsigned char* end; int utf8; } Pool;

/* ASCII string i of the pool into out (refs, remotes and names are ASCII). */
static int pool_string(const Pool* pool, uint32_t i, char* out, size_t size) {
    if (i >= pool->count) return -1;
    const unsigned char* p = pool->data + le32(pool->offsets + 4 * i);
    if (p >= pool->end) return -1;
    size_t length;
    if (pool->utf8) {
        if (*p & 0x80) p++;                      /* character count */
        p++;
        if (p >= pool->end) return -1;
        length = *p & 0x7f;
        if (*p & 0x80) { if (p + 1 >= pool->end) return -1; length = length << 8 | p[1]; p++; }
        p++;
        if (length >= size || p + length > pool->end) return -1;
        for (size_t k = 0; k < length; k++) { if (p[k] & 0x80) return -1; out[k] = (char)p[k]; }
    } else {
        if (p + 2 > pool->end) return -1;
        length = p[0] | p[1] << 8;
        p += 2;
        if (length >= size || p + 2 * length > pool->end) return -1;
        for (size_t k = 0; k < length; k++) {
            if (p[2 * k + 1] || (p[2 * k] & 0x80)) return -1;
            out[k] = (char)p[2 * k];
        }
    }
    out[length] = '\0';
    return 0;
}

/* <manifest package> and the FLATPAK_REF / FLATPAK_REMOTE <meta-data>. */
static int manifest_values(const unsigned char* xml, size_t size, char* package, size_t package_size,
        char* ref, size_t ref_size, char* remote, size_t remote_size) {
    if (size < 8 || (xml[0] | xml[1] << 8) != 0x0003) return -1;
    const unsigned char* end = xml + size;
    const unsigned char* chunk = xml + (xml[2] | xml[3] << 8);
    Pool pool = {0};
    package[0] = ref[0] = remote[0] = '\0';
    while (chunk + 8 <= end) {
        unsigned type = chunk[0] | chunk[1] << 8, header = chunk[2] | chunk[3] << 8;
        uint32_t length = le32(chunk + 4);
        if (length < 8 || length > (size_t)(end - chunk)) return -1;
        if (type == 0x0001 && length >= 28) {
            pool.base = chunk;
            pool.count = le32(chunk + 8);
            pool.utf8 = (le32(chunk + 16) & 0x100) != 0;
            pool.offsets = chunk + header;
            pool.data = chunk + le32(chunk + 20);
            pool.end = chunk + length;
            if (pool.offsets + 4 * (uint64_t)pool.count > pool.end || pool.data > pool.end) return -1;
        } else if (type == 0x0102 && pool.base && length >= 36) {
            char element[32], name[64], value[1100];
            const unsigned char* ext = chunk + header;
            if (header < 16 || ext + 20 > chunk + length) return -1;
            unsigned attr_start = ext[8] | ext[9] << 8, attr_size = ext[10] | ext[11] << 8,
                    attr_count = ext[12] | ext[13] << 8;
            if (pool_string(&pool, le32(ext + 4), element, sizeof(element))) { chunk += length; continue; }
            int is_manifest = !strcmp(element, "manifest"), is_meta = !strcmp(element, "meta-data");
            char meta_name[128] = "", meta_value[1100] = "";
            for (unsigned a = 0; (is_manifest || is_meta) && a < attr_count; a++) {
                const unsigned char* attr = ext + attr_start + (size_t)a * attr_size;
                if (attr_size < 20 || attr + 20 > chunk + length) return -1;
                if (pool_string(&pool, le32(attr + 4), name, sizeof(name))) continue;
                uint32_t raw = le32(attr + 8);
                uint32_t index = raw != 0xffffffffu ? raw : (attr[15] == 0x03 ? le32(attr + 16) : 0xffffffffu);
                if (index == 0xffffffffu || pool_string(&pool, index, value, sizeof(value))) continue;
                if (is_manifest && !strcmp(name, "package")) snprintf(package, package_size, "%s", value);
                else if (is_meta && !strcmp(name, "name")) snprintf(meta_name, sizeof(meta_name), "%s", value);
                else if (is_meta && !strcmp(name, "value")) snprintf(meta_value, sizeof(meta_value), "%s", value);
            }
            if (is_meta && !strcmp(meta_name, "org.matonos.linuxhost.FLATPAK_REF") && !ref[0])
                snprintf(ref, ref_size, "%s", meta_value);
            if (is_meta && !strcmp(meta_name, "org.matonos.linuxhost.FLATPAK_REMOTE") && !remote[0])
                snprintf(remote, remote_size, "%s", meta_value);
        }
        chunk += length;
    }
    return package[0] && ref[0] ? 0 : -1;
}

int stub_verify_caller(int uid, const char* package, char* ref, size_t ref_size,
        char* remote, size_t remote_size, char* error, size_t error_size) {
    if (uid < 10000 || uid % 100000 < 10000 || uid % 100000 > 19999) {
        set_error(error, error_size, "caller is not an application"); return -1;
    }
    if (!package || !valid_package(package)) {
        set_error(error, error_size, "not a generated stub package name"); return -1;
    }
    if (package_uid(uid / 100000, package) != uid) {
        set_error(error, error_size, "package does not belong to the caller"); return -1;
    }
    unsigned char signer[MAX_CERT];
    size_t signer_length = 0;
    if (read_signer(signer, &signer_length)) {
        set_error(error, error_size, "stub signing certificate unknown"); return -1;
    }
    char path[1024];
    if (find_base_apk(package, path, sizeof(path))) {
        set_error(error, error_size, "stub APK not found"); return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 22 || st.st_size > MAX_APK) {
        if (fd >= 0) close(fd);
        set_error(error, error_size, "cannot read stub APK"); return -1;
    }
    void* map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { set_error(error, error_size, "cannot read stub APK"); return -1; }
    const unsigned char* cert = NULL;
    const unsigned char* xml = NULL;
    size_t cert_length = 0, xml_length = 0;
    char declared[300];
    const char* problem = NULL;
    if (apk_signer(map, (size_t)st.st_size, &cert, &cert_length) || cert_length != signer_length ||
            memcmp(cert, signer, signer_length))
        problem = "stub is not signed with the stub key";
    else if (apk_manifest(map, (size_t)st.st_size, &xml, &xml_length) ||
            manifest_values(xml, xml_length, declared, sizeof(declared), ref, ref_size, remote, remote_size))
        problem = "cannot read the stub manifest";
    else if (strcmp(declared, package))
        problem = "stub manifest names another package";
    munmap(map, (size_t)st.st_size);
    if (problem) { set_error(error, error_size, problem); return -1; }
    return 0;
}

int stub_uid_has_package(int uid) {
    char path[64]; snprintf(path, sizeof(path), "/data/user/%d", uid / 100000);
    DIR* dir = opendir(path); if (!dir) return -1;
    int found = 0; struct dirent* entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!valid_package(entry->d_name)) continue;
        char full[400]; struct stat st;
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (lstat(full, &st)) { found = -1; break; }
        if (S_ISDIR(st.st_mode) && package_uid(uid / 100000, entry->d_name) == uid) { found = 1; break; }
        errno = 0;
    }
    if (!entry && errno) found = -1;
    closedir(dir); return found;
}
