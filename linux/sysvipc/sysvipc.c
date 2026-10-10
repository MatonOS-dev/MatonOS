/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "sysvipc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <linux/sem.h>
#include <linux/shm.h>
#include <linux/msg.h>
#include <linux/memfd.h>
#ifndef SHM_DEST
#define SHM_DEST 01000
#define SHM_LOCKED 02000
#endif
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef SECCOMP_ADDFD_FLAG_SEND
#define SECCOMP_ADDFD_FLAG_SEND 2
#endif
#define WAIT_RESULT INT64_MIN
#define SEM_MAX 32767
#define SEM_OPS 500
#define SEM_SETS 32000
#define SEM_PER_SET 32000
#define MSG_MAX 8192
#define MSG_BYTES 16384
#define MSG_SETS 32000
#define SHM_SETS 4096

enum kind { KSEM, KSHM, KMSG };
enum operation { O_BAD, O_SEMGET, O_SEMOP, O_SEMTIMEDOP, O_SEMCTL,
    O_SHMGET, O_SHMAT, O_SHMDT, O_SHMCTL, O_MSGGET, O_MSGSND, O_MSGRCV, O_MSGCTL };
struct caller {
    pid_t pid, tgid;
    uid_t uid;
    gid_t gid;
    gid_t *groups;
    size_t ngroups;
    uint64_t caps;
};
struct message { struct message *next; int64_t type; size_t size; unsigned char data[]; };
struct object {
    struct object *next;
    enum kind kind;
    int id, index;
    int32_t key;
    uid_t uid, cuid;
    gid_t gid, cgid;
    unsigned mode;
    bool removed;
    int64_t ctime, atime, dtime;
    pid_t cpid, lpid;
    size_t count, size, bytes, qbytes;
    int fd;
    void *locked;
    size_t locked_size;
    uid_t lock_uid;
    unsigned short *values;
    pid_t *pids;
    struct message *messages;
};
struct undo { struct undo *next; int id; size_t n; int *adj; };
struct attachment { struct attachment *next; int id, token; uint64_t addr; bool committed; };
struct process { struct process *next; pid_t pid; int pidfd; struct undo *undo; struct attachment *attachments; };
struct request {
    struct request *next;
    struct seccomp_notif notif;
    struct caller caller;
    enum operation op;
    bool compat, old, time64, multiplex;
    uint64_t a[6];
    int listener; /* the listener this notification arrived on */
    struct sembuf *ops;
    size_t nops, blocked;
    int64_t deadline;
    struct message *message;
};
struct listener { int fd; bool owned; };
struct maton_sysvipc {
    int wake;
    /* Serving-thread state: every listener of the app's sessions. */
    struct listener *listeners;
    size_t nlisteners;
    /* Handed over by maton_sysvipc_add_listener from other threads. */
    pthread_mutex_t lock;
    int *incoming;
    size_t nincoming;
    bool stopping;
    uint32_t nextid[3];
    struct object *objects;
    struct process *processes;
    struct request *pending;
};

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static int64_t walltime(void) { return (int64_t)time(NULL); }
static bool valid(struct maton_sysvipc *s, struct request *r) {
    (void)s; return ioctl(r->listener, SECCOMP_IOCTL_NOTIF_ID_VALID, &r->notif.id) == 0;
}
static bool memory(struct maton_sysvipc *s, struct request *r, uint64_t addr,
        void *buf, size_t len, bool write) {
    if (!valid(s, r)) { errno = ESRCH; return false; }
    if (!len) return true;
    if (addr > UINTPTR_MAX || len > UINTPTR_MAX - addr || (r->compat &&
            (addr > UINT32_MAX || len > (uint64_t)UINT32_MAX + 1 - addr))) {
        errno = EFAULT; return false;
    }
    struct iovec local = { buf, len }, remote = { (void *)(uintptr_t)addr, len };
    ssize_t n = write ? process_vm_writev(r->caller.pid, &local, 1, &remote, 1, 0) :
        process_vm_readv(r->caller.pid, &local, 1, &remote, 1, 0);
    if (n != (ssize_t)len) { errno = EFAULT; return false; }
    /* The caller may have died or received a signal while copying. */
    if (!valid(s, r)) { errno = ESRCH; return false; }
    return true;
}
static bool readmem(struct maton_sysvipc *s, struct request *r, uint64_t a, void *p, size_t n) {
    return memory(s, r, a, p, n, false);
}
static bool writemem(struct maton_sysvipc *s, struct request *r, uint64_t a, void *p, size_t n) {
    return memory(s, r, a, p, n, true);
}
static void reply(struct maton_sysvipc *s, struct request *r, int64_t result) {
    struct seccomp_notif_resp resp = { .id = r->notif.id };
    if (result < 0) resp.error = (int)result;
    else resp.val = result;
    (void)s; (void)ioctl(r->listener, SECCOMP_IOCTL_NOTIF_SEND, &resp);
}
static void request_free(struct request *r) {
    free(r->caller.groups); free(r->ops); free(r->message); free(r);
}
static bool credentials(struct caller *c, pid_t pid) {
    char path[64], *line = NULL;
    size_t cap = 0;
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "re");
    if (!f) return false;
    c->pid = pid;
    bool uid = false, gid = false, tgid = false, ok = true;
    while (getline(&line, &cap, f) >= 0) {
        unsigned real, effective, saved, fs;
        if (sscanf(line, "Uid: %u %u %u %u", &real, &effective, &saved, &fs) == 4) {
            c->uid = effective; uid = true;
        } else if (sscanf(line, "Gid: %u %u %u %u", &real, &effective, &saved, &fs) == 4) {
            c->gid = effective; gid = true;
        } else if (sscanf(line, "Tgid: %d", &c->tgid) == 1) tgid = true;
        else if (!strncmp(line, "CapEff:", 7)) c->caps = strtoull(line + 7, NULL, 16);
        else if (!strncmp(line, "Groups:", 7)) {
            char *p = line + 7, *end;
            for (;;) {
                unsigned long g = strtoul(p, &end, 10);
                if (end == p) break;
                if (c->ngroups == SIZE_MAX / sizeof(gid_t)) { ok = false; break; }
                gid_t *gs = realloc(c->groups, (c->ngroups + 1) * sizeof(gid_t));
                if (!gs) { ok = false; break; }
                c->groups = gs; c->groups[c->ngroups++] = (gid_t)g; p = end;
            }
        }
    }
    free(line); fclose(f);
    return ok && uid && gid && tgid;
}
static bool capability(struct caller *c, unsigned bit) { return !!(c->caps & (UINT64_C(1) << bit)); }
static bool in_group(struct caller *c, gid_t g) {
    if (c->gid == g) return true;
    for (size_t i = 0; i < c->ngroups; i++) if (c->groups[i] == g) return true;
    return false;
}
static bool access_ok(struct caller *c, struct object *o, unsigned needed) {
    unsigned mode = o->mode;
    if (c->uid == o->uid || c->uid == o->cuid) mode >>= 6;
    else if (in_group(c, o->gid) || in_group(c, o->cgid)) mode >>= 3;
    return (mode & needed) == needed || capability(c, 15); /* CAP_IPC_OWNER */
}
static bool owner(struct caller *c, struct object *o) {
    return c->uid == o->uid || c->uid == o->cuid || capability(c, 21); /* CAP_SYS_ADMIN */
}
static struct object *find(struct maton_sysvipc *s, enum kind k, int id) {
    for (struct object *o = s->objects; o; o = o->next)
        if (o->kind == k && o->id == id) return o;
    return NULL;
}
static size_t num_objects(struct maton_sysvipc *s, enum kind k) {
    size_t n = 0;
    for (struct object *o = s->objects; o; o = o->next) if (o->kind == k) n++;
    return n;
}
static void object_free(struct object *o) {
    if (o->locked) munmap(o->locked, o->locked_size);
    if (o->fd >= 0) close(o->fd);
    free(o->values); free(o->pids);
    while (o->messages) { struct message *m = o->messages; o->messages = m->next; free(m); }
    free(o);
}
static void remove_object(struct maton_sysvipc *s, struct object *o) {
    if (o->kind == KSEM) for (struct process *proc = s->processes; proc; proc = proc->next) {
        struct undo **up = &proc->undo;
        while (*up) {
            struct undo *u = *up;
            if (u->id == o->id) { *up = u->next; free(u->adj); free(u); }
            else up = &u->next;
        }
    }
    struct object **p = &s->objects;
    while (*p && *p != o) p = &(*p)->next;
    if (*p) *p = o->next;
    object_free(o);
}
static struct process *process_get(struct maton_sysvipc *s, pid_t pid) {
    for (struct process *p = s->processes; p; p = p->next) if (p->pid == pid) return p;
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) return NULL;
    struct process *p = calloc(1, sizeof(*p));
    if (!p) { close(fd); return NULL; }
    p->pid = pid; p->pidfd = fd; p->next = s->processes; s->processes = p;
    return p;
}
static struct undo *undo_get(struct process *p, struct object *o) {
    for (struct undo *u = p->undo; u; u = u->next) if (u->id == o->id) return u;
    struct undo *u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->adj = calloc(o->count, sizeof(int));
    if (!u->adj) { free(u); return NULL; }
    u->id = o->id; u->n = o->count; u->next = p->undo; p->undo = u;
    return u;
}
static void clear_undo(struct maton_sysvipc *s, int id, int index) {
    for (struct process *p = s->processes; p; p = p->next)
        for (struct undo *u = p->undo; u; u = u->next) if (u->id == id) {
            if (index >= 0) u->adj[index] = 0;
            else memset(u->adj, 0, u->n * sizeof(int));
        }
}
static void detach(struct maton_sysvipc *s, struct process *p, struct attachment **a) {
    struct attachment *old = *a;
    struct object *o = find(s, KSHM, old->id);
    if (o) {
        if (o->count) o->count--;
        if (old->committed) { o->dtime = walltime(); o->lpid = p->pid; }
        if (o->removed && !o->count) remove_object(s, o);
    }
    *a = old->next; free(old);
}
static bool reap_processes(struct maton_sysvipc *s) {
    bool changed = false;
    struct process **pp = &s->processes;
    while (*pp) {
        struct process *p = *pp;
        struct pollfd fd = { .fd = p->pidfd, .events = POLLIN };
        if (poll(&fd, 1, 0) <= 0) { pp = &p->next; continue; }
        for (struct undo *u = p->undo; u; u = u->next) {
            struct object *o = find(s, KSEM, u->id);
            if (!o) continue;
            for (size_t i = 0; i < u->n; i++) if (u->adj[i]) {
                int n = (int)o->values[i] + u->adj[i];
                o->values[i] = (unsigned short)(n < 0 ? 0 : n > SEM_MAX ? SEM_MAX : n);
                o->pids[i] = p->pid; o->atime = walltime();
            }
        }
        while (p->undo) { struct undo *u = p->undo; p->undo = u->next; free(u->adj); free(u); }
        while (p->attachments) detach(s, p, &p->attachments);
        *pp = p->next; close(p->pidfd); free(p); changed = true;
    }
    return changed;
}

static uint64_t getnum(const unsigned char *b, size_t offset, size_t size) {
    uint64_t v = 0; memcpy(&v, b + offset, size); return v;
}
static void putnum(unsigned char *b, size_t offset, size_t size, uint64_t v) {
    memcpy(b + offset, &v, size);
}
static size_t ds_size(struct request *r, enum kind k) {
    if (!r->compat) return k == KSEM ? 104 : k == KSHM ? 112 : 120;
    if (r->old) return k == KSEM ? 44 : k == KSHM ? 48 : 56;
    return k == KSEM ? 64 : k == KSHM ? 84 : 88;
}
static void pack_perm(unsigned char *b, struct object *o, bool old) {
    putnum(b, 0, 4, (uint32_t)o->key);
    if (old) {
        putnum(b, 4, 2, o->uid > 65535 ? 65534 : o->uid);
        putnum(b, 6, 2, o->gid > 65535 ? 65534 : o->gid);
        putnum(b, 8, 2, o->cuid > 65535 ? 65534 : o->cuid);
        putnum(b, 10, 2, o->cgid > 65535 ? 65534 : o->cgid);
        putnum(b, 12, 2, o->mode); putnum(b, 14, 2, (unsigned)o->id >> 15);
    } else {
        putnum(b, 4, 4, o->uid); putnum(b, 8, 4, o->gid);
        putnum(b, 12, 4, o->cuid); putnum(b, 16, 4, o->cgid);
        putnum(b, 20, 4, o->mode); putnum(b, 24, 2, (unsigned)o->id >> 15);
    }
}
static int stat_ds(struct maton_sysvipc *s, struct request *r, struct object *o, uint64_t addr) {
    unsigned char b[120] = {0};
    bool old = r->compat && r->old;
    pack_perm(b, o, old);
    if (old) {
        if (o->kind == KSEM) {
            putnum(b, 16, 4, o->atime); putnum(b, 20, 4, o->ctime); putnum(b, 40, 2, o->count);
        } else if (o->kind == KSHM) {
            putnum(b, 16, 4, o->size); putnum(b, 20, 4, o->atime);
            putnum(b, 24, 4, o->dtime); putnum(b, 28, 4, o->ctime);
            putnum(b, 32, 2, o->cpid); putnum(b, 34, 2, o->lpid); putnum(b, 36, 2, o->count);
        } else {
            putnum(b, 24, 4, o->atime); putnum(b, 28, 4, o->dtime); putnum(b, 32, 4, o->ctime);
            putnum(b, 36, 4, o->bytes); putnum(b, 40, 4, o->qbytes);
            putnum(b, 44, 2, o->bytes); putnum(b, 46, 2, o->count); putnum(b, 48, 2, o->qbytes);
            putnum(b, 50, 2, o->cpid); putnum(b, 52, 2, o->lpid);
        }
    } else {
        size_t p = r->compat ? 36 : 48, w = r->compat ? 4 : 8;
        if (o->kind == KSEM) {
            putnum(b, p, 8, o->atime); putnum(b, p + (r->compat ? 8 : 16), 8, o->ctime);
            putnum(b, p + (r->compat ? 16 : 32), w, o->count);
        } else if (o->kind == KSHM) {
            putnum(b, p, w, o->size); p += w;
            putnum(b, p, 8, o->atime); putnum(b, p + 8, 8, o->dtime); putnum(b, p + 16, 8, o->ctime);
            putnum(b, p + 24, 4, o->cpid); putnum(b, p + 28, 4, o->lpid); putnum(b, p + 32, w, o->count);
        } else {
            putnum(b, p, 8, o->atime); putnum(b, p + 8, 8, o->dtime); putnum(b, p + 16, 8, o->ctime);
            putnum(b, p + 24, w, o->bytes); putnum(b, p + 24 + w, w, o->count);
            putnum(b, p + 24 + 2*w, w, o->qbytes); putnum(b, p + 24 + 3*w, 4, o->cpid);
            putnum(b, p + 28 + 3*w, 4, o->lpid);
        }
    }
    return writemem(s, r, addr, b, ds_size(r, o->kind)) ? 0 : -EFAULT;
}
static int set_ds(struct maton_sysvipc *s, struct request *r, struct object *o, uint64_t addr) {
    unsigned char b[120];
    if (!readmem(s, r, addr, b, ds_size(r, o->kind))) return -EFAULT;
    bool old = r->compat && r->old;
    uid_t uid = (uid_t)getnum(b, 4, old ? 2 : 4);
    gid_t gid = (gid_t)getnum(b, old ? 6 : 8, old ? 2 : 4);
    unsigned mode = (unsigned)getnum(b, old ? 12 : 20, old ? 2 : 4);
    if (uid == UINT32_MAX || gid == UINT32_MAX) return -EINVAL;
    if (o->kind == KMSG) {
        size_t qbytes = old ? (size_t)getnum(b, 48, 2) :
            (size_t)getnum(b, r->compat ? 68 : 88, r->compat ? 4 : 8);
        if (old && !qbytes) qbytes = (size_t)getnum(b, 40, 4);
        if (qbytes > MSG_BYTES && !capability(&r->caller, 24)) return -EPERM;
        o->qbytes = qbytes;
    }
    o->uid = uid; o->gid = gid; o->mode = (o->mode & ~0777u) | (mode & 0777);
    o->ctime = walltime(); return 0;
}
static int64_t info(struct maton_sysvipc *s, struct request *r, enum kind k, int cmd, uint64_t addr) {
    unsigned char b[120] = {0};
    size_t n = num_objects(s, k), total = 0, bytes = 0, len = 0;
    int maxid = 0;
    for (struct object *o = s->objects; o; o = o->next) if (o->kind == k) {
        if (o->index > maxid) maxid = o->index;
        total += o->count; bytes += k == KSHM ? (o->size + (size_t)getpagesize()-1)/(size_t)getpagesize() : o->bytes;
    }
    if (k == KSEM) {
        int vals[] = { SEM_SETS*SEM_PER_SET, SEM_SETS, SEM_SETS*SEM_PER_SET,
            SEM_SETS*SEM_PER_SET, SEM_PER_SET, SEM_OPS, SEM_OPS,
            cmd == SEM_INFO ? (int)n : 20, SEM_MAX, cmd == SEM_INFO ? (int)total : SEM_MAX };
        memcpy(b, vals, sizeof(vals)); len = sizeof(vals);
    } else if (k == KMSG) {
        int vals[] = { cmd == MSG_INFO ? (int)n : MSG_SETS*MSG_BYTES/1024,
            cmd == MSG_INFO ? (int)total : MSG_BYTES, MSG_MAX, MSG_BYTES, MSG_SETS, 16,
            cmd == MSG_INFO ? (int)bytes : MSG_BYTES };
        memcpy(b, vals, sizeof(vals)); putnum(b, 28, 2, 65535); len = 32;
    } else if (cmd == SHM_INFO) {
        size_t w = r->compat ? 4 : 8, p = r->compat ? 4 : 8;
        putnum(b, 0, 4, n); putnum(b, p, w, bytes);
        /* Resident/swapped pages cannot be known from memfd metadata. */
        len = p + 5*w;
    } else if (r->compat && r->old) {
        uint32_t vals[] = { INT_MAX, 1, SHM_SETS, SHM_SETS, UINT32_MAX - (1u << 24) };
        memcpy(b, vals, sizeof(vals)); len = sizeof(vals);
    } else {
        size_t w = r->compat ? 4 : 8;
        uint64_t limit = (r->compat ? UINT32_MAX : UINT64_MAX) - (UINT64_C(1)<<24);
        uint64_t vals[] = { limit, 1, SHM_SETS, SHM_SETS, limit };
        for (size_t i = 0; i < 5; i++) putnum(b, i*w, w, vals[i]);
        len = 9*w;
    }
    return writemem(s, r, addr, b, len) ? maxid : -EFAULT;
}

static int64_t get_object(struct maton_sysvipc *s, struct request *r, enum kind k) {
    int32_t key = (int32_t)r->a[0];
    size_t size = k == KMSG ? 0 : r->a[1];
    unsigned flags = (unsigned)r->a[k == KMSG ? 1 : 2];
    if (k == KSEM && (int64_t)size < 0) return -EINVAL;
    if (k == KSEM && size > SEM_PER_SET) return -EINVAL;
    if (key != IPC_PRIVATE) for (struct object *o = s->objects; o; o = o->next) {
        if (o->kind != k || o->key != key || o->removed) continue;
        if ((flags & (IPC_CREAT | IPC_EXCL)) == (IPC_CREAT | IPC_EXCL)) return -EEXIST;
        if (!access_ok(&r->caller, o, ((flags >> 6) | (flags >> 3) | flags) & 7)) return -EACCES;
        if ((k == KSEM && size > o->count) || (k == KSHM && size > o->size)) return -EINVAL;
        return o->id;
    }
    if (key != IPC_PRIVATE && !(flags & IPC_CREAT)) return -ENOENT;
    if (k != KMSG && !size) return -EINVAL;
    size_t limit = k == KSEM ? SEM_SETS : k == KSHM ? SHM_SETS : MSG_SETS;
    if (num_objects(s, k) >= limit) return -ENOSPC;
    if (k == KSHM && size > (uint64_t)INT64_MAX - (size_t)getpagesize()) return -EINVAL;
    if (k == KSHM && (flags & SHM_HUGETLB)) return -EINVAL;
    struct object *o = calloc(1, sizeof(*o));
    if (!o) return -ENOMEM;
    o->fd = -1; o->kind = k; o->key = key; o->mode = flags & 0777;
    o->uid = o->cuid = r->caller.uid; o->gid = o->cgid = r->caller.gid;
    o->cpid = r->caller.tgid; o->ctime = walltime();
    if (k == KSEM) {
        o->count = size; o->values = calloc(size, sizeof(unsigned short)); o->pids = calloc(size, sizeof(pid_t));
        if (!o->values || !o->pids) { object_free(o); return -ENOMEM; }
    } else if (k == KSHM) {
        o->size = size;
        size_t page = (size_t)getpagesize(), rounded = (size + page - 1) & ~(page - 1);
        o->fd = (int)syscall(SYS_memfd_create, "maton-sysvipc", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (o->fd < 0) { int e = errno; object_free(o); return -e; }
        if (ftruncate(o->fd, (off_t)rounded) || fcntl(o->fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) {
            int e = errno; object_free(o); return -e;
        }
    } else o->qbytes = MSG_BYTES;
    /* Reusable index plus sequence, so stale IDs cannot name a replacement. */
    for (;;) {
        uint32_t id = s->nextid[k]++ & INT_MAX;
        int index = (int)(id & 32767);
        bool used = false;
        for (struct object *other = s->objects; other; other = other->next)
            if (other->kind == k && other->index == index) { used = true; break; }
        if (used) continue;
        o->id = (int)id; o->index = index; break;
    }
    o->next = s->objects; s->objects = o; return o->id;
}
static int64_t sem_ops(struct maton_sysvipc *s, struct request *r, bool pending) {
    struct object *o = find(s, KSEM, (int)r->a[0]);
    if (!o) return pending ? -EIDRM : -EINVAL;
    if (!r->ops) {
        r->nops = (size_t)r->a[2];
        if (!r->nops) return -EINVAL;
        if (r->nops > SEM_OPS) return -E2BIG;
        r->ops = calloc(r->nops, sizeof(struct sembuf));
        if (!r->ops) return -ENOMEM;
        if (!readmem(s, r, r->a[1], r->ops, r->nops * sizeof(struct sembuf))) return -EFAULT;
        if (r->op == O_SEMTIMEDOP && r->a[3]) {
            int64_t sec, ns;
            if (r->compat && !r->time64) {
                int32_t t[2];
                if (!readmem(s, r, r->a[3], t, sizeof(t))) return -EFAULT;
                sec = t[0]; ns = t[1];
            } else {
                int64_t t[2];
                if (!readmem(s, r, r->a[3], t, sizeof(t))) return -EFAULT;
                sec = t[0]; ns = t[1];
            }
            if (sec < 0 || ns < 0 || ns >= 1000000000) return -EINVAL;
            int64_t now = now_ns();
            r->deadline = sec > (INT64_MAX - now - ns)/1000000000 ? INT64_MAX : now + sec*1000000000 + ns;
        }
    }
    bool alter = false, undo = false;
    for (size_t i = 0; i < r->nops; i++) {
        if (r->ops[i].sem_num >= o->count) return -EFBIG;
        alter |= r->ops[i].sem_op != 0; undo |= !!(r->ops[i].sem_flg & SEM_UNDO);
    }
    if (!access_ok(&r->caller, o, alter ? 2 : 4)) return -EACCES;
    struct undo *u = NULL;
    if (undo) {
        struct process *p = process_get(s, r->caller.tgid);
        if (!p) return -ENOMEM;
        u = undo_get(p, o);
        if (!u) return -ENOMEM;
    }
    int *vals = malloc(o->count * sizeof(int)), *adj = NULL;
    if (!vals) return -ENOMEM;
    if (u) { adj = malloc(o->count * sizeof(int)); if (!adj) { free(vals); return -ENOMEM; } }
    for (size_t i = 0; i < o->count; i++) { vals[i] = o->values[i]; if (u) adj[i] = u->adj[i]; }
    int64_t result = 0;
    for (size_t i = 0; i < r->nops; i++) {
        struct sembuf b = r->ops[i];
        int v = vals[b.sem_num];
        if ((!b.sem_op && v != 0) || (b.sem_op < 0 && v + b.sem_op < 0)) {
            r->blocked = i;
            result = (b.sem_flg & IPC_NOWAIT) ? -EAGAIN : WAIT_RESULT; break;
        }
        if (v + b.sem_op > SEM_MAX) { result = -ERANGE; break; }
        vals[b.sem_num] = v + b.sem_op;
        if (b.sem_flg & SEM_UNDO) {
            int a = adj[b.sem_num] - b.sem_op;
            if (a < -SEM_MAX || a > SEM_MAX) { result = -ERANGE; break; }
            adj[b.sem_num] = a;
        }
    }
    if (result == WAIT_RESULT && r->deadline && now_ns() >= r->deadline) result = -EAGAIN;
    if (!result && valid(s, r)) {
        for (size_t i = 0; i < o->count; i++) { o->values[i] = (unsigned short)vals[i]; if (u) u->adj[i] = adj[i]; }
        for (size_t i = 0; i < r->nops; i++) o->pids[r->ops[i].sem_num] = r->caller.tgid;
        o->atime = walltime();
    }
    free(vals); free(adj); return result;
}
static int64_t shm_lock_ctl(struct maton_sysvipc *s, struct request *r, struct object *o, int cmd) {
    bool privileged = capability(&r->caller, 14); /* CAP_IPC_LOCK */
    if (!privileged && r->caller.uid != o->uid && r->caller.uid != o->cuid) return -EPERM;
    if (cmd == SHM_UNLOCK) {
        if (o->locked) { munmap(o->locked, o->locked_size); o->locked = NULL; }
        o->mode &= ~SHM_LOCKED; return 0;
    }
    if (o->locked) return 0;
    struct rlimit limit;
    if (prlimit(r->caller.tgid, RLIMIT_MEMLOCK, NULL, &limit)) return -EPERM;
    if (!privileged && !limit.rlim_cur) return -EPERM;
    size_t page = (size_t)getpagesize(), size = (o->size + page - 1) & ~(page - 1);
    uint64_t used = 0;
    for (struct object *other = s->objects; other; other = other->next)
        if (other->locked && other->lock_uid == r->caller.uid) {
            if (other->locked_size > UINT64_MAX - used) return -ENOMEM;
            used += other->locked_size;
        }
    if (!privileged && (used > limit.rlim_cur || size > limit.rlim_cur - used)) return -ENOMEM;
    void *mapping = mmap(NULL, size, PROT_READ, MAP_SHARED, o->fd, 0);
    if (mapping == MAP_FAILED) return -ENOMEM;
    if (mlock(mapping, size)) { munmap(mapping, size); return -ENOMEM; }
    o->locked = mapping; o->locked_size = size; o->lock_uid = r->caller.uid;
    o->mode |= SHM_LOCKED; return 0;
}
static int64_t control(struct maton_sysvipc *s, struct request *r, enum kind k) {
    int cmd = (int)r->a[k == KSEM ? 2 : 1];
    r->old = r->compat && r->multiplex && !(cmd & IPC_64);
    cmd &= ~IPC_64;
    if ((int)r->a[0] < 0) return -EINVAL;
    bool known = cmd == IPC_INFO || cmd == IPC_STAT || cmd == IPC_SET || cmd == IPC_RMID;
    if (k == KSEM) known |= cmd == SEM_INFO || cmd == SEM_STAT || cmd == SEM_STAT_ANY ||
        cmd == GETPID || cmd == GETVAL || cmd == GETALL || cmd == GETNCNT || cmd == GETZCNT ||
        cmd == SETVAL || cmd == SETALL;
    else if (k == KSHM) known |= cmd == SHM_INFO || cmd == SHM_STAT || cmd == SHM_STAT_ANY ||
        cmd == SHM_LOCK || cmd == SHM_UNLOCK;
    else known |= cmd == MSG_INFO || cmd == MSG_STAT || cmd == MSG_STAT_ANY;
    if (!known) return -EINVAL;
    uint64_t arg = r->a[k == KSEM ? 3 : 2];
    if (cmd == IPC_INFO || (k == KSEM && cmd == SEM_INFO) ||
            (k == KSHM && cmd == SHM_INFO) || (k == KMSG && cmd == MSG_INFO)) return info(s, r, k, cmd, arg);
    bool indexed = (k == KSEM && (cmd == SEM_STAT || cmd == SEM_STAT_ANY)) ||
        (k == KSHM && (cmd == SHM_STAT || cmd == SHM_STAT_ANY)) ||
        (k == KMSG && (cmd == MSG_STAT || cmd == MSG_STAT_ANY));
    bool any = (k == KSEM && cmd == SEM_STAT_ANY) || (k == KSHM && cmd == SHM_STAT_ANY) ||
        (k == KMSG && cmd == MSG_STAT_ANY);
    struct object *o = find(s, k, (int)r->a[0]);
    if (indexed) {
        o = NULL;
        for (struct object *candidate = s->objects; candidate; candidate = candidate->next)
            if (candidate->kind == k && candidate->index == (int)r->a[0]) { o = candidate; break; }
    }
    if (!o) return -EINVAL;
    if (cmd == IPC_RMID) {
        if (!owner(&r->caller, o)) return -EPERM;
        if (k == KSHM && o->count) { o->removed = true; o->key = IPC_PRIVATE; o->mode |= SHM_DEST; }
        else remove_object(s, o);
        return 0;
    }
    if (cmd == IPC_SET) {
        if (!owner(&r->caller, o)) return -EPERM;
        return set_ds(s, r, o, arg);
    }
    if (cmd == IPC_STAT || indexed) {
        if (!any && !access_ok(&r->caller, o, 4)) return -EACCES;
        int res = stat_ds(s, r, o, arg);
        return res < 0 ? res : indexed ? o->id : 0;
    }
    if (k == KSHM && (cmd == SHM_LOCK || cmd == SHM_UNLOCK)) return shm_lock_ctl(s, r, o, cmd);
    if (k != KSEM) return -EINVAL;
    size_t index = (uint32_t)r->a[1];
    if (cmd != GETALL && cmd != SETALL && index >= o->count) return -EINVAL;
    if (cmd == SETVAL || cmd == SETALL) {
        if (!access_ok(&r->caller, o, 2)) return -EACCES;
        if (cmd == SETVAL) {
            int value = (int)arg;
            if (value < 0 || value > SEM_MAX) return -ERANGE;
            o->values[index] = (unsigned short)value; o->pids[index] = r->caller.tgid;
            clear_undo(s, o->id, (int)index);
        } else {
            unsigned short *v = malloc(o->count * sizeof(*v));
            if (!v) return -ENOMEM;
            if (!readmem(s, r, arg, v, o->count * sizeof(*v))) { free(v); return -EFAULT; }
            for (size_t i = 0; i < o->count; i++) if (v[i] > SEM_MAX) { free(v); return -ERANGE; }
            memcpy(o->values, v, o->count*sizeof(*v)); free(v);
            for (size_t i = 0; i < o->count; i++) o->pids[i] = r->caller.tgid;
            clear_undo(s, o->id, -1);
        }
        o->ctime = walltime(); return 0;
    }
    if (!access_ok(&r->caller, o, 4)) return -EACCES;
    if (cmd == GETVAL) return o->values[index];
    if (cmd == GETPID) return o->pids[index];
    if (cmd == GETALL) return writemem(s, r, arg, o->values, o->count*sizeof(*o->values)) ? 0 : -EFAULT;
    if (cmd == GETNCNT || cmd == GETZCNT) {
        int n = 0;
        for (struct request *w = s->pending; w; w = w->next) if (
                (w->op == O_SEMOP || w->op == O_SEMTIMEDOP) && w->a[0] == r->a[0] && valid(s, w)) {
            struct sembuf b = w->ops[w->blocked];
            if (b.sem_num == index && ((cmd == GETZCNT && !b.sem_op) || (cmd == GETNCNT && b.sem_op < 0))) n++;
        }
        return n;
    }
    return -EINVAL;
}
static bool mapped_segment(pid_t pid, uint64_t addr, struct object *o) {
    char path[64], *line = NULL;
    size_t cap = 0;
    struct stat st;
    if (fstat(o->fd, &st)) return false;
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "re");
    if (!f) return false;
    bool found = false;
    while (getline(&line, &cap, f) >= 0) {
        unsigned long long start, end, offset, inode;
        unsigned maj, min;
        char perms[5];
        if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu", &start, &end, perms,
                &offset, &maj, &min, &inode) != 7) continue;
        if (addr == start && perms[3] == 's' && !offset && inode == st.st_ino &&
                maj == major(st.st_dev) && min == minor(st.st_dev)) { found = true; break; }
    }
    free(line); fclose(f); return found;
}
/* Static programs cannot load the shmat helper, and only it can map the
 * injected memfd. Without it, behave like a kernel without SysV shm: ENOSYS. */
static bool helper_loaded(pid_t tgid) {
    char path[64], *line = NULL;
    size_t cap = 0;
    snprintf(path, sizeof(path), "/proc/%d/maps", tgid);
    FILE *f = fopen(path, "re");
    if (!f) return false;
    bool found = false;
    while (!found && getline(&line, &cap, f) >= 0) found = strstr(line, "libmaton-shm") != NULL;
    free(line); fclose(f);
    return found;
}
static int64_t shm_attach(struct maton_sysvipc *s, struct request *r) {
    struct process *p = process_get(s, r->caller.tgid);
    if (!p) return -ENOMEM;
    if (r->a[3] == MATON_SHM_RESET) {
        while (p->attachments) detach(s, p, &p->attachments);
        return 0;
    }
    if (r->a[3] == MATON_SHM_INHERIT) {
        struct object *o = find(s, KSHM, (int)r->a[0]);
        if (!o || !r->a[1] || r->a[1] % (unsigned)getpagesize()) return -EINVAL;
        if (!mapped_segment(r->caller.tgid, r->a[1], o)) return -EINVAL;
        struct attachment *a = calloc(1, sizeof(*a));
        if (!a) return -ENOMEM;
        a->id = o->id; a->addr = r->a[1]; a->committed = true;
        a->next = p->attachments; p->attachments = a; o->count++;
        o->atime = walltime(); o->lpid = p->pid; return 0;
    }
    if (r->a[3] == MATON_SHM_COMMIT || r->a[3] == MATON_SHM_CANCEL) {
        struct attachment **ap = &p->attachments;
        while (*ap && ((*ap)->id != (int)r->a[0] || (*ap)->token != (int)r->a[4] || (*ap)->committed)) ap = &(*ap)->next;
        if (!*ap) return -EINVAL;
        if (r->a[3] == MATON_SHM_CANCEL) { detach(s, p, ap); return 0; }
        if (!r->a[1] || r->a[1] % (unsigned)getpagesize()) return -EINVAL;
        struct object *o = find(s, KSHM, (*ap)->id);
        if (!o || !mapped_segment(r->caller.tgid, r->a[1], o)) return -EINVAL;
        struct attachment *current = *ap;
        if (r->a[2] & SHM_REMAP) {
            size_t page = (size_t)getpagesize(), span = (o->size + page - 1) & ~(page - 1);
            struct attachment **old = &p->attachments;
            while (*old) {
                if (*old != current && (*old)->committed && (*old)->addr >= r->a[1] &&
                        (*old)->addr - r->a[1] < span) detach(s, p, old);
                else old = &(*old)->next;
            }
        }
        current->addr = r->a[1]; current->committed = true;
        o->atime = walltime(); o->lpid = p->pid; return 0;
    }
    if (!helper_loaded(r->caller.tgid)) return -ENOSYS;
    struct object *o = find(s, KSHM, (int)r->a[0]);
    if (!o) return -EINVAL;
    unsigned flags = (unsigned)r->a[2];
    unsigned needed = flags & SHM_RDONLY ? 4 : 6;
    if (flags & SHM_EXEC) needed |= 1;
    if (!access_ok(&r->caller, o, needed)) return -EACCES;
    if (flags & ~(SHM_RDONLY | SHM_RND | SHM_REMAP | SHM_EXEC)) return -EINVAL;
    struct attachment *a = calloc(1, sizeof(*a));
    if (!a) return -ENOMEM;
    int src = o->fd, ro = -1;
    if (flags & SHM_RDONLY) {
        char path[64]; snprintf(path, sizeof(path), "/proc/self/fd/%d", o->fd);
        src = ro = open(path, O_RDONLY | O_CLOEXEC);
        if (src < 0) { free(a); return -errno; }
    }
    struct seccomp_notif_addfd add = { .id = r->notif.id, .srcfd = (unsigned)src,
        .newfd_flags = O_CLOEXEC, .flags = SECCOMP_ADDFD_FLAG_SEND };
    int injected = ioctl(r->listener, SECCOMP_IOCTL_NOTIF_ADDFD, &add);
    int e = errno;
    if (ro >= 0) close(ro);
    if (injected < 0) { free(a); return -e; }
    a->id = o->id; a->token = injected; a->next = p->attachments; p->attachments = a;
    o->count++;
    /* ADDFD_FLAG_SEND completed the original syscall atomically. */
    return WAIT_RESULT + 1;
}
static int64_t shm_detach(struct maton_sysvipc *s, struct request *r) {
    for (struct process *p = s->processes; p; p = p->next) if (p->pid == r->caller.tgid) {
        struct attachment **ap = &p->attachments;
        while (*ap && (!(*ap)->committed || (*ap)->addr != r->a[0])) ap = &(*ap)->next;
        if (!*ap) return -EINVAL;
        detach(s, p, ap); return 0;
    }
    return -EINVAL;
}
static int64_t msg_send(struct maton_sysvipc *s, struct request *r, bool pending) {
    struct object *o = find(s, KMSG, (int)r->a[0]);
    if (!o) return pending ? -EIDRM : -EINVAL;
    if (!access_ok(&r->caller, o, 2)) return -EACCES;
    size_t size = r->a[2], w = r->compat ? 4 : 8;
    if (size > MSG_MAX) return -EINVAL;
    if (!r->message) {
        unsigned char type[8] = {0};
        if (!readmem(s, r, r->a[1], type, w)) return -EFAULT;
        int64_t t = r->compat ? (int64_t)(int32_t)getnum(type, 0, 4) : (int64_t)getnum(type, 0, 8);
        if (t <= 0) return -EINVAL;
        r->message = malloc(sizeof(struct message) + size);
        if (!r->message) return -ENOMEM;
        r->message->next = NULL; r->message->type = t; r->message->size = size;
        if (!readmem(s, r, r->a[1] + w, r->message->data, size)) return -EFAULT;
    }
    if (size > o->qbytes || o->bytes > o->qbytes - size || o->count >= o->qbytes)
        return r->a[3] & IPC_NOWAIT ? -EAGAIN : WAIT_RESULT;
    struct message **tail = &o->messages;
    while (*tail) tail = &(*tail)->next;
    *tail = r->message; r->message = NULL; o->bytes += size; o->count++;
    o->cpid = r->caller.tgid; o->atime = walltime(); return 0;
}
static int64_t msg_recv(struct maton_sysvipc *s, struct request *r, bool pending) {
    struct object *o = find(s, KMSG, (int)r->a[0]);
    if (!o) return pending ? -EIDRM : -EINVAL;
    if (!access_ok(&r->caller, o, 4)) return -EACCES;
    int64_t type = r->compat ? (int64_t)(int32_t)r->a[3] : (int64_t)r->a[3];
    unsigned flags = (unsigned)r->a[4];
    if (flags & MSG_COPY) return -EINVAL;
    if ((r->compat && r->a[2] > INT32_MAX) || (!r->compat && r->a[2] > INT64_MAX)) return -EINVAL;
    uint64_t bound = type == INT64_MIN ? INT64_MAX : type < 0 ? (uint64_t)-type : 0;
    struct message **best = NULL;
    for (struct message **mp = &o->messages; *mp; mp = &(*mp)->next) {
        struct message *m = *mp;
        if (!type || (type > 0 && ((flags & MSG_EXCEPT) ? m->type != type : m->type == type))) { best = mp; break; }
        if (type < 0 && (uint64_t)m->type <= bound && (!best || m->type < (*best)->type)) best = mp;
    }
    if (!best) return flags & IPC_NOWAIT ? -ENOMSG : WAIT_RESULT;
    struct message *m = *best;
    if (m->size > r->a[2] && !(flags & MSG_NOERROR)) return -E2BIG;
    size_t size = m->size < r->a[2] ? m->size : (size_t)r->a[2], w = r->compat ? 4 : 8;
    unsigned char *buf = malloc(w + size);
    if (!buf) return -ENOMEM;
    putnum(buf, 0, w, m->type); memcpy(buf + w, m->data, size);
    bool ok = writemem(s, r, r->a[1], buf, w + size); free(buf);
    *best = m->next; o->bytes -= m->size; o->count--; free(m);
    o->lpid = r->caller.tgid; o->dtime = walltime(); return ok ? (int64_t)size : -EFAULT;
}

static int decode(struct maton_sysvipc *s, struct request *r) {
    r->compat = r->notif.data.arch == AUDIT_ARCH_I386;
    if (!r->compat && r->notif.data.arch != AUDIT_ARCH_X86_64) return -EINVAL;
    for (size_t i = 0; i < 6; i++) r->a[i] = r->compat ? (uint32_t)r->notif.data.args[i] : r->notif.data.args[i];
    int nr = r->notif.data.nr;
    if (!r->compat) {
        switch (nr) {
        case 64: r->op = O_SEMGET; break; case 65: r->op = O_SEMOP; break;
        case 66: r->op = O_SEMCTL; break; case 220: r->op = O_SEMTIMEDOP; break;
        case 29: r->op = O_SHMGET; break; case 30: r->op = O_SHMAT; break;
        case 31: r->op = O_SHMCTL; break; case 67: r->op = O_SHMDT; break;
        case 68: r->op = O_MSGGET; break; case 69: r->op = O_MSGSND; break;
        case 70: r->op = O_MSGRCV; break; case 71: r->op = O_MSGCTL; break;
        default: return -EINVAL;
        }
    } else {
        switch (nr) {
        case 393: r->op = O_SEMGET; break; case 394: r->op = O_SEMCTL; break;
        case 420: r->op = O_SEMTIMEDOP; r->time64 = true; break;
        case 395: r->op = O_SHMGET; break; case 396: r->op = O_SHMCTL; break;
        case 397: r->op = O_SHMAT; break; case 398: r->op = O_SHMDT; break;
        case 399: r->op = O_MSGGET; break; case 400: r->op = O_MSGSND; break;
        case 401: r->op = O_MSGRCV; break; case 402: r->op = O_MSGCTL; break;
        case 117: {
            uint64_t a[6]; memcpy(a, r->a, sizeof(a));
            unsigned call = (unsigned)a[0] & 0xffff, version = (unsigned)a[0] >> 16;
            r->multiplex = true;
            switch (call) {
            case SEMGET: r->op = O_SEMGET; r->a[0] = a[1]; r->a[1] = a[2]; r->a[2] = a[3]; break;
            case SEMOP: case SEMTIMEDOP:
                r->op = call == SEMOP ? O_SEMOP : O_SEMTIMEDOP;
                r->a[0] = a[1]; r->a[1] = a[4]; r->a[2] = a[2]; r->a[3] = a[5]; break;
            case SEMCTL: {
                uint32_t arg;
                if (!readmem(s, r, a[4], &arg, sizeof(arg))) return -EFAULT;
                r->op = O_SEMCTL; r->a[0] = a[1]; r->a[1] = a[2]; r->a[2] = a[3]; r->a[3] = arg; break;
            }
            case SHMGET: r->op = O_SHMGET; r->a[0] = a[1]; r->a[1] = a[2]; r->a[2] = a[3]; break;
            case SHMCTL: r->op = O_SHMCTL; r->a[0] = a[1]; r->a[1] = a[2]; r->a[2] = a[4]; break;
            case SHMAT:
                /* This deliberately returns fd, not the ipc raddr result.
                 * The interposer uses direct shmat 397 instead. */
                if (version) return -EINVAL;
                r->op = O_SHMAT; r->a[0] = a[1]; r->a[1] = a[4]; r->a[2] = a[2]; r->a[3] = 0; break;
            case SHMDT: r->op = O_SHMDT; r->a[0] = a[4]; break;
            case MSGGET: r->op = O_MSGGET; r->a[0] = a[1]; r->a[1] = a[2]; break;
            case MSGCTL: r->op = O_MSGCTL; r->a[0] = a[1]; r->a[1] = a[2]; r->a[2] = a[4]; break;
            case MSGSND: r->op = O_MSGSND; r->a[0] = a[1]; r->a[1] = a[4]; r->a[2] = a[2]; r->a[3] = a[3]; break;
            case MSGRCV:
                r->op = O_MSGRCV; r->a[0] = a[1]; r->a[2] = a[2]; r->a[4] = a[3];
                if (!version) {
                    uint32_t kludge[2];
                    if (!readmem(s, r, a[4], kludge, sizeof(kludge))) return -EFAULT;
                    r->a[1] = kludge[0]; r->a[3] = kludge[1];
                } else { r->a[1] = a[4]; r->a[3] = a[5]; }
                break;
            default: return -EINVAL;
            }
            break;
        }
        default: return -EINVAL;
        }
    }
    return 0;
}
static int64_t dispatch(struct maton_sysvipc *s, struct request *r, bool pending) {
    if (!valid(s, r)) return -ESRCH;
    switch (r->op) {
    case O_SEMGET: return get_object(s, r, KSEM);
    case O_SHMGET: return get_object(s, r, KSHM);
    case O_MSGGET: return get_object(s, r, KMSG);
    case O_SEMOP: case O_SEMTIMEDOP: return sem_ops(s, r, pending);
    case O_SEMCTL: return control(s, r, KSEM);
    case O_SHMCTL: return control(s, r, KSHM);
    case O_MSGCTL: return control(s, r, KMSG);
    case O_SHMAT: return shm_attach(s, r);
    case O_SHMDT: return shm_detach(s, r);
    case O_MSGSND: return msg_send(s, r, pending);
    case O_MSGRCV: return msg_recv(s, r, pending);
    default: return -EINVAL;
    }
}
static void reevaluate(struct maton_sysvipc *s) {
    bool progress;
    do {
        progress = false;
        struct request **pp = &s->pending;
        while (*pp) {
            struct request *r = *pp;
            bool alive = valid(s, r);
            int64_t result = alive ? dispatch(s, r, true) : -ESRCH;
            if (result == WAIT_RESULT) { pp = &r->next; continue; }
            *pp = r->next;
            if (alive) reply(s, r, result);
            request_free(r); progress = true;
        }
    } while (progress && s->pending);
}
struct maton_sysvipc *maton_sysvipc_new(void) {
    struct maton_sysvipc *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s->wake < 0) { free(s); return NULL; }
    if (pthread_mutex_init(&s->lock, NULL)) { close(s->wake); free(s); return NULL; }
    return s;
}
static void wake(struct maton_sysvipc *s) {
    uint64_t one = 1;
    while (write(s->wake, &one, sizeof(one)) < 0 && errno == EINTR) {}
}
void maton_sysvipc_stop(struct maton_sysvipc *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock); s->stopping = true; pthread_mutex_unlock(&s->lock);
    wake(s);
}
int maton_sysvipc_add_listener(struct maton_sysvipc *s, int listener_fd) {
    if (!s || listener_fd < 0) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&s->lock);
    int *grown = s->nincoming < SIZE_MAX / sizeof(int) - 1 ?
        realloc(s->incoming, (s->nincoming + 1) * sizeof(int)) : NULL;
    if (grown) { s->incoming = grown; s->incoming[s->nincoming++] = listener_fd; }
    pthread_mutex_unlock(&s->lock);
    if (!grown) { errno = ENOMEM; return -1; }
    wake(s);
    return 0;
}
static bool add_serving(struct maton_sysvipc *s, int fd, bool owned) {
    if (s->nlisteners >= SIZE_MAX / sizeof(struct listener) - 1) return false;
    struct listener *grown = realloc(s->listeners, (s->nlisteners + 1) * sizeof(*grown));
    if (!grown) return false;
    s->listeners = grown; s->listeners[s->nlisteners++] = (struct listener){ fd, owned };
    return true;
}
/* Take listeners handed over by other threads; returns true when stopping. */
static bool take_incoming(struct maton_sysvipc *s) {
    uint64_t count;
    while (read(s->wake, &count, sizeof(count)) < 0 && errno == EINTR) {}
    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < s->nincoming; i++)
        if (!add_serving(s, s->incoming[i], true)) close(s->incoming[i]);
    s->nincoming = 0;
    bool stopping = s->stopping;
    pthread_mutex_unlock(&s->lock);
    return stopping;
}
/* A listener hung up: its tracees are gone, so its pending requests are dead. */
static void drop_listener(struct maton_sysvipc *s, size_t index) {
    struct listener l = s->listeners[index];
    for (struct request **r = &s->pending; *r;) {
        if ((*r)->listener == l.fd) { struct request *dead = *r; *r = dead->next; request_free(dead); }
        else r = &(*r)->next;
    }
    if (l.owned) close(l.fd);
    s->listeners[index] = s->listeners[--s->nlisteners];
}
void maton_sysvipc_free(struct maton_sysvipc *s) {
    if (!s) return;
    while (s->objects) { struct object *o = s->objects; s->objects = o->next; object_free(o); }
    while (s->pending) { struct request *r = s->pending; s->pending = r->next; request_free(r); }
    while (s->processes) {
        struct process *p = s->processes; s->processes = p->next;
        while (p->undo) { struct undo *u = p->undo; p->undo = u->next; free(u->adj); free(u); }
        while (p->attachments) { struct attachment *a = p->attachments; p->attachments = a->next; free(a); }
        close(p->pidfd); free(p);
    }
    for (size_t i = 0; i < s->nlisteners; i++) if (s->listeners[i].owned) close(s->listeners[i].fd);
    for (size_t i = 0; i < s->nincoming; i++) close(s->incoming[i]);
    free(s->listeners); free(s->incoming);
    pthread_mutex_destroy(&s->lock); close(s->wake); free(s);
}
int maton_sysvipc_serve(struct maton_sysvipc *s, int listener_fd) {
    if (!s) { errno = EINVAL; return -1; }
    struct seccomp_notif_sizes sizes;
    if (syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sizes) < 0) return -1;
    struct seccomp_notif *notif = calloc(1, sizes.seccomp_notif);
    if (!notif) return -1;
    if (listener_fd >= 0 && !add_serving(s, listener_fd, false)) { free(notif); errno = ENOMEM; return -1; }
    int result = 0;
    for (;;) {
        (void)reap_processes(s); reevaluate(s);
        size_t n = 1 + s->nlisteners;
        for (struct process *p = s->processes; p; p = p->next) n++;
        if (n > SIZE_MAX / sizeof(struct pollfd)) { errno = ENOMEM; result = -1; break; }
        struct pollfd *fds = calloc(n, sizeof(*fds));
        if (!fds) { result = -1; break; }
        fds[0] = (struct pollfd){ .fd = s->wake, .events = POLLIN };
        size_t i = 1;
        for (size_t l = 0; l < s->nlisteners; l++) fds[i++] = (struct pollfd){ .fd = s->listeners[l].fd, .events = POLLIN };
        for (struct process *p = s->processes; p; p = p->next) fds[i++] = (struct pollfd){ .fd = p->pidfd, .events = POLLIN };
        /* Periodically discard interrupted notification IDs. */
        int timeout = s->pending ? 50 : -1;
        for (struct request *r = s->pending; r; r = r->next) if (r->deadline) {
            int64_t remaining = r->deadline - now_ns();
            int ms = remaining <= 0 ? 0 : remaining/1000000 >= INT_MAX ? INT_MAX : (int)(remaining/1000000 + 1);
            if (timeout < 0 || ms < timeout) timeout = ms;
        }
        int rc = poll(fds, (nfds_t)n, timeout);
        if (rc < 0) { free(fds); if (errno == EINTR) continue; result = -1; break; }
        if ((fds[0].revents & POLLIN) && take_incoming(s)) { free(fds); break; }
        /* Walk backwards: drop_listener moves the last entry into the hole. */
        size_t count = s->nlisteners;
        bool failed = false;
        for (size_t l = count; l-- > 0;) {
            short revents = fds[1 + l].revents;
            if (revents & (POLLHUP | POLLNVAL | POLLERR)) { drop_listener(s, l); continue; }
            if (!(revents & POLLIN)) continue;
            int fd = s->listeners[l].fd;
            memset(notif, 0, sizes.seccomp_notif);
            if (ioctl(fd, SECCOMP_IOCTL_NOTIF_RECV, notif) < 0) {
                if (errno == EINTR || errno == ENOENT) continue;
                if (errno == EBADF) { drop_listener(s, l); continue; }
                failed = true; break;
            }
            struct request *r = calloc(1, sizeof(*r));
            if (!r) {
                struct seccomp_notif_resp resp = { .id = notif->id, .error = -ENOMEM };
                (void)ioctl(fd, SECCOMP_IOCTL_NOTIF_SEND, &resp); continue;
            }
            r->notif = *notif; r->listener = fd;
            int64_t value;
            if (!credentials(&r->caller, (pid_t)notif->pid)) value = -ESRCH;
            else { int e = decode(s, r); value = e ? e : dispatch(s, r, false); }
            if (value == WAIT_RESULT) {
                struct request **tail = &s->pending;
                while (*tail) tail = &(*tail)->next;
                *tail = r;
            } else {
                if (value != WAIT_RESULT + 1) reply(s, r, value);
                request_free(r);
            }
        }
        free(fds);
        if (failed) { result = -1; break; }
    }
    while (s->pending) {
        struct request *r = s->pending; s->pending = r->next;
        if (valid(s, r)) reply(s, r, -EINTR);
        request_free(r);
    }
    /* The borrowed initial listener stays open for the caller. */
    for (size_t l = s->nlisteners; l-- > 0;) if (!s->listeners[l].owned) s->listeners[l] = s->listeners[--s->nlisteners];
    free(notif); return result;
}
