#ifndef XC3_ALLOCATOR_CORE_H
#define XC3_ALLOCATOR_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XC3_ALLOCATOR_CLASS_COUNT 52u
#define XC3_ALLOCATOR_LARGE_GROUP_COUNT 16u
#define XC3_ALLOCATOR_SNAPSHOT_BYTES 0x3818u

typedef struct xc3_allocator_core xc3_allocator_core;

typedef enum {
    XC3_ALLOCATOR_OK = 0,
    XC3_ALLOCATOR_INVALID_ARGUMENT,
    XC3_ALLOCATOR_OUT_OF_MEMORY,
    XC3_ALLOCATOR_BACKEND_INVALID,
    XC3_ALLOCATOR_NOT_FOUND,
    XC3_ALLOCATOR_BUSY,
    XC3_ALLOCATOR_SHUT_DOWN,
    XC3_ALLOCATOR_LIMIT_REACHED,
    /* Allocation was not published and its guest mapping/accounting is unchanged.
     * This is not a physical-commit OOM or a request to run pressure callbacks. */
    XC3_ALLOCATOR_BACKING_IO_ERROR
} xc3_allocator_status;

typedef struct {
    size_t reserved_bytes;
    size_t committed_bytes;
} xc3_allocator_backing_info;

typedef struct {
    void *context;
    xc3_allocator_status (*allocate)(void *context, size_t reserve_bytes,
                                     size_t commit_bytes, size_t alignment,
                                     void **out);
    xc3_allocator_status (*free)(void *context, void *live_base);
    xc3_allocator_status (*query)(void *context, const void *live_base,
                                  xc3_allocator_backing_info *out);
} xc3_allocator_backend;

typedef struct {
    xc3_allocator_backend backend;
    void *heap_base;
    /* Reserved VA bound and real commit capacity are distinct. */
    size_t heap_capacity;
    size_t commit_capacity;
    uint64_t budget;
} xc3_allocator_config;

typedef struct {
    uint64_t pointer;
    uint64_t usable_bytes;
    uint64_t charged_bytes;
    uint64_t extent_bytes;
    uint64_t owner;
} xc3_allocator_record;

typedef void (*xc3_allocator_live_visitor)(void *context,
                                         const xc3_allocator_record *record);

typedef struct {
    uint64_t live_allocations;
    uint64_t small_slabs;
    uint64_t empty_small_slabs;
    uint64_t small_unit_bytes;
    uint64_t small_actual_bytes;
    uint64_t empty_small_unit_bytes;
    uint64_t empty_small_actual_bytes;
    uint64_t live_large_ranges;
    uint64_t live_large_c_bytes;
    uint64_t live_large_r_bytes;
    uint64_t live_large_actual_bytes;
    uint64_t backing_ranges;
    uint64_t acquisitions;
    uint64_t highwater; /* Historical highest accepted reserve end offset. */
} xc3_allocator_memory_stats;

typedef struct {
    uint32_t index;
    uint32_t group_index;
    uint64_t slot_bytes;
    uint64_t unit_bytes;
    uint64_t slots_per_unit;
    uint64_t alignment_bit;
    uint64_t reserved_extent_bytes;
    bool small;
} xc3_allocator_class;

typedef struct {
    xc3_allocator_class allocation_class;
    size_t prefix_bytes;
    size_t usable_bytes;
    size_t charged_bytes;
    size_t extent_bytes;
    size_t backend_bytes;
    size_t backend_alignment;
} xc3_allocator_plan;

typedef enum {
    XC3_ALLOCATOR_BACKEND_ERROR_NONE = 0,
    XC3_ALLOCATOR_BACKEND_ERROR_BASE_RANGE,
    XC3_ALLOCATOR_BACKEND_ERROR_ALIGNMENT,
    XC3_ALLOCATOR_BACKEND_ERROR_REQUEST_OVERLAP,
    XC3_ALLOCATOR_BACKEND_ERROR_SHORT_SIZE,
    XC3_ALLOCATOR_BACKEND_ERROR_SIZE_RANGE,
    XC3_ALLOCATOR_BACKEND_ERROR_SIZE_OVERLAP,
    XC3_ALLOCATOR_BACKEND_ERROR_COMMIT_SIZE,
    XC3_ALLOCATOR_BACKEND_ERROR_QUERY_FAILED,
    XC3_ALLOCATOR_BACKEND_ERROR_ALLOCATE_FAILED,
    XC3_ALLOCATOR_BACKEND_ERROR_RELEASE_FAILED
} xc3_allocator_backend_error_reason;

typedef struct {
    xc3_allocator_backend_error_reason reason;
    uintptr_t pointer;
    size_t requested_bytes;
    size_t required_alignment;
    size_t reported_bytes;
    size_t required_commit_bytes;
    size_t reported_committed_bytes;
    uint64_t quarantined_candidates;
} xc3_allocator_backend_error;

typedef struct {
    uint64_t live_allocations;
    uint64_t small_slabs;
    uint64_t live_large_ranges;
    uint64_t released_ranges;
    uint64_t quarantined_candidates;
    bool forced;
} xc3_allocator_shutdown_report;

/* These pure functions describe only the default guest flags (1, 0). */
xc3_allocator_status xc3_allocator_core_class(uint32_t index,
                                             xc3_allocator_class *out);
xc3_allocator_status xc3_allocator_core_plan(size_t bytes, size_t alignment,
                                            xc3_allocator_plan *out);

/* Callbacks run under the registry mutex and must not reenter this core.
 * Registry metadata uses host allocation, never the supplied backend.
 * The backend context and actual reserved address span remain valid through
 * shutdown. Allocate/query leave outputs unchanged on failure. Query is only
 * called after base/alignment/expected-reserve validation. A successful query
 * describes an owned reserved span and its accessible committed prefix.
 * Free returns OK after release or safe retirement behind backend-managed
 * leases that prevent reuse. Errors disable normal use without canceling the
 * live record. Retry teardown requires quiescence. */
xc3_allocator_status xc3_allocator_core_create(const xc3_allocator_config *config,
                                              xc3_allocator_core **out);

/* Record and owner outputs are unchanged on failure. Only live payload bases
 * are accepted by free/query_info. query_owner also accepts live raw-slot and
 * large reserved-tail addresses; a NULL owner can be a successful result. */
xc3_allocator_status xc3_allocator_core_allocate(xc3_allocator_core *core,
                                                size_t bytes, size_t alignment,
                                                uintptr_t owner,
                                                xc3_allocator_record *out);
xc3_allocator_status xc3_allocator_core_free(xc3_allocator_core *core,
                                            const void *payload,
                                            xc3_allocator_record *out);
xc3_allocator_status xc3_allocator_core_query_info(xc3_allocator_core *core,
                                                  const void *payload,
                                                  xc3_allocator_record *out);
/* Hold the registry mutex through a short visitor of an exact live payload.
 * The visitor must not reenter the core or SDK/backend, retain record, or wait
 * for an operation needing this mutex. The temporary record is read-only.
 * A live payload shorter than minimum_usable_bytes is INVALID_ARGUMENT; a
 * foreign/interior/freed payload is NOT_FOUND. Failure never calls visitor.
 * This pins current backing only; registration identity/generation remains
 * the caller's responsibility. Core destruction still needs quiescence. */
xc3_allocator_status xc3_allocator_core_visit_live(
    xc3_allocator_core *core, const void *exact_payload,
    size_t minimum_usable_bytes, xc3_allocator_live_visitor visitor,
    void *context);
xc3_allocator_status xc3_allocator_core_query_owner(xc3_allocator_core *core,
                                                   const void *address,
                                                   uintptr_t *out);
bool xc3_allocator_core_owns_range(xc3_allocator_core *core, const void *address);

/* Snapshot is a little-endian 0x3818-byte guest record. Its arena header uses
 * the documented bounded-backend policy, not the original 60 GiB arena. */
xc3_allocator_status xc3_allocator_core_snapshot(xc3_allocator_core *core,
                                                void *out, size_t bytes);
/* One mutex-protected integer pair; neither output changes on failure.
 * Guest gates can apply their own saved FPCR/FPSR to these exact operands. */
xc3_allocator_status xc3_allocator_core_usage(xc3_allocator_core *core,
                                             uint64_t *out_budget,
                                             uint64_t *out_used);
/* Host-only accounting; actual_bytes is the validated committed prefix K.
 * One mutex protects the complete live/retained snapshot. Requires an active
 * core and leaves out unchanged on failure. This function allocates nothing. */
xc3_allocator_status xc3_allocator_core_memory_stats(
    xc3_allocator_core *core, xc3_allocator_memory_stats *out);
/* This portable helper uses the host C floating-point environment. */
xc3_allocator_status xc3_allocator_core_ratio(xc3_allocator_core *core,
                                             float *out);
xc3_allocator_status xc3_allocator_core_backend_error(
    xc3_allocator_core *core, xc3_allocator_backend_error *out);

/* Invalid backend results disable further allocation. An untrusted returned
 * pointer is quarantined without query/free when those calls are unsafe;
 * the owner must retain the diagnostic and finalize its backend instance. */

/* Without force, live allocations produce BUSY and no cleanup. With force,
 * all validated backing is released. No caller may use old payloads afterward.
 * destroy requires successful shutdown and externally quiescent callers. */
xc3_allocator_status xc3_allocator_core_shutdown(
    xc3_allocator_core *core, bool force, xc3_allocator_shutdown_report *out);
xc3_allocator_status xc3_allocator_core_destroy(xc3_allocator_core *core);

const char *xc3_allocator_core_status_string(xc3_allocator_status status);

#ifdef __cplusplus
}
#endif

#endif
