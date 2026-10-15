#define _GNU_SOURCE
/* Syscall contract tests, not a substitute for a verity-enabled filesystem. */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#include <linux/fsverity.h>
#include <errno.h>
#include <sys/ioctl.h>
static int enable_error, measure_error, calls, bad_digest;
static int test_ioctl(int fd, unsigned long command, ...) {
    assert(fd >= 0);
    va_list ap; va_start(ap, command);
    void* arg = va_arg(ap, void*); va_end(ap);
    calls++;
    if (command == FS_IOC_ENABLE_VERITY) {
        struct fsverity_enable_arg* a = arg;
        assert(a->version == 1 && a->hash_algorithm == FS_VERITY_HASH_ALG_SHA256);
        assert(a->block_size == 4096 && !a->salt_size && !a->sig_size);
        if (enable_error) { errno = enable_error; return -1; }
    } else {
        assert(command == FS_IOC_MEASURE_VERITY);
        struct fsverity_digest* d = arg;
        assert(d->digest_size == 32);
        if (measure_error) { errno = measure_error; return -1; }
        d->digest_algorithm = bad_digest ? 2 : FS_VERITY_HASH_ALG_SHA256;
        d->digest_size = 32;
    }
    return 0;
}
#define MATON_VERITY_IOCTL test_ioctl
#include "../StagedVerity.h"

int main(void) {
    char tmp[] = "/tmp/maton-verity-contract-XXXXXX", error[128];
    assert(mkdtemp(tmp));
    int dir = open(tmp, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(dir >= 0);
    int file = openat(dir, "file", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file >= 0 && write(file, "hello", 5) == 5 && !close(file));
    struct stat st; assert(!fstat(dir, &st));
    const int failures[] = {EOPNOTSUPP, ENOTTY, ETXTBSY, EACCES, EPERM, EIO, ENOSPC};
    for (size_t i = 0; i < sizeof(failures)/sizeof(*failures); i++) {
        calls = 0; enable_error = failures[i];
        assert(maton_verity_openat(dir,"file",st.st_dev,getuid(),error,sizeof(error)) < 0);
        assert(errno == failures[i] && calls == 1);
        assert(!strcmp(error, maton_verity_error(failures[i])));
    }
    enable_error = 0; measure_error = EIO;
    assert(maton_verity_openat(dir,"file",st.st_dev,getuid(),error,sizeof(error)) < 0 && errno == EIO);
    measure_error = 0; bad_digest = 1;
    assert(maton_verity_openat(dir,"file",st.st_dev,getuid(),error,sizeof(error)) < 0 && errno == EINVAL);
    bad_digest = 0;
    for (int existing = 0; existing < 2; existing++) {
        enable_error = existing ? EEXIST : 0;
        int sealed = maton_verity_openat(dir,"file",st.st_dev,getuid(),error,sizeof(error));
        char content[5];
        assert(sealed >= 0 && (fcntl(sealed,F_GETFL) & O_ACCMODE) == O_RDONLY);
        assert(read(sealed,content,5) == 5 && !memcmp(content,"hello",5));
        assert(!close(sealed));
    }
    calls = 0;
    assert(maton_verity_openat(dir,"../file",st.st_dev,getuid(),error,sizeof(error)) < 0 && errno == EINVAL && !calls);
    assert(maton_verity_openat(dir,".",st.st_dev,getuid(),error,sizeof(error)) < 0 && errno == EINVAL && !calls);
    assert(!symlinkat("file",dir,"link"));
    calls = 0;
    assert(maton_verity_openat(dir,"link",st.st_dev,getuid(),error,sizeof(error)) < 0 && !calls);
    assert(!mkfifoat(dir,"fifo",0600));
    assert(maton_verity_openat(dir,"fifo",st.st_dev,getuid(),error,sizeof(error)) < 0 && !calls);
    assert(maton_verity_openat(dir,"file",st.st_dev+1,getuid(),error,sizeof(error)) < 0 && !calls);
    assert(maton_verity_openat(dir,"file",st.st_dev,getuid()+1,error,sizeof(error)) < 0 && !calls);
    assert(!unlinkat(dir,"fifo",0) && !unlinkat(dir,"link",0) && !unlinkat(dir,"file",0));
    close(dir); assert(!rmdir(tmp));
    puts("PASS: seal parameters, measured fd, fail-closed errors, type/device/owner guards");
    return 0;
}
