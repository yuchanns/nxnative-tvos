#include "xc3_heap_pages.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define HORIZON_PAGE_BYTES ((size_t)4096)
#define HEAP_UNIT_BYTES ((size_t)65536)
#define FILE_DISK_RESERVE ((uint64_t)64 * 1024 * 1024)

typedef struct {
    size_t bytes;
    size_t preallocated;
} file_span;

typedef struct {
    uintptr_t base;
    size_t bytes;
    unsigned char *mapped;
} page_region;

struct xc3_heap_pages {
    pthread_mutex_t mutex;
    page_region heap;
    page_region alias;
    size_t host_page_bytes;
    size_t commit_limit;
    size_t heap_committed_bytes;
    size_t alias_committed_bytes;
    bool poisoned;
    int last_vm_errno;
    uintptr_t last_vm_address;
    size_t last_vm_bytes;
    bool last_vm_was_commit;
    char *file_template;
    char *file_path;
    file_span *file_spans;
    size_t file_slots;
    xc3_heap_page_stats file_state;
};

static bool valid_region(const void *base, size_t bytes, size_t page) {
    uintptr_t address = (uintptr_t)base;
    return address && bytes && !(address % page) && !(bytes % page) &&
           bytes <= UINTPTR_MAX - address;
}

static bool contains(const page_region *region, uintptr_t base, size_t bytes) {
    return base >= region->base && bytes <= region->bytes &&
           base - region->base <= region->bytes - bytes;
}

xc3_allocator_status xc3_heap_pages_create(const xc3_heap_pages_config *config,
                                           xc3_heap_pages **out) {
    long native_page = sysconf(_SC_PAGESIZE);
    if (!config || !out || native_page <= 0 ||
        config->host_page_bytes != (size_t)native_page ||
        config->host_page_bytes < HORIZON_PAGE_BYTES ||
        config->host_page_bytes > HEAP_UNIT_BYTES ||
        (config->host_page_bytes & (config->host_page_bytes - 1)) ||
        !config->commit_limit || config->commit_limit > config->heap_bytes ||
        !valid_region(config->heap_base, config->heap_bytes, config->host_page_bytes) ||
        !valid_region(config->alias_base, config->alias_bytes, config->host_page_bytes))
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    static const char suffix[] = "/xc3-heap-XXXXXX";
    size_t directory_bytes = 0;
    if (config->file_backed_heap) {
        if (!config->file_backing_directory || config->file_backing_directory[0] != '/')
            return XC3_ALLOCATOR_INVALID_ARGUMENT;
        directory_bytes = strnlen(config->file_backing_directory, PATH_MAX);
        if (directory_bytes > PATH_MAX - sizeof suffix || config->heap_bytes > INT64_MAX)
            return XC3_ALLOCATOR_INVALID_ARGUMENT;
    }
    uintptr_t heap = (uintptr_t)config->heap_base;
    uintptr_t alias = (uintptr_t)config->alias_base;
    if (heap < alias + config->alias_bytes && alias < heap + config->heap_bytes)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    xc3_heap_pages *pages = calloc(1, sizeof(*pages));
    if (!pages) return XC3_ALLOCATOR_OUT_OF_MEMORY;
    pages->heap = (page_region){heap, config->heap_bytes, NULL};
    pages->alias = (page_region){alias, config->alias_bytes, NULL};
    pages->heap.mapped = calloc(config->heap_bytes / HORIZON_PAGE_BYTES, 1);
    pages->alias.mapped = calloc(config->alias_bytes / HORIZON_PAGE_BYTES, 1);
    if (config->file_backed_heap) {
        size_t path_bytes = directory_bytes + sizeof suffix;
        pages->file_template = malloc(path_bytes);
        pages->file_path = malloc(path_bytes);
        pages->file_slots = config->heap_bytes / HEAP_UNIT_BYTES +
                            (config->heap_bytes % HEAP_UNIT_BYTES != 0);
        pages->file_spans = calloc(pages->file_slots, sizeof(*pages->file_spans));
        if (pages->file_template)
            snprintf(pages->file_template, path_bytes, "%s%s",
                     config->file_backing_directory, suffix);
    }
    if (!pages->heap.mapped || !pages->alias.mapped ||
        (config->file_backed_heap && (!pages->file_template || !pages->file_path ||
                                      !pages->file_spans)) ||
        pthread_mutex_init(&pages->mutex, NULL)) {
        free(pages->heap.mapped);
        free(pages->alias.mapped);
        free(pages->file_template);
        free(pages->file_path);
        free(pages->file_spans);
        free(pages);
        return XC3_ALLOCATOR_OUT_OF_MEMORY;
    }
    pages->host_page_bytes = config->host_page_bytes;
    pages->commit_limit = config->commit_limit;
    pages->file_state.file_backed_heap = config->file_backed_heap;
    *out = pages;
    return XC3_ALLOCATOR_OK;
}

const char *xc3_heap_file_operation_string(xc3_heap_file_operation operation) {
    switch (operation) {
        case XC3_HEAP_FILE_OP_NONE: return "none";
        case XC3_HEAP_FILE_OP_CREATE: return "create";
        case XC3_HEAP_FILE_OP_UNLINK: return "unlink";
        case XC3_HEAP_FILE_OP_STAT: return "stat";
        case XC3_HEAP_FILE_OP_SPACE: return "space";
        case XC3_HEAP_FILE_OP_PREALLOCATE: return "preallocate";
        case XC3_HEAP_FILE_OP_TRUNCATE: return "truncate";
        case XC3_HEAP_FILE_OP_MAP: return "map";
        case XC3_HEAP_FILE_OP_CLOSE: return "close";
        case XC3_HEAP_FILE_OP_RESTORE: return "restore";
        case XC3_HEAP_FILE_OP_DECOMMIT: return "decommit";
        case XC3_HEAP_FILE_OP_ACCOUNTING: return "accounting";
    }
    return "unknown_file_operation";
}

static void file_error(xc3_heap_pages *pages, xc3_heap_file_operation operation,
                       int file_errno, int vm_errno) {
    xc3_heap_page_stats *state = &pages->file_state;
    if (state->file_failure_sequence != UINT64_MAX) ++state->file_failure_sequence;
    state->last_file_operation = operation;
    state->last_file_errno = file_errno;
    state->last_file_vm_errno = vm_errno;
    state->last_file_cleanup_operation = XC3_HEAP_FILE_OP_NONE;
    state->last_file_cleanup_errno = 0;
    state->last_file_rollback_errno = 0;
    state->last_file_rollback_attempted = false;
    state->last_file_rollback_succeeded = false;
}

static void file_cleanup_error(xc3_heap_pages *pages,
                               xc3_heap_file_operation operation, int error) {
    if (pages->file_state.last_file_cleanup_operation == XC3_HEAP_FILE_OP_NONE) {
        pages->file_state.last_file_cleanup_operation = operation;
        pages->file_state.last_file_cleanup_errno = error;
    }
}

static void record_vm_error(xc3_heap_pages *pages, void *base, size_t bytes,
                            int error, bool commit) {
    pages->last_vm_errno = error;
    pages->last_vm_address = (uintptr_t)base;
    pages->last_vm_bytes = bytes;
    pages->last_vm_was_commit = commit;
}

static bool file_restore(xc3_heap_pages *pages, void *base, size_t bytes) {
    pages->file_state.last_file_rollback_attempted = true;
    void *result = mmap(base, bytes, PROT_NONE,
                        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    bool restored = result == base;
    pages->file_state.last_file_rollback_succeeded = restored;
    if (!restored) {
        int error = result == MAP_FAILED ? errno : EFAULT;
        pages->file_state.last_file_rollback_errno = error;
        file_cleanup_error(pages, XC3_HEAP_FILE_OP_RESTORE, error);
        record_vm_error(pages, base, bytes, error, false);
        pages->poisoned = true;
    }
    return restored;
}

/* The page mutex serializes path reuse and the one transient owned fd. */
static xc3_allocator_status file_commit(xc3_heap_pages *pages, void *base,
                                         size_t bytes) {
    size_t slot = ((uintptr_t)base - pages->heap.base) / HEAP_UNIT_BYTES;
    if (slot >= pages->file_slots || pages->file_spans[slot].bytes)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    if (pages->file_state.file_acquisitions == UINT64_MAX) {
        file_error(pages, XC3_HEAP_FILE_OP_ACCOUNTING, EOVERFLOW, 0);
        return XC3_ALLOCATOR_BACKING_IO_ERROR;
    }
    memcpy(pages->file_path, pages->file_template, strlen(pages->file_template) + 1);
    int fd = -1;
    bool created = false, unlinked = false, map_attempted = false;
    bool unexpected_mapping = false;
    size_t preallocated = 0;
    xc3_heap_file_operation operation = XC3_HEAP_FILE_OP_CREATE;
    int error = 0;
    fd = mkstemp(pages->file_path);
    if (fd < 0) { error = errno; goto failed; }
    created = true;
    operation = XC3_HEAP_FILE_OP_UNLINK;
    if (unlink(pages->file_path)) { error = errno; goto failed; }
    unlinked = true;
    struct stat st;
    operation = XC3_HEAP_FILE_OP_STAT;
    if (fstat(fd, &st)) { error = errno; goto failed; }
    if (!S_ISREG(st.st_mode)) { error = EINVAL; goto failed; }
    struct statfs fs;
    operation = XC3_HEAP_FILE_OP_SPACE;
    if (fstatfs(fd, &fs)) { error = errno; goto failed; }
    if (!fs.f_bsize || (uint64_t)fs.f_bavail > UINT64_MAX / (uint64_t)fs.f_bsize) {
        error = EOVERFLOW; goto failed;
    }
    uint64_t available = (uint64_t)fs.f_bavail * (uint64_t)fs.f_bsize;
    if ((uint64_t)bytes > UINT64_MAX - FILE_DISK_RESERVE ||
        available < (uint64_t)bytes + FILE_DISK_RESERVE) {
        error = ENOSPC; goto failed;
    }
    fstore_t storage = {.fst_flags = F_ALLOCATEALL, .fst_posmode = F_PEOFPOSMODE,
                        .fst_offset = 0, .fst_length = (off_t)bytes};
    operation = XC3_HEAP_FILE_OP_PREALLOCATE;
    if (fcntl(fd, F_PREALLOCATE, &storage)) { error = errno; goto failed; }
    if (storage.fst_bytesalloc < (off_t)bytes ||
        (uint64_t)storage.fst_bytesalloc > SIZE_MAX) {
        error = ENOSPC; goto failed;
    }
    preallocated = (size_t)storage.fst_bytesalloc;
    if ((uint64_t)preallocated > available - FILE_DISK_RESERVE) {
        error = ENOSPC; goto failed;
    }
    if (preallocated > SIZE_MAX - pages->file_state.file_preallocated_bytes) {
        operation = XC3_HEAP_FILE_OP_ACCOUNTING; error = EOVERFLOW; goto failed;
    }
    operation = XC3_HEAP_FILE_OP_TRUNCATE;
    if (ftruncate(fd, (off_t)bytes)) { error = errno; goto failed; }
    operation = XC3_HEAP_FILE_OP_MAP;
    map_attempted = true;
    void *mapped = mmap(base, bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_FIXED, fd, 0);
    if (mapped != base) {
        error = mapped == MAP_FAILED ? errno : EFAULT;
        unexpected_mapping = mapped != MAP_FAILED;
        record_vm_error(pages, base, bytes, error, true);
        goto failed;
    }
    operation = XC3_HEAP_FILE_OP_CLOSE;
    int closed = close(fd);
    fd = -1;
    if (closed) {
        error = errno;
        pages->file_state.file_close_unconfirmed = true;
        pages->poisoned = true;
        goto failed;
    }
    pages->file_spans[slot] = (file_span){bytes, preallocated};
    ++pages->file_state.file_live_mappings;
    pages->file_state.file_mapped_bytes += bytes;
    pages->file_state.file_preallocated_bytes += preallocated;
    ++pages->file_state.file_acquisitions;
    return XC3_ALLOCATOR_OK;

failed:
    file_error(pages, operation,
               operation == XC3_HEAP_FILE_OP_MAP ? 0 : error,
               operation == XC3_HEAP_FILE_OP_MAP ? error : 0);
    if (fd >= 0) {
        int closed = close(fd), saved = closed ? errno : 0;
        fd = -1;
        if (closed) {
            file_cleanup_error(pages, XC3_HEAP_FILE_OP_CLOSE, saved);
            pages->file_state.file_close_unconfirmed = true;
            pages->poisoned = true;
        }
    }
    if (created && !unlinked && unlink(pages->file_path))
        file_cleanup_error(pages, XC3_HEAP_FILE_OP_UNLINK, errno);
    bool restored = !map_attempted || file_restore(pages, base, bytes);
    if (unexpected_mapping) pages->poisoned = true;
    /* An ambiguous close must stop the caller even after complete VM rollback.
     * Never retry that descriptor: another thread may already own its number. */
    return restored && !unexpected_mapping ? XC3_ALLOCATOR_BACKING_IO_ERROR
                                           : XC3_ALLOCATOR_BACKEND_INVALID;
}

static xc3_allocator_status file_decommit(xc3_heap_pages *pages, void *base,
                                         size_t bytes) {
    size_t slot = ((uintptr_t)base - pages->heap.base) / HEAP_UNIT_BYTES;
    if (slot >= pages->file_slots || pages->file_spans[slot].bytes != bytes)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    void *mapped = mmap(base, bytes, PROT_NONE,
                        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (mapped != base) {
        int error = mapped == MAP_FAILED ? errno : EFAULT;
        record_vm_error(pages, base, bytes, error, false);
        file_error(pages, XC3_HEAP_FILE_OP_DECOMMIT, 0, error);
        pages->poisoned = true;
        return XC3_ALLOCATOR_BACKEND_INVALID;
    }
    --pages->file_state.file_live_mappings;
    pages->file_state.file_mapped_bytes -= bytes;
    pages->file_state.file_preallocated_bytes -= pages->file_spans[slot].preallocated;
    pages->file_spans[slot] = (file_span){0};
    return XC3_ALLOCATOR_OK;
}

static bool host_page_mapped(const page_region *region, size_t first,
                             size_t count, size_t request_first,
                             size_t request_end, bool after, bool mapped) {
    for (size_t index = first; index < first + count; ++index) {
        bool value = region->mapped[index] != 0;
        if (after && index >= request_first && index < request_end) value = mapped;
        if (value) return true;
    }
    return false;
}

static bool host_page_changes(const xc3_heap_pages *pages, const page_region *region,
                              size_t index, size_t first, size_t end, bool mapped) {
    size_t count = pages->host_page_bytes / HORIZON_PAGE_BYTES;
    bool before = host_page_mapped(region, index, count, first, end, false, mapped);
    bool after = host_page_mapped(region, index, count, first, end, true, mapped);
    return before != after;
}

static bool restore_uncommitted(const xc3_heap_pages *pages, const page_region *region,
                                 size_t first, size_t end) {
    size_t count = pages->host_page_bytes / HORIZON_PAGE_BYTES;
    size_t host_first = first / count * count;
    size_t host_end = (end + count - 1) / count * count;
    size_t run = SIZE_MAX;
    bool restored = true;
    for (size_t index = host_first; index <= host_end; index += count) {
        bool changed = index < host_end &&
                       host_page_changes(pages, region, index, first, end, true);
        if (changed && run == SIZE_MAX) run = index;
        if (!changed && run != SIZE_MAX) {
            if (mprotect((void *)(region->base + run * HORIZON_PAGE_BYTES),
                         (index - run) * HORIZON_PAGE_BYTES, PROT_NONE))
                restored = false;
            run = SIZE_MAX;
        }
    }
    return restored;
}

/* The mutex covers both the VM change and its shared heap/alias budget. */
static xc3_allocator_status change_pages(xc3_heap_pages *pages, page_region *region,
                                         uintptr_t base, size_t bytes, bool mapped,
                                         bool exact_heap) {
    if (!bytes || base % HORIZON_PAGE_BYTES || bytes % HORIZON_PAGE_BYTES ||
        !contains(region, base, bytes) ||
        (exact_heap && (base % HEAP_UNIT_BYTES || bytes % HEAP_UNIT_BYTES)))
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&pages->mutex);
    xc3_allocator_status status = XC3_ALLOCATOR_OK;
    if (pages->poisoned) {
        status = XC3_ALLOCATOR_BACKEND_INVALID;
        goto done;
    }
    size_t first = (base - region->base) / HORIZON_PAGE_BYTES;
    size_t end = first + bytes / HORIZON_PAGE_BYTES;
    if (exact_heap) {
        for (size_t index = first; index < end; ++index) {
            if ((region->mapped[index] != 0) == mapped) {
                status = XC3_ALLOCATOR_INVALID_ARGUMENT;
                goto done;
            }
        }
    }
    size_t count = pages->host_page_bytes / HORIZON_PAGE_BYTES;
    size_t host_first = first / count * count;
    size_t host_end = (end + count - 1) / count * count;
    size_t changed_bytes = 0;
    for (size_t index = host_first; index < host_end; index += count)
        if (host_page_changes(pages, region, index, first, end, mapped))
            changed_bytes += pages->host_page_bytes;
    size_t used = pages->heap_committed_bytes + pages->alias_committed_bytes;
    if (mapped && (used > pages->commit_limit ||
                   changed_bytes > pages->commit_limit - used)) {
        status = XC3_ALLOCATOR_OUT_OF_MEMORY;
        goto done;
    }
    if (exact_heap && pages->file_state.file_backed_heap) {
        status = mapped ? file_commit(pages, (void *)base, bytes)
                        : file_decommit(pages, (void *)base, bytes);
        if (status != XC3_ALLOCATOR_OK) goto done;
        goto publish;
    }
    size_t run = SIZE_MAX;
    for (size_t index = host_first; index <= host_end; index += count) {
        bool changed = index < host_end &&
                       host_page_changes(pages, region, index, first, end, mapped);
        if (changed && run == SIZE_MAX) run = index;
        if (!changed && run != SIZE_MAX) {
            void *address = (void *)(region->base + run * HORIZON_PAGE_BYTES);
            size_t length = (index - run) * HORIZON_PAGE_BYTES;
            if (mapped) {
                if (mprotect(address, length, PROT_READ | PROT_WRITE)) {
                    pages->last_vm_errno = errno;
                    pages->last_vm_address = (uintptr_t)address;
                    pages->last_vm_bytes = length;
                    pages->last_vm_was_commit = true;
                    if (!restore_uncommitted(pages, region, first, end)) {
                        pages->poisoned = true;
                        status = XC3_ALLOCATOR_BACKEND_INVALID;
                    } else status = XC3_ALLOCATOR_OUT_OF_MEMORY;
                    goto done;
                }
            } else {
                /* Replace only fully unused, owned host pages, ensuring zero on reuse. */
                void *result = mmap(address, length, PROT_NONE,
                                    MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
                if (result != address) {
                    pages->last_vm_errno = errno;
                    pages->last_vm_address = (uintptr_t)address;
                    pages->last_vm_bytes = length;
                    pages->last_vm_was_commit = false;
                    pages->poisoned = true;
                    status = XC3_ALLOCATOR_BACKEND_INVALID;
                    goto done;
                }
            }
            run = SIZE_MAX;
        }
    }
    if (mapped && !exact_heap) {
        /* A live neighbor can keep an old subpage mapped; new logical pages
         * still need zero contents, without touching fresh demand-zero spans. */
        for (size_t index = first; index < end; ++index) {
            size_t host = index / count * count;
            if (!region->mapped[index] &&
                host_page_mapped(region, host, count, first, end, false, mapped))
                memset((void *)(region->base + index * HORIZON_PAGE_BYTES), 0,
                       HORIZON_PAGE_BYTES);
        }
    }
publish:
    memset(region->mapped + first, mapped ? 1 : 0, end - first);
    size_t *charged = region == &pages->heap ? &pages->heap_committed_bytes
                                            : &pages->alias_committed_bytes;
    if (mapped) *charged += changed_bytes;
    else *charged -= changed_bytes;
done:
    pthread_mutex_unlock(&pages->mutex);
    return status;
}

xc3_allocator_status xc3_heap_pages_commit(void *context, void *base, size_t bytes) {
    xc3_heap_pages *pages = context;
    if (!pages) return XC3_ALLOCATOR_INVALID_ARGUMENT;
    return change_pages(pages, &pages->heap, (uintptr_t)base, bytes, true, true);
}

xc3_allocator_status xc3_heap_pages_decommit(void *context, void *base, size_t bytes) {
    xc3_heap_pages *pages = context;
    if (!pages) return XC3_ALLOCATOR_INVALID_ARGUMENT;
    return change_pages(pages, &pages->heap, (uintptr_t)base, bytes, false, true);
}

xc3_allocator_status xc3_heap_pages_map_alias(xc3_heap_pages *pages, uintptr_t base,
                                             size_t bytes, bool mapped) {
    if (!pages) return XC3_ALLOCATOR_INVALID_ARGUMENT;
    return change_pages(pages, &pages->alias, base, bytes, mapped, false);
}

xc3_allocator_status xc3_heap_pages_query(xc3_heap_pages *pages, uintptr_t address,
                                         xc3_heap_page_span *out) {
    if (!pages || !out) return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&pages->mutex);
    xc3_allocator_status status = XC3_ALLOCATOR_NOT_FOUND;
    if (pages->poisoned) status = XC3_ALLOCATOR_BACKEND_INVALID;
    else {
        page_region *region = contains(&pages->heap, address, 1) ? &pages->heap :
                              contains(&pages->alias, address, 1) ? &pages->alias : NULL;
        if (region) {
            size_t index = (address - region->base) / HORIZON_PAGE_BYTES;
            size_t first = index, end = index + 1;
            size_t count = region->bytes / HORIZON_PAGE_BYTES;
            unsigned char mapped = region->mapped[index];
            while (first && region->mapped[first - 1] == mapped) --first;
            while (end < count && region->mapped[end] == mapped) ++end;
            *out = (xc3_heap_page_span){region->base + first * HORIZON_PAGE_BYTES,
                                       (end - first) * HORIZON_PAGE_BYTES, mapped != 0};
            status = XC3_ALLOCATOR_OK;
        }
    }
    pthread_mutex_unlock(&pages->mutex);
    return status;
}

xc3_allocator_status xc3_heap_pages_stats(xc3_heap_pages *pages,
                                         xc3_heap_page_stats *out) {
    if (!pages || !out) return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&pages->mutex);
    *out = pages->file_state;
    out->heap_committed_bytes = pages->heap_committed_bytes;
    out->alias_committed_bytes = pages->alias_committed_bytes;
    out->commit_limit = pages->commit_limit;
    out->poisoned = pages->poisoned;
    out->last_vm_errno = pages->last_vm_errno;
    out->last_vm_address = pages->last_vm_address;
    out->last_vm_bytes = pages->last_vm_bytes;
    out->last_vm_was_commit = pages->last_vm_was_commit;
    pthread_mutex_unlock(&pages->mutex);
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_heap_pages_destroy(xc3_heap_pages *pages) {
    if (!pages) return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&pages->mutex);
    xc3_allocator_status status = pages->poisoned ? XC3_ALLOCATOR_BACKEND_INVALID :
        pages->heap_committed_bytes || pages->alias_committed_bytes ? XC3_ALLOCATOR_BUSY :
        XC3_ALLOCATOR_OK;
    pthread_mutex_unlock(&pages->mutex);
    if (status != XC3_ALLOCATOR_OK) return status;
    /* Destruction, like the core's destruction, requires external quiescence. */
    pthread_mutex_destroy(&pages->mutex);
    free(pages->heap.mapped);
    free(pages->alias.mapped);
    free(pages->file_template);
    free(pages->file_path);
    free(pages->file_spans);
    free(pages);
    return XC3_ALLOCATOR_OK;
}
