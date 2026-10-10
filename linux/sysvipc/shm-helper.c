/* SPDX-License-Identifier: Apache-2.0 */
/* In-process half of SysV shm: the supervisor injects a segment's memfd as
 * shmat's result and only this code can map it. LD_PRELOADed into Flatpak
 * sandboxes by matonos-bwrap for both glibc ABIs (x86_64, i386).
 *
 * Freestanding on purpose: no libc headers, so the NDK clang builds both ABIs
 * (--target=x86_64-linux-gnu / i686-linux-gnu -ffreestanding -nostdlib). It
 * binds only stable glibc symbols at load time and avoids ABI-dependent libc
 * types (pthread_mutex_t, struct stat). */
#include "sysvipc.h"
#include <stddef.h>
#include <stdint.h>

long syscall(long number, ...);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);
long lseek(int fd, long offset, int whence);
int close(int fd);
void *malloc(size_t size);
void free(void *ptr);
int sched_yield(void);
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));
int *__errno_location(void);
#define errno (*__errno_location())

#define EINVAL 22
#define ENOMEM 12
#define PAGE 4096UL               /* x86 page size; SHMLBA equals it */
#define SHM_RDONLY 010000
#define SHM_RND 020000
#define SHM_REMAP 040000
#define SHM_EXEC 0100000
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_FIXED 0x10
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void *)-1)
#define SEEK_END 2
#if defined(__i386__)
/* glibc may use ipc(117) internally. Interposing its public shmat/shmdt
 * bypasses that wrapper; direct 397/398 have the same helper contract. */
#define NR_SHMAT 397
#define NR_SHMDT 398
#elif defined(__x86_64__)
#define NR_SHMAT 30
#define NR_SHMDT 67
#else
#error "shm-helper.c: add this architecture's shmat/shmdt numbers"
#endif

struct mapping { struct mapping *next; void *addr; size_t size; int id; };
static struct mapping *mappings;
static char locked;
static void lock(void) { while (__atomic_test_and_set(&locked, __ATOMIC_ACQUIRE)) sched_yield(); }
static void unlock(void) { __atomic_clear(&locked, __ATOMIC_RELEASE); }

static void fork_prepare(void) { lock(); }
static void fork_parent(void) { unlock(); }
static void fork_child(void) {
    for (struct mapping *m = mappings; m; m = m->next)
        (void)syscall(NR_SHMAT, m->id, m->addr, 0, MATON_SHM_INHERIT, 0, 0);
    unlock();
}
__attribute__((constructor)) static void init(void) {
    (void)syscall(NR_SHMAT, -1, 0, 0, MATON_SHM_RESET, 0, 0);
    (void)pthread_atfork(fork_prepare, fork_parent, fork_child);
}
static void *fail(struct mapping *m, int e) { unlock(); free(m); errno = e; return MAP_FAILED; }

__attribute__((visibility("default"))) void *shmat(int id, const void *address, int flags) {
    uintptr_t addr = (uintptr_t)address;
    if (flags & SHM_RND) addr -= addr % PAGE;
    if ((addr && addr % PAGE) || ((flags & SHM_REMAP) && !addr)) { errno = EINVAL; return MAP_FAILED; }
    struct mapping *m = malloc(sizeof(*m));
    if (!m) { errno = ENOMEM; return MAP_FAILED; }
    lock();
    if (flags & SHM_REMAP) for (struct mapping *old = mappings; old; old = old->next) {
        uintptr_t start = (uintptr_t)old->addr;
        if (start < addr && addr - start < old->size) return fail(m, EINVAL);
    }
    long fd = syscall(NR_SHMAT, id, (void *)addr, flags, 0, 0, 0);
    if (fd < 0) return fail(m, errno);
    int prot = PROT_READ | ((flags & SHM_RDONLY) ? 0 : PROT_WRITE) | ((flags & SHM_EXEC) ? PROT_EXEC : 0);
    int mapflags = MAP_SHARED;
    if (addr) mapflags |= (flags & SHM_REMAP) ? MAP_FIXED : MAP_FIXED_NOREPLACE;
    /* The memfd is sized to the page-rounded segment. */
    long size = lseek((int)fd, 0, SEEK_END);
    int e = EINVAL;
    void *mapped = MAP_FAILED;
    int size_ok = size > 0;
    if (size_ok && (flags & SHM_REMAP)) {
        /* Partial overlaps cannot be represented by this tiny helper. */
        for (struct mapping *old = mappings; old; old = old->next) {
            uintptr_t start = (uintptr_t)old->addr;
            if (start >= addr && start - addr < (size_t)size && old->size > (size_t)size - (start - addr)) {
                size_ok = 0; break;
            }
        }
    }
    if (size_ok) {
        mapped = mmap((void *)addr, (size_t)size, prot, mapflags, (int)fd, 0);
        if (mapped == MAP_FAILED) e = errno;
    }
    if (mapped == MAP_FAILED) {
        (void)syscall(NR_SHMAT, id, 0, 0, MATON_SHM_CANCEL, fd, 0);
        close((int)fd); return fail(m, e);
    }
    if (syscall(NR_SHMAT, id, mapped, flags, MATON_SHM_COMMIT, fd, 0) < 0) {
        e = errno;
        munmap(mapped, (size_t)size);
        (void)syscall(NR_SHMAT, id, 0, 0, MATON_SHM_CANCEL, fd, 0);
        close((int)fd); return fail(m, e);
    }
    close((int)fd);
    m->addr = mapped; m->size = (size_t)size; m->id = id;
    /* SHM_REMAP replaces any previous helper mapping at this address. */
    if (flags & SHM_REMAP) {
        struct mapping **mp = &mappings;
        while (*mp) {
            struct mapping *old = *mp;
            uintptr_t start = (uintptr_t)old->addr;
            if (start >= (uintptr_t)mapped && start - (uintptr_t)mapped < m->size) { *mp = old->next; free(old); }
            else mp = &old->next;
        }
    }
    m->next = mappings; mappings = m;
    unlock(); return mapped;
}
__attribute__((visibility("default"))) int shmdt(const void *addr) {
    lock();
    struct mapping **mp = &mappings;
    while (*mp && (*mp)->addr != addr) mp = &(*mp)->next;
    if (!*mp) { unlock(); errno = EINVAL; return -1; }
    struct mapping *m = *mp;
    if (munmap(m->addr, m->size)) { unlock(); return -1; }
    *mp = m->next;
    int result = (int)syscall(NR_SHMDT, addr), e = errno;
    free(m); unlock(); errno = e; return result;
}
