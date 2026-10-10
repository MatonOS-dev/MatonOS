#define _GNU_SOURCE
#include "RuntimeRefs.h"
#include "SafePath.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
struct Entry { char ref[513]; int* uids; size_t count, capacity; };
struct RuntimeRefs { int dir; unsigned user; size_t count, capacity; char name[64]; struct Entry* entries; char* data; size_t data_capacity; };
/* Geometric growth with checked element multiplication. */
static void* grow(void* array, size_t* capacity, size_t count, size_t element) {
    if (count <= *capacity) return array;
    if (count > SIZE_MAX / element) return NULL;
    size_t next = *capacity ? *capacity : 8;
    while (next < count) {
        if (next > SIZE_MAX / 2) { next = count; break; }
        next *= 2;
    }
    if (next > SIZE_MAX / element) next = count;
    void* memory = realloc(array, next * element);
    if (!memory) return NULL;
    *capacity = next; return memory;
}
static int append(char** data, size_t* used, size_t* capacity, const char* format, ...) {
    va_list args; va_start(args, format);
    va_list copy; va_copy(copy, args);
    int n = vsnprintf(NULL, 0, format, copy); va_end(copy);
    if (n < 0 || (size_t)n >= SIZE_MAX - *used) { va_end(args); return -1; }
    char* next = grow(*data, capacity, *used + (size_t)n + 1, 1);
    if (!next) { va_end(args); return -1; }
    *data = next;
    vsnprintf(*data + *used, *capacity - *used, format, args); va_end(args);
    *used += (size_t)n; return 0;
}
static int valid_uid(RuntimeRefs* db, int uid) {
    return uid >= 10000 && (unsigned)uid / 100000 == db->user && uid % 100000 >= 10000 && uid % 100000 <= 19999;
}
static int valid_ref(const char* ref) {
    if (strncmp(ref, "runtime/", 8) && strncmp(ref, "app/", 4)) return 0;
    if (strlen(ref) > 512) return 0;
    unsigned parts = 0, n = 0;
    for (const unsigned char* p = (const unsigned char*)ref; *p; p++) {
        if (*p == '/') { if (!n) return 0; parts++; n = 0; }
        else { if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-') return 0; n++; }
    }
    return parts == 3 && n && !strstr(ref, "/../") && !strstr(ref, "/./") &&
            strcmp(strrchr(ref, '/') + 1, ".") && strcmp(strrchr(ref, '/') + 1, "..");
}
static int ensure_ref(RuntimeRefs* db, const char* ref) {
    if (!db || !valid_ref(ref)) return -1;
    size_t i;
    for (i = 0; i < db->count; i++) if (!strcmp(ref, db->entries[i].ref)) break;
    if (i == db->count) {
        if (db->count == SIZE_MAX) return -1;
        struct Entry* next = grow(db->entries, &db->capacity, db->count + 1, sizeof(*db->entries));
        if (!next) return -1;
        db->entries = next;
        memset(&db->entries[i], 0, sizeof(db->entries[i]));
        strcpy(db->entries[i].ref, ref); db->count++;
    }
    return 0;
}
int runtime_refs_add(RuntimeRefs* db, const char* ref, int uid) {
    if (!db || !valid_uid(db, uid) || ensure_ref(db, ref)) return -1;
    size_t i;
    for (i = 0; i < db->count; i++) if (!strcmp(ref, db->entries[i].ref)) break;
    struct Entry* e = &db->entries[i];
    for (size_t j = 0; j < e->count; j++) if (e->uids[j] == uid) return 0;
    if (e->count == SIZE_MAX) return -1;
    int* next = grow(e->uids, &e->capacity, e->count + 1, sizeof(*e->uids));
    if (!next) return -1;
    e->uids = next;
    e->uids[e->count++] = uid; return 0;
}
static void space(char** p) { while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t') (*p)++; }
static int token(char** p, char c) { space(p); if (**p != c) return -1; (*p)++; return 0; }
static int parse(RuntimeRefs* db, char* p) {
    if (token(&p, '{')) return -1;
    space(&p);
    if (*p != '}') for (;;) {
        if (token(&p, '"')) return -1;
        char* ref = p;
        while (*p && *p != '"') p++;
        if (!*p) return -1;
        *p++ = 0;
        if (!valid_ref(ref)) return -1;
        for (size_t i = 0; i < db->count; i++) if (!strcmp(db->entries[i].ref, ref)) return -1;
        if (ensure_ref(db, ref)) return -1;
        struct Entry* e = &db->entries[db->count - 1];
        if (token(&p, ':') || token(&p, '[')) return -1;
        space(&p);
        if (*p != ']') for (;;) {
            if (*p < '1' || *p > '9') return -1;
            char* end; errno = 0; long uid = strtol(p, &end, 10);
            if (errno || uid > INT_MAX || !valid_uid(db, (int)uid)) return -1;
            for (size_t j = 0; j < e->count; j++) if (e->uids[j] == uid) return -1;
            if (e->count == SIZE_MAX) return -1;
            int* next = grow(e->uids, &e->capacity, e->count + 1, sizeof(*e->uids));
            if (!next) return -1;
            e->uids = next;
            e->uids[e->count++] = (int)uid; p = end; space(&p);
            if (*p != ',') break;
            p++; space(&p);
        }
        if (token(&p, ']')) return -1;
        space(&p); if (*p != ',') break;
        p++;
    }
    if (token(&p, '}')) return -1;
    space(&p); return *p ? -1 : 0;
}
static int serialize(RuntimeRefs* db, size_t* length) {
    size_t used = 0;
    if (append(&db->data, &used, &db->data_capacity, "{")) goto bad;
    for (size_t i = 0; i < db->count; i++) {
        struct Entry* e = &db->entries[i];
        if (append(&db->data, &used, &db->data_capacity, "%s\"%s\":[", i ? "," : "", e->ref)) goto bad;
        for (size_t j = 0; j < e->count; j++)
            if (append(&db->data, &used, &db->data_capacity, "%s%d", j ? "," : "", e->uids[j])) goto bad;
        if (append(&db->data, &used, &db->data_capacity, "]")) goto bad;
    }
    if (append(&db->data, &used, &db->data_capacity, "}\n")) goto bad;
    *length = used; return 0;
bad: return -1;
}
int runtime_refs_save(RuntimeRefs* db) {
    size_t used;
    if (serialize(db, &used)) return -1;
    int rc = safe_replace_at(db->dir, db->name, db->data, used);
    if (!rc) rc = fsync(db->dir);
    return rc;
}
RuntimeRefs* runtime_refs_open(const char* directory, unsigned user) {
    RuntimeRefs* db = calloc(1, sizeof(*db)); if (!db) return NULL;
    db->dir = -1; db->user = user;
    int root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) goto bad;
    db->dir = safe_directory_at(root, directory + 1, 0); close(root);
    if (db->dir < 0) goto bad;
    snprintf(db->name, sizeof(db->name), "refs-%u.json", user);
    int fd = openat(db->dir, db->name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) goto bad;
        return db;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 07777) != 0600 || st.st_size < 2 || (uintmax_t)st.st_size > SIZE_MAX - 2) { close(fd); goto bad; }
    char* data = malloc((size_t)st.st_size + 2);
    if (!data) { close(fd); goto bad; }
    size_t used = 0; ssize_t n;
    n = 0;
    while (used <= (size_t)st.st_size) {
        n = read(fd, data + used, (size_t)st.st_size + 1 - used);
        if (n > 0) used += (size_t)n;
        else if (n == 0 || errno != EINTR) break;
    }
    close(fd);
    int ok = used == (size_t)st.st_size && n == 0 && !memchr(data, 0, used);
    data[used] = 0;
    if (ok) ok = parse(db, data) == 0;
    free(data); if (!ok) goto bad;
    return db;
bad: runtime_refs_close(db); return NULL;
}
int runtime_refs_replace(RuntimeRefs* db, int uid, const char* const* refs, size_t count) {
    if (!db || !valid_uid(db, uid)) return -1;
    /* Require durable union protection before dropping any old ownership. */
    for (size_t k = 0; k < count; k++) {
        if (!valid_ref(refs[k])) return -1;
        int found = 0;
        for (size_t i = 0; i < db->count; i++) if (!strcmp(refs[k], db->entries[i].ref)) {
            for (size_t j = 0; j < db->entries[i].count; j++)
                if (db->entries[i].uids[j] == uid) found = 1;
        }
        if (!found) return -1;
    }
    for (size_t i = 0; i < db->count; i++) {
        struct Entry* e = &db->entries[i];
        int keep = 0;
        for (size_t k = 0; k < count; k++) if (!strcmp(e->ref, refs[k])) keep = 1;
        if (!keep) for (size_t j = 0; j < e->count;) {
            if (e->uids[j] == uid) e->uids[j] = e->uids[--e->count]; else j++;
        }
    }
    return runtime_refs_save(db);
}
int runtime_refs_remove(RuntimeRefs* db, int uid) {
    if (!valid_uid(db, uid)) return -1;
    for (size_t i = 0; i < db->count; i++) {
        struct Entry* e = &db->entries[i];
        for (size_t j = 0; j < e->count;) {
            if (e->uids[j] == uid) e->uids[j] = e->uids[--e->count]; else j++;
        }
    }
    return runtime_refs_save(db);
}
int runtime_refs_reconcile(RuntimeRefs* db, RuntimeRefsLive live, void* context) {
    for (size_t i = 0; i < db->count; i++) {
        struct Entry* e = &db->entries[i];
        for (size_t j = 0; j < e->count;) {
            int alive = live(e->uids[j], context); if (alive < 0) return -1;
            if (!alive) e->uids[j] = e->uids[--e->count]; else j++;
        }
    }
    return runtime_refs_save(db);
}
int runtime_refs_prune(RuntimeRefs* db, RuntimeRefsPrune prune, void* context) {
    /* Reserve serialization storage before any uninstall. Removing entries only
     * shrinks the serialized database, so later saves cannot need allocations. */
    size_t length;
    if (serialize(db, &length)) return -1;
    /* Probe all candidates first. A failed extension uninstall blocks bases. */
    if (db->count > SIZE_MAX / sizeof(int)) return -1;
    int* ext = calloc(db->count ? db->count : 1, sizeof(int)); if (!ext) return -1;
    for (size_t i = 0; i < db->count; i++) if (!db->entries[i].count && prune(db->entries[i].ref, &ext[i], context)) { free(ext); return -1; }
    for (int phase = 1; phase >= 0; phase--) {
        for (size_t i = 0; i < db->count; i++) {
            struct Entry* e = &db->entries[i];
            if (e->count || ext[i] != phase) continue;
            if (prune(e->ref, NULL, context)) { free(ext); return -1; }
            /* Persist each success; failures retain their empty entry for retry. */
            free(e->uids);
            memmove(e, e + 1, (db->count - i - 1) * sizeof(*e));
            memmove(ext + i, ext + i + 1, (db->count - i - 1) * sizeof(*ext));
            db->count--; i--;
            if (runtime_refs_save(db)) { free(ext); return -1; }
        }
    }
    free(ext); return 0;
}
void runtime_refs_close(RuntimeRefs* db) { if (db) { if (db->dir >= 0) close(db->dir); for (size_t i = 0; i < db->count; i++) free(db->entries[i].uids); free(db->entries); free(db->data); free(db); } }
