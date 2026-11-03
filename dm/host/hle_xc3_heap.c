#include "hle.h"
#include "xc3_allocator_core.h"
#include "xc3_heap.h"
#include "xc3_sparse_arena.h"

#include <errno.h>
#include <math.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    void (*construct)(void *);
    void (*initialize)(void *, void *, size_t);
    void (*finalize)(void *);
    void *(*allocate)(void *, size_t, size_t);
    void (*free)(void *, void *);
    size_t (*size_of)(const void *, const void *);
    size_t (*total_free)(const void *);
    size_t (*allocatable)(const void *);
    void (*cleanup)(const void *);
    uint32_t (*set_heap)(size_t);
    uintptr_t (*heap_address)(void);
    size_t (*heap_size)(void);
    uint32_t (*allocate_block)(uintptr_t *, size_t);
    void (*free_block)(uintptr_t, size_t);
} allocator_api;

typedef struct {
    alignas(64) unsigned char before[64];
    unsigned char storage[256];
    unsigned char after[64];
} guarded_allocator;

typedef struct {
    void *pointer;
    size_t size;
    size_t alignment;
    size_t usable_size;
    unsigned char pattern;
} test_block;

static void *guest_sdk_symbol(const char *name) {
    for (int i = 0; i < g_nmod; i++) {
        module_t *sdk = &g_mod[i];
        if (strcmp(sdk->name, "sdk")) continue;
        if (strcmp(sdk->build_id, "e8cdceb1e77a8465fe20f21928e8d296"))
            die("XB3 allocator: unsupported SDK build %s", sdk->build_id);
        void *symbol = module_sym(sdk, name);
        if (!symbol) die("XB3 allocator: missing SDK symbol %s", name);
        return symbol;
    }
    die("XB3 allocator: SDK module is missing");
}

static allocator_api bind_allocator(void) {
    return (allocator_api){
        .construct = guest_sdk_symbol("_ZN2nn3mem17StandardAllocatorC1Ev"),
        .initialize = guest_sdk_symbol("_ZN2nn3mem17StandardAllocator10InitializeEPvm"),
        .finalize = guest_sdk_symbol("_ZN2nn3mem17StandardAllocator8FinalizeEv"),
        .allocate = guest_sdk_symbol("_ZN2nn3mem17StandardAllocator8AllocateEmm"),
        .free = guest_sdk_symbol("_ZN2nn3mem17StandardAllocator4FreeEPv"),
        .size_of = guest_sdk_symbol("_ZNK2nn3mem17StandardAllocator9GetSizeOfEPKv"),
        .total_free = guest_sdk_symbol("_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv"),
        .allocatable = guest_sdk_symbol("_ZNK2nn3mem17StandardAllocator18GetAllocatableSizeEv"),
        .cleanup = guest_sdk_symbol("_ZNK2nn3mem17StandardAllocator21CleanUpManagementAreaEv"),
        .set_heap = guest_sdk_symbol("_ZN2nn2os17SetMemoryHeapSizeEm"),
        .heap_address = guest_sdk_symbol("_ZN2nn2os20GetMemoryHeapAddressEv"),
        .heap_size = guest_sdk_symbol("_ZN2nn2os17GetMemoryHeapSizeEv"),
        .allocate_block = guest_sdk_symbol("_ZN2nn2os19AllocateMemoryBlockEPmm"),
        .free_block = guest_sdk_symbol("_ZN2nn2os15FreeMemoryBlockEmm"),
    };
}

static void check_pattern(const void *pointer, size_t size, unsigned char pattern) {
    const unsigned char *bytes = pointer;
    for (size_t index = 0; index < size; index++)
        if (bytes[index] != pattern)
            die("XC3 allocator diagnostic: data changed at byte %zu", index);
}

static void check_allocator_guards(const guarded_allocator *object) {
    check_pattern(object->before, sizeof object->before, 0xa7);
    check_pattern(object->after, sizeof object->after, 0xa7);
    // The reviewed interface accesses [0, 0x38); keep a larger guarded slot.
    check_pattern(object->storage + 0x38, sizeof object->storage - 0x38, 0xa7);
}

static void check_block_address(test_block *block, const void *heap, size_t capacity,
                                const allocator_api *api, const void *allocator) {
    uintptr_t address = (uintptr_t)block->pointer;
    uintptr_t base = (uintptr_t)heap;
    if (!address || address % block->alignment || address < base ||
        block->size > capacity || address - base > capacity - block->size)
        die("XC3 allocator diagnostic: invalid block size=%zu alignment=%zu",
            block->size, block->alignment);
    size_t actual = api->size_of(allocator, block->pointer);
    if (actual < block->size || actual > capacity - (address - base))
        die("XC3 allocator diagnostic: invalid size query %zu for %zu", actual,
            block->size);
    block->usable_size = actual;
}

static void check_disjoint(const test_block *block, const test_block *live,
                            size_t count) {
    uintptr_t start = (uintptr_t)block->pointer;
    uintptr_t end = start + block->usable_size;
    for (size_t index = 0; index < count; index++) {
        if (!live[index].pointer || &live[index] == block) continue;
        uintptr_t other = (uintptr_t)live[index].pointer;
        if (start < other + live[index].usable_size && other < end)
            die("XC3 allocator diagnostic: overlapping live blocks");
    }
}

static void test_capacity(const allocator_api *api, size_t capacity) {
    guarded_allocator object = {0};
    memset(object.before, 0xa7, sizeof object.before);
    memset(object.after, 0xa7, sizeof object.after);
    memset(object.storage + 0x38, 0xa7, sizeof object.storage - 0x38);
    void *allocator = object.storage;
    uint32_t result = api->set_heap(capacity);
    if (result || api->heap_size() != capacity)
        die("XC3 allocator diagnostic: SetMemoryHeapSize(%zu) failed %#x",
            capacity, result);
    uintptr_t address = 0;
    result = api->allocate_block(&address, capacity);
    if (result || !address || address != api->heap_address())
        die("XC3 allocator diagnostic: AllocateMemoryBlock(%zu) failed %#x",
            capacity, result);
    void *heap = (void *)address;
    if (mprotect(heap, capacity, PROT_READ | PROT_WRITE))
        die("XC3 allocator diagnostic: native heap protection failed errno=%d", errno);
    LOG("XC3 allocator diagnostic: real heap %p +%zu bytes", heap, capacity);

    api->construct(allocator);
    api->initialize(allocator, heap, capacity);
    api->cleanup(allocator);
    check_allocator_guards(&object);
    size_t initial_free = api->total_free(allocator);
    size_t initial_max = api->allocatable(allocator);
    if (!initial_max || initial_max > initial_free || initial_free > capacity)
        die("XC3 allocator diagnostic: invalid initial accounting %zu/%zu/%zu",
            initial_max, initial_free, capacity);

    test_block blocks[] = {
        {.size = 17, .alignment = 16, .pattern = 0x31},
        {.size = 4097, .alignment = 64, .pattern = 0x42},
        {.size = 65537, .alignment = 4096, .pattern = 0x53},
        {.size = 262145, .alignment = 16384, .pattern = 0x64},
        {.size = 1048577, .alignment = 2097152, .pattern = 0x75},
    };
    for (size_t index = 0; index < sizeof blocks / sizeof blocks[0]; index++) {
        test_block *block = &blocks[index];
        block->pointer = api->allocate(allocator, block->size, block->alignment);
        if (!block->pointer)
            die("XC3 allocator diagnostic: allocation failed %zu/%zu",
                block->size, block->alignment);
        check_block_address(block, heap, capacity, api, allocator);
        check_disjoint(block, blocks, index);
        memset(block->pointer, block->pattern, block->size);
    }
    if (api->total_free(allocator) >= initial_free)
        die("XC3 allocator diagnostic: allocations did not reduce free bytes");

    // Exercise the allocate/copy/free sequence used by the guest realloc wrapper.
    test_block *resized = &blocks[1];
    size_t old_size = resized->size;
    test_block replacement = {
        .pointer = api->allocate(allocator, old_size * 3, resized->alignment),
        .size = old_size * 3,
        .alignment = resized->alignment,
        .pattern = resized->pattern,
    };
    if (!replacement.pointer)
        die("XC3 allocator diagnostic: replacement allocation failed");
    check_block_address(&replacement, heap, capacity, api, allocator);
    check_disjoint(&replacement, blocks, sizeof blocks / sizeof blocks[0]);
    memcpy(replacement.pointer, resized->pointer, old_size);
    check_pattern(replacement.pointer, old_size, resized->pattern);
    api->free(allocator, resized->pointer);
    *resized = replacement;
    memset(resized->pointer, resized->pattern, resized->size);

    size_t before_failure = api->total_free(allocator);
    void *oversized = api->allocate(allocator, capacity + 1, 16);
    if (oversized || api->total_free(allocator) != before_failure)
        die("XC3 allocator diagnostic: oversized allocation changed the pool");
    for (size_t index = 0; index < sizeof blocks / sizeof blocks[0]; index++) {
        check_block_address(&blocks[index], heap, capacity, api, allocator);
        check_pattern(blocks[index].pointer, blocks[index].size, blocks[index].pattern);
    }
    check_allocator_guards(&object);

    static const unsigned free_order[] = {2, 0, 4, 1, 3};
    for (size_t index = 0; index < sizeof free_order / sizeof free_order[0]; index++)
        api->free(allocator, blocks[free_order[index]].pointer);
    size_t raw_free = api->total_free(allocator);
    size_t raw_max = api->allocatable(allocator);
    api->cleanup(allocator);
    size_t final_free = api->total_free(allocator);
    size_t final_max = api->allocatable(allocator);
    LOG("XC3 allocator diagnostic: raw free/max=%zu/%zu; cleaned=%zu/%zu",
        raw_free, raw_max, final_free, final_max);
    if (final_free != initial_free || final_max != initial_max)
        die("XC3 allocator diagnostic: unrecovered pool %zu/%zu, expected %zu/%zu",
            final_free, final_max, initial_free, initial_max);
    api->finalize(allocator);
    check_allocator_guards(&object);
    api->free_block(address, capacity);
    result = api->set_heap(0);
    if (result)
        die("XC3 allocator diagnostic: heap release failed %#x", result);
    LOG("XC3 allocator diagnostic: PASS capacity=%zu free=%zu max=%zu",
        capacity, initial_free, initial_max);
}

void xc3_heap_self_test(void) {
    if (strcmp(dm_main_build(), "82d187fe9ef9be921b8e1b1cddf7bd28"))
        die("XB3 allocator diagnostic requires the supported main build");
    allocator_api api = bind_allocator();
    size_t full_capacity = dm_guest_heap_capacity();
    const size_t small_capacity = 32u << 20;
    if (full_capacity < small_capacity || full_capacity > (4ull << 30))
        die("XC3 allocator diagnostic: invalid application heap capacity %zu",
            full_capacity);
    test_capacity(&api, small_capacity);
    if (full_capacity != small_capacity)
        test_capacity(&api, full_capacity);
    LOG("XC3 allocator diagnostic: PASS; game allocator and nnMain were not entered");
}

_Static_assert(sizeof(xc3_allocator_record) == 0x28, "Guest allocation record ABI");

static const uint64_t port_entries[] = {
    0x12407a0, 0x12409e0, 0x1240b00, 0x1240b30, 0x1240b80,
    0x1240bb0, 0x1240c00, 0x1240c50, 0x1240e20,
};

static struct {
    allocator_api api;
    guarded_allocator object;
    xc3_allocator_core *core;
    uintptr_t heap;
    size_t capacity;
    size_t reserve_capacity;
    size_t initial_free;
    size_t initial_max;
    xc3_sparse_arena *arena;
    xc3_heap_pages *pages;
    xc3_allocator_backend page_backend;
} port;
static module_t *port_main;
static int port_enabled, port_trace;
static bool port_memory_diagnostics;
static bool port_pressure_enabled;
static bool port_sparse_enabled;
static _Thread_local unsigned port_depth;
static _Thread_local bool port_backing_null;
static _Atomic unsigned long long port_calls[9];
static _Atomic unsigned long long port_backend_failures;
static _Atomic unsigned long long port_refusals;
static _Atomic unsigned long long port_pressure_attempts;
static _Atomic unsigned port_commit_observations;

static bool port_report_failure(unsigned long long count) {
    return count <= 8 || (count & (count - 1)) == 0;
}

int xc3_heap_port_enabled(void) { return port_enabled; }

void xc3_heap_port_prepare(void) {
    unsigned count = 0;
    for (int number = SVC_HOST_XC3_HEAP_INITIALIZE;
         number <= SVC_HOST_XC3_HEAP_RATIO; number++)
        count += !!gate_svc_present(number);
    if (!count) return;
    if (count != 9 || strcmp(dm_main_build(), "82d187fe9ef9be921b8e1b1cddf7bd28"))
        die("XB3 allocator requires all nine gates and the supported main build");
    for (int index = 0; index < g_nmod; index++)
        if (!strcmp(g_mod[index].name, "main")) port_main = &g_mod[index];
    if (!port_main || port_main->text_end <= 0x1243580)
        die("XC3 allocator port: main module is missing or too small");
    (void)guest_sdk_symbol("_ZN2nn2os17GetMemoryHeapSizeEv");
    port_enabled = 1;
    port_trace = getenv("DM_XC3_ALLOCATOR_PORT_SELF_TEST") != NULL ||
                 getenv("DM_XC3_ALLOCATOR_PORT_TRACE") != NULL;
    const char *diagnostics = getenv("DM_XC3_ALLOCATOR_MEMORY_DIAGNOSTICS");
    port_memory_diagnostics = diagnostics && !strcmp(diagnostics, "1");
    const char *pressure = getenv("DM_XC3_ALLOCATOR_PRESSURE");
    port_pressure_enabled = pressure && !strcmp(pressure, "1");
    port_sparse_enabled = true;
    LOG("XC3 allocator port: nine offline entry gates enabled; actual bounded heap policy");
    if (port_pressure_enabled)
        LOG("XC3 allocator port: exact pressure-leaf notification opt-in enabled");
    if (port_sparse_enabled)
        LOG("XB3 allocator: bounded sparse reserve/commit backend enabled");
}

static void port_require(xc3_allocator_status status, const char *operation) {
    if (status == XC3_ALLOCATOR_OK) return;
    if (status == XC3_ALLOCATOR_BACKEND_INVALID && port.core) {
        xc3_allocator_backend_error error = {0};
        xc3_allocator_core_backend_error(port.core, &error);
        die("XC3 allocator port: %s: invalid backend reason=%u ptr=%#zx request=%zu "
            "alignment=%zu reported=%zu commit=%zu/%zu quarantined=%llu", operation,
            (unsigned)error.reason, (size_t)error.pointer, error.requested_bytes,
            error.required_alignment, error.reported_bytes,
            error.required_commit_bytes, error.reported_committed_bytes,
            (unsigned long long)error.quarantined_candidates);
    }
    die("XC3 allocator port: %s: %s", operation,
        xc3_allocator_core_status_string(status));
}

static void port_sparse_stats(unsigned long long count) {
    xc3_sparse_arena_stats arena = {0};
    xc3_heap_page_stats pages = {0};
    xc3_sparse_arena_get_stats(port.arena, &arena);
    xc3_heap_pages_stats(port.pages, &pages);
    LOG("XC3 sparse backing #%llu post-failure: reserved=%llu committed=%llu "
        "charged=%llu uncertain=%llu pending-R/C=%llu/%llu leases=%llu "
        "VM heap/alias/limit=%zu/%zu/%zu VM-error=%d addr=%#zx bytes=%zu "
        "commit=%u poison=%u/%u", count,
        (unsigned long long)arena.reserved_bytes,
        (unsigned long long)arena.committed_bytes,
        (unsigned long long)arena.charged_bytes,
        (unsigned long long)arena.uncertain_bytes,
        (unsigned long long)arena.pending_reserved_bytes,
        (unsigned long long)arena.pending_committed_bytes,
        (unsigned long long)arena.lease_count,
        pages.heap_committed_bytes, pages.alias_committed_bytes, pages.commit_limit,
        pages.last_vm_errno, (size_t)pages.last_vm_address, pages.last_vm_bytes,
        (unsigned)pages.last_vm_was_commit, (unsigned)arena.poisoned,
        (unsigned)pages.poisoned);
}

static xc3_allocator_status port_commit_pages(void *context, void *base, size_t bytes) {
    unsigned observation = 0;
    if (port_memory_diagnostics && bytes >= (64u << 20)) {
        unsigned count = atomic_load_explicit(&port_commit_observations, memory_order_relaxed);
        while (count < 16) {
            if (atomic_compare_exchange_weak_explicit(
                    &port_commit_observations, &count, count + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                observation = count + 1;
                break;
            }
        }
    }
    xc3_heap_page_stats before = {0}, after = {0};
    xc3_allocator_status before_status = XC3_ALLOCATOR_NOT_FOUND;
    if (observation) before_status = xc3_heap_pages_stats(context, &before);
    xc3_allocator_status status = xc3_heap_pages_commit(context, base, bytes);
    /* Arena metadata/VA exhaustion must not impersonate a physical commit failure. */
    if (status == XC3_ALLOCATOR_OUT_OF_MEMORY) port_backing_null = true;
    if (observation) {
        xc3_allocator_status after_status = xc3_heap_pages_stats(context, &after);
        LOG("XC3 provider commit #%u: base=%#zx bytes=%zu status=%s "
            "before-status=%s heap/alias/limit=%zu/%zu/%zu "
            "after-status=%s heap/alias/limit=%zu/%zu/%zu "
            "VM-error=%d poison=%u/%u atomic-snapshot=0", observation,
            (size_t)base, bytes, xc3_allocator_core_status_string(status),
            xc3_allocator_core_status_string(before_status),
            before.heap_committed_bytes, before.alias_committed_bytes, before.commit_limit,
            xc3_allocator_core_status_string(after_status),
            after.heap_committed_bytes, after.alias_committed_bytes, after.commit_limit,
            after.last_vm_errno, (unsigned)before.poisoned, (unsigned)after.poisoned);
    }
    if (observation || status == XC3_ALLOCATOR_BACKING_IO_ERROR ||
        status == XC3_ALLOCATOR_BACKEND_INVALID) {
        xc3_heap_page_stats file = {0};
        if (xc3_heap_pages_stats(context, &file) == XC3_ALLOCATOR_OK && file.file_backed_heap)
            LOG("XC3 file backing commit #%u: status=%s live-mappings=%zu mapped-bytes=%zu "
                "live-preallocated-bytes=%zu acquisitions=%llu failure-seq=%llu "
                "last-op=%s file-errno=%d map-errno=%d cleanup-op=%s cleanup-errno=%d "
                "rollback=%u/%u/%d close-unconfirmed=%u atomic-snapshot=0",
                observation, xc3_allocator_core_status_string(status),
                file.file_live_mappings, file.file_mapped_bytes, file.file_preallocated_bytes,
                (unsigned long long)file.file_acquisitions,
                (unsigned long long)file.file_failure_sequence,
                xc3_heap_file_operation_string(file.last_file_operation),
                file.last_file_errno, file.last_file_vm_errno,
                xc3_heap_file_operation_string(file.last_file_cleanup_operation),
                file.last_file_cleanup_errno, (unsigned)file.last_file_rollback_attempted,
                (unsigned)file.last_file_rollback_succeeded, file.last_file_rollback_errno,
                (unsigned)file.file_close_unconfirmed);
    }
    return status;
}

static xc3_allocator_status port_allocate_backing(void *context, size_t bytes,
                                                  size_t commit, size_t alignment,
                                                  void **out) {
    (void)context;
    void *pointer = NULL;
    xc3_allocator_status status;
    if (port.arena) {
        status = port.page_backend.allocate(port.page_backend.context,
                                             bytes, commit, alignment, &pointer);
    } else {
        pointer = port.api.allocate(port.object.storage, bytes, alignment);
        status = pointer ? XC3_ALLOCATOR_OK : XC3_ALLOCATOR_OUT_OF_MEMORY;
    }
    if (status == XC3_ALLOCATOR_OUT_OF_MEMORY) {
        if (!port.arena) port_backing_null = true;
        unsigned long long count = atomic_fetch_add_explicit(
            &port_backend_failures, 1, memory_order_relaxed) + 1;
        if (port_report_failure(count)) {
            if (port.arena) {
                LOG("XC3 allocator backing failure #%llu: reserve=%zu commit=%zu "
                    "alignment=%zu sparse out_of_memory commit-witness=%u", count,
                    bytes, commit, alignment, (unsigned)port_backing_null);
                port_sparse_stats(count);
                return status;
            }
            // The core still holds its registry mutex, so backing cannot change here.
            size_t free_bytes = port.api.total_free(port.object.storage);
            size_t max_bytes = port.api.allocatable(port.object.storage);
            LOG("XC3 allocator backing failure #%llu: request=%zu alignment=%zu "
                "SDK free=%zu max=%zu", count, bytes, alignment, free_bytes, max_bytes);
        }
    }
    if (status == XC3_ALLOCATOR_OK) *out = pointer;
    return status;
}

static xc3_allocator_status port_free_backing(void *context, void *pointer) {
    (void)context;
    if (port.arena) return port.page_backend.free(port.page_backend.context, pointer);
    port.api.free(port.object.storage, pointer);
    return XC3_ALLOCATOR_OK;
}

static xc3_allocator_status port_backing_query(void *context, const void *pointer,
                                               xc3_allocator_backing_info *out) {
    (void)context;
    if (port.arena)
        return port.page_backend.query(port.page_backend.context, pointer, out);
    size_t actual = port.api.size_of(port.object.storage, pointer);
    *out = (xc3_allocator_backing_info){actual, actual};
    return XC3_ALLOCATOR_OK;
}

typedef struct {
    xc3_sparse_arena *arena;
    xc3_sparse_lease token;
} storage_lease;

int xc3_heap_storage_get_accounting(xc3_heap_storage_accounting *out) {
    if (!out) return -1;
    if (!port_sparse_enabled || !port.arena || !port.pages) return 0;
    xc3_heap_storage_accounting snapshot = {0};
    if (xc3_sparse_arena_get_stats(port.arena, &snapshot.arena) != XC3_ALLOCATOR_OK)
        return -1;
    if (!snapshot.arena.active) return 0;
    if (xc3_heap_pages_stats(port.pages, &snapshot.pages) != XC3_ALLOCATOR_OK)
        return -1;
    *out = snapshot;
    return 1;
}

int dm_guest_storage_lease_acquire(const void *base, size_t bytes, void **out_lease) {
    if (!port_sparse_enabled || !port.arena) return 0;
    uintptr_t address = (uintptr_t)base;
    if (!bytes || bytes > UINTPTR_MAX - address || !out_lease) return -1;
    uintptr_t end = address + bytes, heap_end = port.heap + port.reserve_capacity;
    if (address >= heap_end || end <= port.heap) return 0;
    if (address < port.heap || end > heap_end) return -1;
    storage_lease *lease = calloc(1, sizeof(*lease));
    if (!lease) return -1;
    xc3_allocator_status status = xc3_sparse_arena_acquire_lease(
        port.arena, address, bytes, &lease->token);
    if (status != XC3_ALLOCATOR_OK) {
        free(lease);
        LOG("XC3 sparse storage lease rejected: base=%#zx bytes=%zu status=%s",
            (size_t)address, bytes, xc3_allocator_core_status_string(status));
        return -1;
    }
    lease->arena = port.arena;
    *out_lease = lease;
    return 1;
}

void dm_guest_storage_lease_release(void *context) {
    storage_lease *lease = context;
    if (!lease) return;
    xc3_allocator_status status = xc3_sparse_arena_release_lease(lease->arena, lease->token);
    if (status != XC3_ALLOCATOR_OK)
        die("XC3 sparse storage lease release failed: %s",
            xc3_allocator_core_status_string(status));
    free(lease);
}

static void port_initialize(uint64_t requested, unsigned flag_a, unsigned flag_b) {
    if (port.core || requested != 0xf00000000ull || flag_a != 1 || flag_b)
        die("XC3 allocator port: unsupported initialization reserve=%#llx flags=%u/%u",
            (unsigned long long)requested, flag_a, flag_b);
    port.api = bind_allocator();
    if (port.api.heap_size())
        die("XC3 allocator port: SDK heap is already in use");
    port.capacity = dm_guest_heap_capacity();
    if (port.capacity < (32u << 20))
        die("XC3 allocator port: application heap is too small");
    uint32_t result;
    if (port_sparse_enabled) {
        port_require(dm_guest_sparse_heap_open(&port.heap, &port.reserve_capacity,
                                               &port.capacity, &port.pages), "open sparse heap");
        size_t admission_limit = dm_guest_heap_capacity();
        if (port.capacity > admission_limit)
            die("Sparse provider commit limit exceeds the original guest capacity");
        long host_page = sysconf(_SC_PAGESIZE);
        if (host_page <= 0) die("XC3 allocator port: no native page size");
        xc3_sparse_arena_config sparse = {
            .base = (void *)port.heap, .span_bytes = port.reserve_capacity,
            .commit_limit = admission_limit, .page_size = (size_t)host_page,
            .vm = {.context = port.pages, .commit = port_commit_pages,
                   .decommit = xc3_heap_pages_decommit},
        };
        port_require(xc3_sparse_arena_create(&sparse, &port.arena), "create sparse arena");
        port.page_backend = xc3_sparse_arena_backend(port.arena);
    } else {
        result = port.api.set_heap(port.capacity);
        if (result || port.api.heap_size() != port.capacity)
            die("XC3 allocator port: SetMemoryHeapSize(%zu) failed %#x", port.capacity, result);
        result = port.api.allocate_block(&port.heap, port.capacity);
        if (result || !port.heap || port.heap != port.api.heap_address())
            die("XC3 allocator port: AllocateMemoryBlock failed %#x", result);
        memset(&port.object, 0, sizeof port.object);
        memset(port.object.before, 0xa7, sizeof port.object.before);
        memset(port.object.after, 0xa7, sizeof port.object.after);
        memset(port.object.storage + 0x38, 0xa7, sizeof port.object.storage - 0x38);
        port.api.construct(port.object.storage);
        port.api.initialize(port.object.storage, (void *)port.heap, port.capacity);
        port.api.cleanup(port.object.storage);
        check_allocator_guards(&port.object);
        port.initial_free = port.api.total_free(port.object.storage);
        port.initial_max = port.api.allocatable(port.object.storage);
        port.reserve_capacity = port.capacity;
    }

    // Keep the original independent budget source; it is not the new heap size.
    uint64_t budget = ((uint64_t (*)(void))(port_main->base + 0x1243580))();
    xc3_allocator_config config = {
        .backend = {
            .context = &port,
            .allocate = port_allocate_backing,
            .free = port_free_backing,
            .query = port_backing_query,
        },
        .heap_base = (void *)port.heap,
        .heap_capacity = port.reserve_capacity,
        .commit_capacity = port.capacity,
        .budget = budget,
    };
    port_require(xc3_allocator_core_create(&config, &port.core), "create");
    if (port.arena) {
        LOG("XC3 allocator port: requested virtual policy=%#llx; actual reserved heap="
            "%#zx+%zu commit-limit=%zu independent-budget=%llu",
            (unsigned long long)requested, (size_t)port.heap, port.reserve_capacity,
            port.capacity, (unsigned long long)budget);
    } else LOG("XC3 allocator port: requested virtual policy=%#llx; real heap=%#zx+%zu "
        "free/max=%zu/%zu budget=%llu", (unsigned long long)requested,
        (size_t)port.heap, port.capacity, port.initial_free, port.initial_max,
        (unsigned long long)budget);
}

static void port_shutdown(void) {
    if (!port.core) die("XC3 allocator port: shutdown before initialization");
    xc3_allocator_shutdown_report report = {0};
    port_require(xc3_allocator_core_shutdown(port.core, false, &report), "shutdown");
    port_require(xc3_allocator_core_destroy(port.core), "destroy");
    port.core = NULL;
    if (port.arena) {
        port_require(xc3_sparse_arena_shutdown(port.arena), "shutdown sparse arena");
        port_require(xc3_sparse_arena_destroy(port.arena), "destroy sparse arena");
        port.arena = NULL;
        port_require(dm_guest_sparse_heap_close(), "close sparse heap");
        LOG("XC3 allocator port: shutdown released %llu ranges; sparse heap commits=0",
            (unsigned long long)report.released_ranges);
        return;
    }
    port.api.cleanup(port.object.storage);
    size_t free_bytes = port.api.total_free(port.object.storage);
    size_t max_bytes = port.api.allocatable(port.object.storage);
    if (free_bytes != port.initial_free || max_bytes != port.initial_max)
        die("XC3 allocator port: unrecovered SDK heap %zu/%zu expected %zu/%zu",
            free_bytes, max_bytes, port.initial_free, port.initial_max);
    port.api.finalize(port.object.storage);
    check_allocator_guards(&port.object);
    port.api.free_block(port.heap, port.capacity);
    uint32_t result = port.api.set_heap(0);
    if (result || port.api.heap_size())
        die("XC3 allocator port: SDK heap release failed %#x", result);
    LOG("XC3 allocator port: shutdown released %llu ranges; SDK heap restored and size=0",
        (unsigned long long)report.released_ranges);
}

static bool port_read_shadow(size_t offset, uint64_t *out) {
    if (offset > port_main->size || sizeof(uint64_t) > port_main->size - offset)
        return false;
    *out = __atomic_load_n((const uint64_t *)(port_main->base + offset),
                           __ATOMIC_RELAXED);
    return true;
}

static bool port_pressure_registration(uintptr_t userdata) {
    uint64_t dispatcher, callback, context;
    return port_read_shadow(0x1c40850, &dispatcher) &&
           port_read_shadow(0x234d968, &callback) &&
           port_read_shadow(0x234d970, &context) &&
           dispatcher == (uintptr_t)port_main->base + 0x128e5b0 &&
           callback == (uintptr_t)port_main->base + 0xd0ef98 &&
           context == userdata && userdata;
}

static bool port_pressure_code_matches(void) {
    static const uint32_t wrapper[] = {
        0xf00085e8, 0xf944b502, 0xb4000082, 0xf00085e8,
        0xf944b901, 0xd61f0040, 0xd65f03c0,
    };
    static const uint32_t leaf[] = {
        0x52864308, 0x8b080028, 0x52800029, 0x089ffd09, 0xd65f03c0,
    };
    return port_main->text_end >= 0x128e5b0 + sizeof wrapper &&
           port_main->text_end >= 0xd0ef98 + sizeof leaf &&
           !memcmp(port_main->base + 0x128e5b0, wrapper, sizeof wrapper) &&
           !memcmp(port_main->base + 0xd0ef98, leaf, sizeof leaf);
}

typedef struct {
    uintptr_t userdata;
    size_t commit_bytes;
    unsigned char before, after;
    bool notified;
} pressure_notification;

static void port_pressure_visit(void *context, const xc3_allocator_record *record) {
    pressure_notification *notification = context;
    // Recheck registration while the exact live allocation is pinned against Free/reuse.
    if (!port_pressure_registration(notification->userdata)) return;
    unsigned char *flag = (unsigned char *)(uintptr_t)record->pointer + 0x3218;
    notification->before = __atomic_load_n(flag, __ATOMIC_ACQUIRE);
    // The validated original leaf is only a release-store; it cannot reenter the core.
    // Use the captured context instead of a wrapper that would reload mutable globals.
    void (*notify)(uint64_t, uintptr_t) = (void *)(port_main->base + 0xd0ef98);
    notify(notification->commit_bytes, notification->userdata);
    notification->after = __atomic_load_n(flag, __ATOMIC_ACQUIRE);
    notification->notified = true;
}

static bool port_notify_pressure(size_t bytes, size_t alignment,
                                 unsigned long long *out_attempt) {
    if (!port_pressure_enabled) return false;
    unsigned long long attempt = atomic_fetch_add_explicit(
        &port_pressure_attempts, 1, memory_order_relaxed) + 1;
    uint64_t userdata = 0;
    xc3_allocator_plan plan;
    xc3_allocator_status status = XC3_ALLOCATOR_INVALID_ARGUMENT;
    pressure_notification notification = {0};
    bool code_matches = port_pressure_code_matches();
    bool registered = port_read_shadow(0x234d970, &userdata) &&
                      port_pressure_registration(userdata);
    xc3_allocator_status planned = xc3_allocator_core_plan(bytes, alignment, &plan);
    if (code_matches && registered && planned == XC3_ALLOCATOR_OK) {
        notification.userdata = userdata;
        notification.commit_bytes = plan.allocation_class.small
                                        ? plan.backend_bytes : plan.charged_bytes;
        status = xc3_allocator_core_visit_live(
            port.core, (void *)(uintptr_t)userdata, 0x3219,
            port_pressure_visit, &notification);
    }
    if (port_report_failure(attempt))
        LOG("XC3 allocator pressure #%llu: original leaf notified=%u status=%s "
            "code=%u registration=%u plan=%s commit=%zu context=%#llx flag=%u/%u", attempt,
            (unsigned)notification.notified, xc3_allocator_core_status_string(status),
            (unsigned)code_matches, (unsigned)registered,
            xc3_allocator_core_status_string(planned),
            notification.commit_bytes, (unsigned long long)userdata,
            (unsigned)notification.before, (unsigned)notification.after);
    *out_attempt = attempt;
    return notification.notified;
}

static xc3_allocator_status port_allocate_record(size_t bytes, size_t alignment,
                                                 uintptr_t owner,
                                                 xc3_allocator_record *record) {
    port_backing_null = false;
    xc3_allocator_status status = xc3_allocator_core_allocate(
        port.core, bytes, alignment, owner, record);
    unsigned long long attempt;
    if (status != XC3_ALLOCATOR_OUT_OF_MEMORY || !port_backing_null ||
        !port_notify_pressure(bytes, alignment, &attempt))
        return status;
    // Match one original 1 ms retry interval without running the asynchronous consumer.
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
    nanosleep(&pause, NULL);
    port_backing_null = false;
    status = xc3_allocator_core_allocate(port.core, bytes, alignment, owner, record);
    if (port_report_failure(attempt))
        LOG("XC3 allocator pressure #%llu: one real allocation retry=%s", attempt,
            xc3_allocator_core_status_string(status));
    return status;
}

static void port_log_refusal(size_t bytes, size_t alignment, uintptr_t owner,
                             xc3_allocator_status status) {
    unsigned long long count = atomic_fetch_add_explicit(
        &port_refusals, 1, memory_order_relaxed) + 1;
    if (!port_report_failure(count)) return;
    LOG("XC3 allocator port: allocation refused #%llu size=%zu alignment=%zu "
        "owner=%#zx: %s", count, bytes, alignment, (size_t)owner,
        xc3_allocator_core_status_string(status));
    xc3_allocator_plan plan;
    xc3_allocator_status planned = xc3_allocator_core_plan(bytes, alignment, &plan);
    if (planned == XC3_ALLOCATOR_OK)
        LOG("XC3 allocator refusal #%llu plan: class=%u small=%u U=%zu C=%zu "
            "R=%zu backend=%zu alignment=%zu", count,
            plan.allocation_class.index, (unsigned)plan.allocation_class.small,
            plan.usable_bytes, plan.charged_bytes, plan.extent_bytes,
            plan.backend_bytes, plan.backend_alignment);
    static const size_t shadow_offsets[] = {
        0x1c40818, 0x1c40810, 0x1c40848, 0x1c40850,
        0x1c40820, 0x234d968, 0x234d970,
    };
    uint64_t shadow[sizeof shadow_offsets / sizeof shadow_offsets[0]];
    bool shadow_valid = true;
    for (size_t index = 0; index < sizeof shadow / sizeof shadow[0]; index++) {
        if (!port_read_shadow(shadow_offsets[index], &shadow[index])) {
            shadow_valid = false;
            break;
        }
    }
    // These original globals remain independent of the new backend's accounting.
    if (shadow_valid)
        LOG("XC3 allocator refusal #%llu original shadow (individual loads): "
            "probe-budget=%llu probe-success=%llu emergency=%#llx "
            "callback=%#llx committed=%llu frontend-callback=%#llx userdata=%#llx",
            count, (unsigned long long)shadow[0], (unsigned long long)shadow[1],
            (unsigned long long)shadow[2], (unsigned long long)shadow[3],
            (unsigned long long)shadow[4], (unsigned long long)shadow[5],
            (unsigned long long)shadow[6]);
    xc3_allocator_memory_stats stats;
    if (xc3_allocator_core_memory_stats(port.core, &stats) != XC3_ALLOCATOR_OK)
        return;
    // Other threads may advance after the failing allocation releases its mutex.
    LOG("XC3 allocator refusal #%llu post-failure: live=%llu small=%llu "
        "small-unit/K=%llu/%llu empty=%llu empty-unit/K=%llu/%llu "
        "large=%llu large-C/R/K=%llu/%llu/%llu ranges=%llu acquired=%llu "
        "highwater=%llu", count, (unsigned long long)stats.live_allocations,
        (unsigned long long)stats.small_slabs,
        (unsigned long long)stats.small_unit_bytes,
        (unsigned long long)stats.small_actual_bytes,
        (unsigned long long)stats.empty_small_slabs,
        (unsigned long long)stats.empty_small_unit_bytes,
        (unsigned long long)stats.empty_small_actual_bytes,
        (unsigned long long)stats.live_large_ranges,
        (unsigned long long)stats.live_large_c_bytes,
        (unsigned long long)stats.live_large_r_bytes,
        (unsigned long long)stats.live_large_actual_bytes,
        (unsigned long long)stats.backing_ranges,
        (unsigned long long)stats.acquisitions,
        (unsigned long long)stats.highwater);
}

static uint32_t port_ratio_bits(uint64_t *registers) {
    uint64_t budget, used;
    port_require(xc3_allocator_core_usage(port.core, &budget, &used), "ratio usage");
    // These offsets are the saved FPSR/FPCR fields in entry.S::_dm_gate's frame.
    uint64_t *saved_fpsr = (uint64_t *)((unsigned char *)registers + 776);
    uint64_t *saved_fpcr = (uint64_t *)((unsigned char *)registers + 784);
    uint64_t host_fpsr, host_fpcr, calculated_fpsr;
    uint32_t bits;
    __asm__ volatile("mrs %0, fpsr\n\tmrs %1, fpcr"
                     : "=r"(host_fpsr), "=r"(host_fpcr));
    // Execute the original float32 conversion/division under the guest FP state.
    // Restore host state before returning to C; the gate will publish guest FPSR.
    __asm__ volatile(
        "msr fpcr, %[guest_fpcr]\n\t"
        "msr fpsr, %[guest_fpsr]\n\t"
        "ucvtf s0, %[used]\n\t"
        "ucvtf s1, %[budget]\n\t"
        "fdiv s0, s0, s1\n\t"
        "fmov %w[bits], s0\n\t"
        "mrs %[calculated_fpsr], fpsr\n\t"
        "msr fpcr, %[host_fpcr]\n\t"
        "msr fpsr, %[host_fpsr]"
        : [bits] "=&r"(bits), [calculated_fpsr] "=&r"(calculated_fpsr)
        : [used] "r"(used), [budget] "r"(budget),
          [guest_fpcr] "r"(*saved_fpcr), [guest_fpsr] "r"(*saved_fpsr),
          [host_fpcr] "r"(host_fpcr), [host_fpsr] "r"(host_fpsr)
        : "v0", "v1", "memory");
    *saved_fpsr = calculated_fpsr;
    return bits;
}

int xc3_heap_svc(int number, uint64_t *registers, uint64_t original_rva) {
    if (number < SVC_HOST_XC3_HEAP_INITIALIZE || number > SVC_HOST_XC3_HEAP_RATIO)
        return 0;
    unsigned index = (unsigned)(number - SVC_HOST_XC3_HEAP_INITIALIZE);
    if (!port_enabled || original_rva != port_entries[index])
        die("XC3 allocator port: unexpected gate %#x at %#llx", number,
            (unsigned long long)original_rva);
    if (port_depth++) die("XC3 allocator port: recursive guest allocation at gate %#x", number);
    if (port_trace) atomic_fetch_add_explicit(&port_calls[index], 1, memory_order_relaxed);
    if (number == SVC_HOST_XC3_HEAP_INITIALIZE) {
        port_initialize(registers[0], (unsigned)registers[1], (unsigned)registers[2]);
        registers[0] = 0;
        port_depth--;
        return 1;
    }
    if (!port.core) die("XC3 allocator port: gate %#x before initialization", number);
    xc3_allocator_status status;
    switch (number) {
    case SVC_HOST_XC3_HEAP_SHUTDOWN:
        port_shutdown();
        registers[0] = 0;
        break;
    case SVC_HOST_XC3_HEAP_ALLOCATE:
        if (registers[2] || (unsigned)registers[3] || !registers[5])
            die("XC3 allocator port: unsupported allocation flags/output");
        status = port_allocate_record(
            registers[0], registers[1], registers[4],
            (xc3_allocator_record *)(uintptr_t)registers[5]);
        if (status != XC3_ALLOCATOR_OK && status != XC3_ALLOCATOR_OUT_OF_MEMORY &&
            status != XC3_ALLOCATOR_LIMIT_REACHED && status != XC3_ALLOCATOR_INVALID_ARGUMENT)
            port_require(status, "allocate");
        if (status != XC3_ALLOCATOR_OK)
            port_log_refusal(registers[0], registers[1], registers[4], status);
        else if (port_memory_diagnostics && registers[0] >= (64u << 20)) {
            const xc3_allocator_record *record = (const void *)(uintptr_t)registers[5];
            LOG("XC3 allocator large allocation: requested=%llu alignment=%llu "
                "P=%#llx U/C/R=%llu/%llu/%llu owner=%#llx",
                (unsigned long long)registers[0], (unsigned long long)registers[1],
                (unsigned long long)record->pointer,
                (unsigned long long)record->usable_bytes,
                (unsigned long long)record->charged_bytes,
                (unsigned long long)record->extent_bytes,
                (unsigned long long)record->owner);
        }
        registers[0] = status == XC3_ALLOCATOR_OK;
        break;
    case SVC_HOST_XC3_HEAP_FREE:
        if (!registers[1]) die("XC3 allocator port: missing free record");
        port_require(xc3_allocator_core_free(port.core, (void *)(uintptr_t)registers[0],
                     (xc3_allocator_record *)(uintptr_t)registers[1]), "free");
        if (port_memory_diagnostics) {
            const xc3_allocator_record *record = (const void *)(uintptr_t)registers[1];
            if (record->extent_bytes >= (64u << 20))
                LOG("XC3 allocator large free: P=%#llx U/C/R=%llu/%llu/%llu owner=%#llx",
                    (unsigned long long)record->pointer,
                    (unsigned long long)record->usable_bytes,
                    (unsigned long long)record->charged_bytes,
                    (unsigned long long)record->extent_bytes,
                    (unsigned long long)record->owner);
        }
        registers[0] = 0;
        break;
    case SVC_HOST_XC3_HEAP_OWNS_RANGE:
        registers[0] = xc3_allocator_core_owns_range(port.core, (void *)(uintptr_t)registers[0]);
        break;
    case SVC_HOST_XC3_HEAP_QUERY_INFO:
        if (!registers[1]) die("XC3 allocator port: missing query record");
        status = xc3_allocator_core_query_info(port.core, (void *)(uintptr_t)registers[0],
                    (xc3_allocator_record *)(uintptr_t)registers[1]);
        if (status != XC3_ALLOCATOR_OK && status != XC3_ALLOCATOR_NOT_FOUND &&
            status != XC3_ALLOCATOR_INVALID_ARGUMENT) port_require(status, "query info");
        registers[0] = status == XC3_ALLOCATOR_OK;
        break;
    case SVC_HOST_XC3_HEAP_QUERY_OWNER:
        if (!registers[1]) die("XC3 allocator port: missing owner output");
        status = xc3_allocator_core_query_owner(port.core, (void *)(uintptr_t)registers[0],
                    (uintptr_t *)(uintptr_t)registers[1]);
        if (status != XC3_ALLOCATOR_OK && status != XC3_ALLOCATOR_NOT_FOUND &&
            status != XC3_ALLOCATOR_INVALID_ARGUMENT) port_require(status, "query owner");
        registers[0] = status == XC3_ALLOCATOR_OK;
        break;
    case SVC_HOST_XC3_HEAP_SNAPSHOT:
        if (!registers[0]) die("XC3 allocator port: missing snapshot output");
        port_require(xc3_allocator_core_snapshot(port.core, (void *)(uintptr_t)registers[0],
                     XC3_ALLOCATOR_SNAPSHOT_BYTES), "snapshot");
        registers[0] = 0;
        break;
    case SVC_HOST_XC3_HEAP_RATIO:
        registers[0] = port_ratio_bits(registers);
        break;
    }
    port_depth--;
    return 1;
}

void xc3_heap_port_self_test(void) {
    if (!port_enabled || !port_main || port.core)
        die("XC3 allocator entry diagnostic requires a fresh opted-in port");
    void (*initialize)(uint64_t, unsigned, unsigned) =
        (void *)(port_main->base + port_entries[0]);
    void (*shutdown)(void) = (void *)(port_main->base + port_entries[1]);
    unsigned (*allocate)(size_t, size_t, uint64_t, unsigned, uintptr_t, xc3_allocator_record *) =
        (void *)(port_main->base + port_entries[2]);
    void (*release)(void *, xc3_allocator_record *) = (void *)(port_main->base + port_entries[3]);
    unsigned (*owns_range)(const void *) = (void *)(port_main->base + port_entries[4]);
    unsigned (*query_info)(const void *, xc3_allocator_record *) =
        (void *)(port_main->base + port_entries[5]);
    unsigned (*query_owner)(const void *, uintptr_t *) =
        (void *)(port_main->base + port_entries[6]);
    void (*snapshot)(void *) = (void *)(port_main->base + port_entries[7]);
    float (*ratio)(void) = (void *)(port_main->base + port_entries[8]);
    initialize(0xf00000000ull, 1, 0);
    if (!port.core || !owns_range((void *)port.heap) ||
        owns_range((void *)(port.heap + port.reserve_capacity)) || owns_range(NULL))
        die("XC3 allocator entry diagnostic: invalid actual heap range");

    static const struct {
        size_t bytes, alignment, usable, charged, extent, prefix;
    } cases[] = {
        {1, 16, 16, 32, 32, 16},
        {17, 16, 48, 64, 64, 16},
        {33, 64, 64, 128, 128, 64},
        {49137, 16, 65536, 65536, 65536, 0},
        {65536, 16, 65536, 65536, 131072, 0},
        {65537, 4096, 131072, 131072, 131072, 0},
    };
    xc3_allocator_record records[sizeof cases / sizeof cases[0]];
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        uintptr_t owner = index % 2 ? 0x12340000 + index : 0;
        if (!allocate(cases[index].bytes, cases[index].alignment, 0, 0, owner, &records[index]))
            die("XC3 allocator entry diagnostic: allocation %zu failed", index);
        const xc3_allocator_record *record = &records[index];
        if (!record->pointer || record->pointer % cases[index].alignment ||
            record->usable_bytes != cases[index].usable ||
            record->charged_bytes != cases[index].charged ||
            record->extent_bytes != cases[index].extent || record->owner != owner)
            die("XC3 allocator entry diagnostic: wrong record %zu", index);
        if (record->pointer < port.heap + cases[index].prefix)
            die("XC3 allocator entry diagnostic: invalid prefix %zu", index);
        uintptr_t raw = record->pointer - cases[index].prefix;
        if (record->extent_bytes > port.reserve_capacity ||
            raw - port.heap > port.reserve_capacity - record->extent_bytes ||
            cases[index].prefix > record->extent_bytes ||
            record->usable_bytes > record->extent_bytes - cases[index].prefix)
            die("XC3 allocator entry diagnostic: range exceeds real heap %zu", index);
        for (size_t previous = 0; previous < index; previous++) {
            uintptr_t other = records[previous].pointer - cases[previous].prefix;
            if (raw < other + records[previous].extent_bytes &&
                other < raw + record->extent_bytes)
                die("XC3 allocator entry diagnostic: overlapping logical blocks");
        }
        xc3_allocator_record queried = {0};
        if (!query_info((void *)(uintptr_t)record->pointer, &queried) ||
            memcmp(record, &queried, sizeof queried))
            die("XC3 allocator entry diagnostic: query record differs %zu", index);
        uintptr_t addresses[] = {raw, record->pointer, raw + record->extent_bytes - 1};
        for (size_t address = 0; address < sizeof addresses / sizeof addresses[0]; address++) {
            uintptr_t actual_owner = UINTPTR_MAX;
            if (!query_owner((void *)addresses[address], &actual_owner) || actual_owner != owner)
                die("XC3 allocator entry diagnostic: wrong interior owner %zu/%zu", index, address);
        }
        memset((void *)(uintptr_t)record->pointer, (int)(0x41 + index), record->usable_bytes);
    }
    xc3_allocator_record sentinel, unchanged;
    memset(&sentinel, 0x9b, sizeof sentinel);
    unchanged = sentinel;
    if (query_info((void *)(uintptr_t)(records[0].pointer + 1), &sentinel) ||
        memcmp(&sentinel, &unchanged, sizeof sentinel))
        die("XC3 allocator entry diagnostic: invalid query changed output");
    if (allocate(port.capacity + 1, 16, 0, 0, 0, &sentinel) ||
        memcmp(&sentinel, &unchanged, sizeof sentinel))
        die("XC3 allocator entry diagnostic: oversized request changed output");

    struct {
        alignas(16) unsigned char before[64];
        uint64_t values[XC3_ALLOCATOR_SNAPSHOT_BYTES / sizeof(uint64_t)];
        unsigned char after[64];
    } state;
    memset(&state, 0xa7, sizeof state);
    snapshot(state.values);
    check_pattern(state.before, sizeof state.before, 0xa7);
    check_pattern(state.after, sizeof state.after, 0xa7);
    if (state.values[1] != 458752 || !state.values[7] || !state.values[8] ||
        state.values[8] > port.reserve_capacity || state.values[9] != port.capacity)
        die("XC3 allocator entry diagnostic: invalid bounded snapshot");
    uint64_t before_fpcr, before_fpsr, expected_fpsr, after_fpcr, after_fpsr;
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr"
                     : "=r"(before_fpcr), "=r"(before_fpsr));
    __asm__ volatile("msr fpsr, %0" : : "r"((uint64_t)0) : "memory");
    // A volatile C reference tests the gate's typed return and published FPSR.
    volatile uint64_t reference_used = state.values[1];
    volatile uint64_t reference_budget = state.values[0];
    volatile float reference_numerator = (float)reference_used;
    volatile float reference_denominator = (float)reference_budget;
    volatile float reference_value = reference_numerator / reference_denominator;
    float expected_value = reference_value;
    __asm__ volatile("mrs %0, fpsr" : "=r"(expected_fpsr) : : "memory");
    __asm__ volatile("msr fpsr, %0" : : "r"((uint64_t)0) : "memory");
    float value = ratio();
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr"
                     : "=r"(after_fpcr), "=r"(after_fpsr) : : "memory");
    __asm__ volatile("msr fpsr, %0" : : "r"(before_fpsr) : "memory");
    uint32_t actual_bits, expected_bits;
    memcpy(&actual_bits, &value, sizeof actual_bits);
    memcpy(&expected_bits, &expected_value, sizeof expected_bits);
    if (after_fpcr != before_fpcr || after_fpsr != expected_fpsr ||
        actual_bits != expected_bits ||
        (!state.values[0] && (!isinf(value) || value < 0 || !(after_fpsr & 2))))
        die("XC3 allocator entry diagnostic: float return or FP state is incorrect");
    LOG("XC3 allocator entry diagnostic: snapshot used=%llu capacity=%llu budget=%llu "
        "ratio=%g FPSR=%#llx", (unsigned long long)state.values[1],
        (unsigned long long)state.values[9], (unsigned long long)state.values[0],
        (double)value, (unsigned long long)after_fpsr);

    static const unsigned order[] = {4, 1, 5, 0, 3, 2};
    for (size_t index = 0; index < sizeof order / sizeof order[0]; index++) {
        unsigned slot = order[index];
        const xc3_allocator_record *record = &records[slot];
        check_pattern((void *)(uintptr_t)record->pointer, record->usable_bytes,
                      (unsigned char)(0x41 + slot));
        xc3_allocator_record released = {0};
        release((void *)(uintptr_t)record->pointer, &released);
        if (memcmp(record, &released, sizeof released))
            die("XC3 allocator entry diagnostic: free record differs %u", slot);
    }
    snapshot(state.values);
    if (state.values[1] != 196608)
        die("XC3 allocator entry diagnostic: retained small slabs are not accounted");
    shutdown();
    for (unsigned index = 0; index < 9; index++)
        if (!atomic_load_explicit(&port_calls[index], memory_order_relaxed))
            die("XC3 allocator entry diagnostic: gate %u was not called", index);
    LOG("XC3 allocator entry diagnostic: PASS all nine original callees, records, "
        "owners, bounded accounting, FP return/state and full SDK heap recovery");
}
