#ifndef XC3_SPARSE_ARENA_H
#define XC3_SPARSE_ARENA_H

#include "xc3_allocator_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xc3_sparse_arena xc3_sparse_arena;

typedef struct {
    void *context;
    /* OUT_OF_MEMORY leaves VM/accounting unchanged or fully restored on
     * return, including verified rollback by the provider. Other
     * failures may be partial; the arena attempts allocation rollback and
     * quarantines uncertain ranges. Callbacks never query arena/core.
     * Decommit OK means the entire prefix is inaccessible, including on
     * rollback of a partially committed or previously uncommitted prefix. */
    xc3_allocator_status (*commit)(void *context, void *base, size_t bytes);
    xc3_allocator_status (*decommit)(void *context, void *base, size_t bytes);
} xc3_sparse_vm;

typedef struct {
    void *base;
    size_t span_bytes;
    size_t commit_limit;
    size_t page_size;
    xc3_sparse_vm vm;
} xc3_sparse_arena_config;

typedef enum {
    XC3_SPARSE_FREE = 0,
    XC3_SPARSE_RESERVED,
    XC3_SPARSE_COMMITTED,
    XC3_SPARSE_FAULTED
} xc3_sparse_span_state;

typedef struct {
    uintptr_t base;
    size_t bytes;
    xc3_sparse_span_state state;
} xc3_sparse_span;

typedef struct {
    uint64_t slot;
    uint64_t generation;
} xc3_sparse_lease;

typedef struct {
    uint64_t span_bytes;
    uint64_t commit_limit;
    uint64_t reserved_bytes;
    uint64_t committed_bytes;
    uint64_t charged_bytes;
    uint64_t uncertain_bytes;
    uint64_t ranges;
    uint64_t faulted_ranges;
    uint64_t pending_ranges;
    uint64_t pending_reserved_bytes;
    uint64_t pending_committed_bytes;
    uint64_t lease_count;
    uint64_t acquisitions;
    uint64_t highwater;
    bool active;
    bool poisoned;
} xc3_sparse_arena_stats;

/* The caller exclusively owns an already-reserved, inaccessible span. This
 * module never creates another VA mapping. Locks nest core -> arena -> VM.
 * Metadata uses host allocation. A VM failure never frees uncertain R. */
xc3_allocator_status xc3_sparse_arena_create(
    const xc3_sparse_arena_config *config, xc3_sparse_arena **out);
xc3_allocator_backend xc3_sparse_arena_backend(xc3_sparse_arena *arena);

/* Query returns an actual contiguous run. RESERVED is userland ownership
 * with no access: its Horizon QueryMemory state is Free/None, not Reserved.
 * FAULTED has unknown VM access and must never be reported as readable.
 * Queries/statistics do not call core/VM and are valid after shutdown. */
xc3_allocator_status xc3_sparse_arena_query_span(
    xc3_sparse_arena *arena, uintptr_t address, xc3_sparse_span *out);
xc3_allocator_status xc3_sparse_arena_get_stats(
    xc3_sparse_arena *arena, xc3_sparse_arena_stats *out);

/* A page-aligned nonempty span must fit one live committed backing; crossing
 * backing boundaries is rejected without partial pinning. Free with leases
 * retires logical ownership but retains R, C, and VM until the last release.
 * Successful release invalidates the token; generation prevents slot ABA.
 * A failed final decommit retains the token/range and poisons the arena. */
xc3_allocator_status xc3_sparse_arena_acquire_lease(
    xc3_sparse_arena *arena, uintptr_t base, size_t bytes,
    xc3_sparse_lease *out);
xc3_allocator_status xc3_sparse_arena_release_lease(
    xc3_sparse_arena *arena, xc3_sparse_lease lease);

/* Teardown requires all core/users to be quiescent. Each successful release
 * is final; a failure retains its range and metadata for a later teardown
 * attempt. Outstanding leases return BUSY without teardown. Destroy requires
 * successful shutdown and never unmaps caller VA. */
xc3_allocator_status xc3_sparse_arena_shutdown(xc3_sparse_arena *arena);
xc3_allocator_status xc3_sparse_arena_destroy(xc3_sparse_arena *arena);

#ifdef __cplusplus
}
#endif

#endif
