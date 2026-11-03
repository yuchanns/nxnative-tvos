#pragma once

#include "xc3_allocator_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct xc3_heap_pages xc3_heap_pages;

typedef struct {
    void *heap_base;
    size_t heap_bytes;
    void *alias_base;
    size_t alias_bytes;
    size_t host_page_bytes;
    size_t commit_limit;
    bool file_backed_heap;
    const char *file_backing_directory;
} xc3_heap_pages_config;

typedef enum {
    XC3_HEAP_FILE_OP_NONE = 0,
    XC3_HEAP_FILE_OP_CREATE,
    XC3_HEAP_FILE_OP_UNLINK,
    XC3_HEAP_FILE_OP_STAT,
    XC3_HEAP_FILE_OP_SPACE,
    XC3_HEAP_FILE_OP_PREALLOCATE,
    XC3_HEAP_FILE_OP_TRUNCATE,
    XC3_HEAP_FILE_OP_MAP,
    XC3_HEAP_FILE_OP_CLOSE,
    XC3_HEAP_FILE_OP_RESTORE,
    XC3_HEAP_FILE_OP_DECOMMIT,
    XC3_HEAP_FILE_OP_ACCOUNTING
} xc3_heap_file_operation;

const char *xc3_heap_file_operation_string(xc3_heap_file_operation operation);

typedef struct {
    uintptr_t base;
    size_t bytes;
    bool committed;
} xc3_heap_page_span;

typedef struct {
    size_t heap_committed_bytes;
    size_t alias_committed_bytes;
    size_t commit_limit;
    bool poisoned;
    int last_vm_errno;
    uintptr_t last_vm_address;
    size_t last_vm_bytes;
    bool last_vm_was_commit;
    bool file_backed_heap;
    size_t file_live_mappings;
    size_t file_mapped_bytes;
    /* Sum of reported fst_bytesalloc for held mappings, not disk usage. */
    size_t file_preallocated_bytes;
    uint64_t file_acquisitions;
    uint64_t file_failure_sequence;
    xc3_heap_file_operation last_file_operation;
    int last_file_errno;
    int last_file_vm_errno;
    xc3_heap_file_operation last_file_cleanup_operation;
    int last_file_cleanup_errno;
    int last_file_rollback_errno;
    bool last_file_rollback_attempted;
    bool last_file_rollback_succeeded;
    bool file_close_unconfirmed;
} xc3_heap_page_stats;

/* The caller owns both initially PROT_NONE regions for the provider's lifetime.
 * File mode copies the caller's absolute temporary directory at creation.
 * It maps one fresh regular file per complete 64 KiB-aligned heap commit;
 * alias pages and the default anonymous mode retain their original behavior.
 * BACKING_IO_ERROR leaves the failed allocation unpublished and its entire C
 * inaccessible with unchanged logical charge. An unconfirmed close poisons
 * the provider even after VM rollback: callers must stop, not retry cleanup. */
xc3_allocator_status xc3_heap_pages_create(const xc3_heap_pages_config *config,
                                           xc3_heap_pages **out);
xc3_allocator_status xc3_heap_pages_destroy(xc3_heap_pages *pages);

/* Heap operations require exact 64 KiB units; alias operations use Horizon pages. */
xc3_allocator_status xc3_heap_pages_commit(void *context, void *base, size_t bytes);
xc3_allocator_status xc3_heap_pages_decommit(void *context, void *base, size_t bytes);
xc3_allocator_status xc3_heap_pages_map_alias(xc3_heap_pages *pages, uintptr_t base,
                                             size_t bytes, bool mapped);
xc3_allocator_status xc3_heap_pages_query(xc3_heap_pages *pages, uintptr_t address,
                                         xc3_heap_page_span *out);
xc3_allocator_status xc3_heap_pages_stats(xc3_heap_pages *pages,
                                         xc3_heap_page_stats *out);
