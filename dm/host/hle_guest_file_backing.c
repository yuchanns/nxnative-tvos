#include "guest_file_backing.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <unistd.h>

int dm_guest_file_backing_access(void *reservation, size_t bytes, int random_access) {
    size_t page = (size_t)getpagesize();
    uintptr_t base = (uintptr_t)reservation;
    if (!base || !bytes || base % page || bytes % page || bytes > UINTPTR_MAX - base ||
        (random_access != 0 && random_access != 1)) { errno = EINVAL; return -1; }
    return madvise(reservation, bytes, random_access ? MADV_RANDOM : MADV_NORMAL);
}

size_t dm_guest_file_backing_cold(const void *reservation, size_t capacity,
                                  const void *address, size_t bytes) {
    uintptr_t base = (uintptr_t)reservation, start = (uintptr_t)address;
    if (!base || !start || !bytes || capacity > UINTPTR_MAX - base ||
        start < base || bytes > capacity || start - base > capacity - bytes) return 0;
    size_t page = (size_t)getpagesize(), skip = (page - start % page) % page;
    if (bytes <= skip) return 0;
    size_t length = (bytes - skip) / page * page;
    // FREE would discard guest data. DONTNEED only lowers the priority of these
    // file-backed pages; later CPU reads and writes still see the same bytes.
    if (!length || madvise((void *)(start + skip), length, MADV_DONTNEED)) return 0;
    return length;
}

int dm_guest_file_backing_map(void *reservation, size_t bytes, const char *directory) {
    size_t page = (size_t)getpagesize();
    if (!reservation || !bytes || (uintptr_t)reservation % page || bytes % page ||
        bytes > INT64_MAX || !directory || directory[0] != '/') { errno = EINVAL; return -1; }
    char path[PATH_MAX];
    int length = snprintf(path, sizeof path, "%s/dm-guest-heap-XXXXXX", directory);
    if (length < 0 || (size_t)length >= sizeof path) { errno = ENAMETOOLONG; return -1; }
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    int error = 0;
    // No stale heap survives a crash. The mapping keeps its unlinked file alive.
    if (unlink(path)) { error = errno; goto failed; }
    struct statfs space;
    if (fstatfs(fd, &space)) { error = errno; goto failed; }
    const uint64_t reserve = 64ull << 20;
    if (!space.f_bsize || (uint64_t)space.f_bavail > UINT64_MAX / space.f_bsize ||
        (uint64_t)space.f_bavail * space.f_bsize < bytes + reserve) { error = ENOSPC; goto failed; }
    // Reserve real disk space before exposing writable pages, so a sparse file
    // cannot later turn an ordinary guest write into SIGBUS due to disk exhaustion.
    fstore_t storage = {.fst_flags = F_ALLOCATEALL, .fst_posmode = F_PEOFPOSMODE, .fst_length = (off_t)bytes};
    if (fcntl(fd, F_PREALLOCATE, &storage)) { error = errno; goto failed; }
    if (storage.fst_bytesalloc < (off_t)bytes) { error = ENOSPC; goto failed; }
    if (ftruncate(fd, (off_t)bytes)) { error = errno; goto failed; }
    void *mapped = mmap(reservation, bytes, PROT_NONE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (mapped != reservation) {
        error = errno;
        // MAP_FIXED may remove the old reservation before the kernel rejects
        // the replacement. It contains no data yet, so restore its guard pages.
        if (mmap(reservation, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != reservation) abort();
        goto failed;
    }
    return fd;
failed:
    close(fd);
    unlink(path);
    errno = error;
    return -1;
}
