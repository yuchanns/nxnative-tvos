#pragma once

#include <stddef.h>
#include <stdint.h>
#include "xc3_heap_pages.h"
#include "xc3_sparse_arena.h"

enum {
    SVC_HOST_XC3_HEAP_INITIALIZE = 0x90,
    SVC_HOST_XC3_HEAP_SHUTDOWN,
    SVC_HOST_XC3_HEAP_ALLOCATE,
    SVC_HOST_XC3_HEAP_FREE,
    SVC_HOST_XC3_HEAP_OWNS_RANGE,
    SVC_HOST_XC3_HEAP_QUERY_INFO,
    SVC_HOST_XC3_HEAP_QUERY_OWNER,
    SVC_HOST_XC3_HEAP_SNAPSHOT,
    SVC_HOST_XC3_HEAP_RATIO,
};

typedef struct {
    xc3_sparse_arena_stats arena;
    xc3_heap_page_stats pages;
} xc3_heap_storage_accounting;

/* Read arena, then pages, releasing each internal lock before the next read.
 * This is not an atomic cross-layer snapshot. Returns 1 only after both reads
 * succeed, 0 for an inactive/non-sparse port, or -1 for invalid arguments/read
 * failures. Outputs are unchanged on 0/-1. No core/GPU lock, allocation, or
 * logging occurs. Callers must not hold arena/provider locks. Initialization
 * publication and destruction still require external quiescence. */
int xc3_heap_storage_get_accounting(xc3_heap_storage_accounting *out);

size_t dm_guest_heap_capacity(void);
xc3_allocator_status dm_guest_sparse_heap_open(uintptr_t *base, size_t *reserve,
                                               size_t *capacity,
                                               xc3_heap_pages **pages);
xc3_allocator_status dm_guest_sparse_heap_close(void);
int dm_guest_storage_lease_acquire(const void *base, size_t bytes, void **out_lease);
void dm_guest_storage_lease_release(void *lease);
void xc3_heap_self_test(void);
void xc3_heap_port_prepare(void);
int xc3_heap_port_enabled(void);
void xc3_heap_port_self_test(void);
int xc3_heap_svc(int number, uint64_t *registers, uint64_t original_rva);
