#include "xc3_allocator_core.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define UNIT_BYTES ((size_t)65536)
#define SMALL_CLASS_COUNT 21u

_Static_assert(sizeof(xc3_allocator_record) == 0x28, "Guest record size");
_Static_assert(sizeof(uintptr_t) <= sizeof(uint64_t), "Guest pointer width");

typedef struct small_slab small_slab;
typedef struct large_range large_range;

struct small_slab {
    small_slab *next;
    small_slab *available_previous;
    small_slab *available_next;
    unsigned char *base;
    size_t backend_bytes;
    size_t reserved_bytes;
    size_t slot_count;
    size_t free_count;
    size_t next_hint;
    uint32_t class_index;
    uintptr_t *owners;
    uint16_t *prefixes;
};

struct large_range {
    large_range *next;
    large_range *free_next;
    unsigned char *base;
    size_t backend_bytes;
    size_t reserved_bytes;
    xc3_allocator_record record;
    bool live;
};

typedef struct {
    large_range *all;
    large_range *available;
    uint32_t total_count;
    uint32_t free_count;
    uint32_t live_count;
} large_group;

typedef struct {
    uintptr_t base;
    uintptr_t end;
    bool small;
    bool released;
    union {
        small_slab *slab;
        large_range *large;
    } object;
} backing_range;

struct xc3_allocator_core {
    pthread_mutex_t mutex;
    xc3_allocator_config config;
    uintptr_t heap_begin;
    uintptr_t heap_end;
    small_slab *slabs[SMALL_CLASS_COUNT];
    small_slab *available_slabs[SMALL_CLASS_COUNT];
    large_group groups[XC3_ALLOCATOR_LARGE_GROUP_COUNT];
    backing_range *ranges;
    size_t range_count;
    size_t range_capacity;
    uint64_t small_unit_bytes;
    uint64_t live_large_bytes;
    uint64_t acquisitions;
    uint64_t highwater;
    uint64_t live_allocations;
    uint64_t slab_count;
    uint64_t live_large_count;
    xc3_allocator_backend_error backend_error;
    bool active;
    bool poisoned;
    bool backing_unknown;
};

static bool power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static uint64_t gcd_u64(uint64_t left, uint64_t right) {
    while (right != 0) {
        uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

xc3_allocator_status xc3_allocator_core_class(uint32_t index,
                                             xc3_allocator_class *out) {
    if (index >= XC3_ALLOCATOR_CLASS_COUNT || out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    uint32_t half = index / 2;
    uint64_t slot = index < 2 ? 32u * (index + 1u)
                             : (uint64_t)(3u + (index & 1u)) << (half + 4u);
    uint64_t unit = slot / gcd_u64(slot, UNIT_BYTES) * UNIT_BYTES;
    uint32_t group = index < 22 ? 0 : half - 10;
    xc3_allocator_class result = {
        .index = index,
        .group_index = group,
        .slot_bytes = slot,
        .unit_bytes = unit,
        .slots_per_unit = unit / slot,
        .alignment_bit = slot & (~slot + 1u),
        .reserved_extent_bytes = (uint64_t)UNIT_BYTES << group,
        .small = index <= 21 && unit / slot >= 2,
    };
    *out = result;
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_allocator_core_plan(size_t bytes, size_t alignment,
                                            xc3_allocator_plan *out) {
    if (out == NULL || !power_of_two(alignment))
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    size_t prefix = alignment < 16 ? 16 : alignment;
    if (prefix > UINT32_MAX || bytes > UINT32_MAX - prefix ||
        (uint64_t)bytes + prefix > UINT64_C(0x80000000))
        return XC3_ALLOCATOR_LIMIT_REACHED;
    uint64_t need = (uint64_t)bytes + prefix;
    for (uint32_t index = 0; index < XC3_ALLOCATOR_CLASS_COUNT; ++index) {
        xc3_allocator_class allocation_class;
        (void)xc3_allocator_core_class(index, &allocation_class);
        if (allocation_class.slot_bytes < need ||
            ((alignment - 1u) & allocation_class.alignment_bit) != 0)
            continue;
        if (allocation_class.unit_bytes > SIZE_MAX ||
            allocation_class.reserved_extent_bytes > SIZE_MAX)
            return XC3_ALLOCATOR_LIMIT_REACHED;
        xc3_allocator_plan result = {.allocation_class = allocation_class};
        if (allocation_class.small) {
            if (prefix > UINT16_MAX)
                return XC3_ALLOCATOR_LIMIT_REACHED;
            result.prefix_bytes = prefix;
            result.usable_bytes = (size_t)allocation_class.slot_bytes - prefix;
            result.charged_bytes = (size_t)allocation_class.slot_bytes;
            result.extent_bytes = result.charged_bytes;
            result.backend_bytes = (size_t)allocation_class.unit_bytes;
            result.backend_alignment = UNIT_BYTES;
        } else {
            /* A zero-commit large region has a different original query ABI. */
            if (bytes == 0)
                return XC3_ALLOCATOR_INVALID_ARGUMENT;
            result.usable_bytes = (bytes + UNIT_BYTES - 1u) & ~(UNIT_BYTES - 1u);
            result.charged_bytes = result.usable_bytes;
            result.extent_bytes = (size_t)allocation_class.reserved_extent_bytes;
            result.backend_bytes = result.extent_bytes;
            result.backend_alignment = alignment > UNIT_BYTES ? alignment : UNIT_BYTES;
        }
        *out = result;
        return XC3_ALLOCATOR_OK;
    }
    return XC3_ALLOCATOR_LIMIT_REACHED;
}

xc3_allocator_status xc3_allocator_core_create(const xc3_allocator_config *config,
                                              xc3_allocator_core **out) {
    if (config == NULL || out == NULL || config->heap_base == NULL ||
        config->heap_capacity == 0 || config->commit_capacity == 0 ||
        config->commit_capacity > config->heap_capacity ||
        config->backend.allocate == NULL ||
        config->backend.free == NULL || config->backend.query == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    uintptr_t begin = (uintptr_t)config->heap_base;
    if (config->heap_capacity > UINTPTR_MAX - begin)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    xc3_allocator_core *core = calloc(1, sizeof(*core));
    if (core == NULL)
        return XC3_ALLOCATOR_OUT_OF_MEMORY;
    if (pthread_mutex_init(&core->mutex, NULL) != 0) {
        free(core);
        return XC3_ALLOCATOR_OUT_OF_MEMORY;
    }
    core->config = *config;
    core->heap_begin = begin;
    core->heap_end = begin + config->heap_capacity;
    core->active = true;
    *out = core;
    return XC3_ALLOCATOR_OK;
}

static size_t range_lower_bound(const xc3_allocator_core *core, uintptr_t address) {
    size_t begin = 0, end = core->range_count;
    while (begin < end) {
        size_t middle = begin + (end - begin) / 2;
        if (core->ranges[middle].base < address)
            begin = middle + 1;
        else
            end = middle;
    }
    return begin;
}

static size_t containing_range(const xc3_allocator_core *core, uintptr_t address) {
    size_t index = range_lower_bound(core, address);
    if (index < core->range_count && core->ranges[index].base == address)
        return index;
    if (index != 0 && address < core->ranges[index - 1].end)
        return index - 1;
    return SIZE_MAX;
}

static bool range_overlaps(const xc3_allocator_core *core, uintptr_t begin,
                           uintptr_t end) {
    size_t index = range_lower_bound(core, begin);
    return (index != 0 && core->ranges[index - 1].end > begin) ||
           (index < core->range_count && core->ranges[index].base < end);
}

static xc3_allocator_status reserve_range_metadata(xc3_allocator_core *core) {
    if (core->range_count < core->range_capacity)
        return XC3_ALLOCATOR_OK;
    size_t next = core->range_capacity == 0 ? 16 : core->range_capacity * 2;
    if (next < core->range_capacity || next > SIZE_MAX / sizeof(backing_range))
        return XC3_ALLOCATOR_LIMIT_REACHED;
    backing_range *ranges = realloc(core->ranges, next * sizeof(*ranges));
    if (ranges == NULL)
        return XC3_ALLOCATOR_OUT_OF_MEMORY;
    core->ranges = ranges;
    core->range_capacity = next;
    return XC3_ALLOCATOR_OK;
}

static void insert_range(xc3_allocator_core *core, backing_range range) {
    size_t index = range_lower_bound(core, range.base);
    memmove(core->ranges + index + 1, core->ranges + index,
            (core->range_count - index) * sizeof(*core->ranges));
    core->ranges[index] = range;
    ++core->range_count;
    ++core->acquisitions;
    uint64_t offset = (uint64_t)(range.end - core->heap_begin);
    if (offset > core->highwater)
        core->highwater = offset;
}

static void remove_range(xc3_allocator_core *core, size_t index) {
    memmove(core->ranges + index, core->ranges + index + 1,
            (core->range_count - index - 1) * sizeof(*core->ranges));
    --core->range_count;
}

static xc3_allocator_status reject_backend(
    xc3_allocator_core *core, xc3_allocator_backend_error_reason reason,
    uintptr_t pointer, size_t requested, size_t alignment, size_t reported) {
    core->poisoned = true;
    core->backend_error.reason = reason;
    core->backend_error.pointer = pointer;
    core->backend_error.requested_bytes = requested;
    core->backend_error.required_alignment = alignment;
    core->backend_error.reported_bytes = reported;
    core->backend_error.required_commit_bytes = 0;
    core->backend_error.reported_committed_bytes = 0;
    ++core->backend_error.quarantined_candidates;
    return XC3_ALLOCATOR_BACKEND_INVALID;
}

static xc3_allocator_status reject_backing_info(
    xc3_allocator_core *core, xc3_allocator_backend_error_reason reason,
    uintptr_t base, size_t reserve, size_t commit, size_t alignment,
    xc3_allocator_backing_info info) {
    xc3_allocator_status status = reject_backend(
        core, reason, base, reserve, alignment, info.reserved_bytes);
    core->backend_error.required_commit_bytes = commit;
    core->backend_error.reported_committed_bytes = info.committed_bytes;
    return status;
}

static xc3_allocator_status acquire_backing(xc3_allocator_core *core, size_t bytes,
                                            size_t commit_bytes, size_t alignment,
                                            void **out,
                                            xc3_allocator_backing_info *out_info) {
    if (core->acquisitions == UINT64_MAX)
        return XC3_ALLOCATOR_LIMIT_REACHED;
    void *pointer = NULL;
    xc3_allocator_status allocated = core->config.backend.allocate(
        core->config.backend.context, bytes, commit_bytes, alignment, &pointer);
    if (allocated == XC3_ALLOCATOR_OUT_OF_MEMORY)
        return XC3_ALLOCATOR_OUT_OF_MEMORY;
    if (allocated == XC3_ALLOCATOR_BACKING_IO_ERROR && pointer == NULL)
        return allocated;
    if (allocated != XC3_ALLOCATOR_OK)
        return reject_backend(core, XC3_ALLOCATOR_BACKEND_ERROR_ALLOCATE_FAILED,
                              (uintptr_t)pointer, bytes, alignment, 0);
    uintptr_t begin = (uintptr_t)pointer;
    /* Reject an invalid/aliased base before invoking the SDK's unsafe query. */
    if (begin < core->heap_begin || begin >= core->heap_end ||
        bytes > core->heap_end - begin)
        return reject_backend(core, XC3_ALLOCATOR_BACKEND_ERROR_BASE_RANGE,
                              begin, bytes, alignment, 0);
    if ((begin & (alignment - 1u)) != 0)
        return reject_backend(core, XC3_ALLOCATOR_BACKEND_ERROR_ALIGNMENT,
                              begin, bytes, alignment, 0);
    if (range_overlaps(core, begin, begin + bytes))
        return reject_backend(core, XC3_ALLOCATOR_BACKEND_ERROR_REQUEST_OVERLAP,
                              begin, bytes, alignment, 0);
    xc3_allocator_backing_info info = {0};
    if (core->config.backend.query(core->config.backend.context, pointer,
                                    &info) != XC3_ALLOCATOR_OK)
        return reject_backend(core, XC3_ALLOCATOR_BACKEND_ERROR_QUERY_FAILED,
                              begin, bytes, alignment, 0);
    if (info.reserved_bytes < bytes)
        return reject_backing_info(core, XC3_ALLOCATOR_BACKEND_ERROR_SHORT_SIZE,
                                   begin, bytes, commit_bytes, alignment, info);
    if (info.reserved_bytes > core->heap_end - begin)
        return reject_backing_info(core, XC3_ALLOCATOR_BACKEND_ERROR_SIZE_RANGE,
                                   begin, bytes, commit_bytes, alignment, info);
    if (range_overlaps(core, begin, begin + info.reserved_bytes))
        return reject_backing_info(core, XC3_ALLOCATOR_BACKEND_ERROR_SIZE_OVERLAP,
                                   begin, bytes, commit_bytes, alignment, info);
    if (info.committed_bytes < commit_bytes ||
        info.committed_bytes > info.reserved_bytes ||
        info.committed_bytes > core->config.commit_capacity)
        return reject_backing_info(core, XC3_ALLOCATOR_BACKEND_ERROR_COMMIT_SIZE,
                                   begin, bytes, commit_bytes, alignment, info);
    *out = pointer;
    *out_info = info;
    return XC3_ALLOCATOR_OK;
}

static xc3_allocator_status reject_release(xc3_allocator_core *core,
                                           const backing_range *range) {
    core->poisoned = true;
    core->backing_unknown = true;
    core->backend_error.reason = XC3_ALLOCATOR_BACKEND_ERROR_RELEASE_FAILED;
    core->backend_error.pointer = range->base;
    core->backend_error.requested_bytes = range->end - range->base;
    core->backend_error.required_alignment = UNIT_BYTES;
    core->backend_error.reported_bytes = range->end - range->base;
    core->backend_error.reported_committed_bytes = range->small
        ? range->object.slab->backend_bytes : range->object.large->backend_bytes;
    core->backend_error.required_commit_bytes = core->backend_error.reported_committed_bytes;
    return XC3_ALLOCATOR_BACKEND_INVALID;
}

static void available_add(xc3_allocator_core *core, small_slab *slab) {
    small_slab **head = &core->available_slabs[slab->class_index];
    slab->available_previous = NULL;
    slab->available_next = *head;
    if (*head != NULL)
        (*head)->available_previous = slab;
    *head = slab;
}

static void available_remove(xc3_allocator_core *core, small_slab *slab) {
    if (slab->available_previous != NULL)
        slab->available_previous->available_next = slab->available_next;
    else
        core->available_slabs[slab->class_index] = slab->available_next;
    if (slab->available_next != NULL)
        slab->available_next->available_previous = slab->available_previous;
    slab->available_previous = slab->available_next = NULL;
}

static void store_le(unsigned char *where, uint64_t value, size_t bytes) {
    for (size_t index = 0; index < bytes; ++index) {
        where[index] = (unsigned char)value;
        value >>= 8;
    }
}

static void write_small_prefix(unsigned char *raw, size_t prefix, size_t usable,
                                uintptr_t owner) {
    unsigned char *payload = raw + prefix;
    memset(raw, 0, prefix);
    store_le(raw, (uint64_t)owner, 8);
    store_le(payload - 16, (uint64_t)owner, 8);
    store_le(payload - 8, usable, 4);
    size_t value = prefix;
    unsigned int bits = 1;
    while (value > 1) {
        value >>= 1;
        ++bits;
    }
    store_le(payload - 2, bits, 2);
    unsigned char checksum = 0;
    for (size_t index = 0; index < prefix; ++index)
        checksum = (unsigned char)(checksum + raw[index]);
    payload[-4] = checksum;
}

static xc3_allocator_status allocate_small(xc3_allocator_core *core,
                                           const xc3_allocator_plan *plan,
                                           uintptr_t owner,
                                           xc3_allocator_record *out) {
    uint32_t index = plan->allocation_class.index;
    small_slab *slab = core->available_slabs[index];
    if (slab == NULL) {
        slab = calloc(1, sizeof(*slab));
        if (slab == NULL)
            return XC3_ALLOCATOR_OUT_OF_MEMORY;
        slab->slot_count = (size_t)plan->allocation_class.slots_per_unit;
        slab->owners = calloc(slab->slot_count, sizeof(*slab->owners));
        slab->prefixes = calloc(slab->slot_count, sizeof(*slab->prefixes));
        xc3_allocator_status status = XC3_ALLOCATOR_OUT_OF_MEMORY;
        if (slab->owners != NULL && slab->prefixes != NULL)
            status = reserve_range_metadata(core);
        if (status == XC3_ALLOCATOR_OK) {
            void *backing = NULL;
            xc3_allocator_backing_info info;
            status = acquire_backing(core, plan->backend_bytes,
                                     plan->backend_bytes,
                                     plan->backend_alignment, &backing, &info);
            slab->base = backing;
            if (status == XC3_ALLOCATOR_OK) {
                slab->backend_bytes = info.committed_bytes;
                slab->reserved_bytes = info.reserved_bytes;
            }
        }
        if (status != XC3_ALLOCATOR_OK) {
            free(slab->owners);
            free(slab->prefixes);
            free(slab);
            return status;
        }
        slab->class_index = index;
        slab->free_count = slab->slot_count;
        slab->next = core->slabs[index];
        core->slabs[index] = slab;
        available_add(core, slab);
        backing_range range = {
            .base = (uintptr_t)slab->base,
            .end = (uintptr_t)slab->base + slab->reserved_bytes,
            .small = true,
            .object.slab = slab,
        };
        insert_range(core, range);
        core->small_unit_bytes += plan->backend_bytes;
        ++core->slab_count;
    }
    size_t slot = slab->next_hint;
    while (slab->prefixes[slot] != 0)
        slot = (slot + 1u) % slab->slot_count;
    slab->prefixes[slot] = (uint16_t)plan->prefix_bytes;
    slab->owners[slot] = owner;
    slab->next_hint = (slot + 1u) % slab->slot_count;
    if (--slab->free_count == 0)
        available_remove(core, slab);
    unsigned char *raw = slab->base + slot * plan->charged_bytes;
    write_small_prefix(raw, plan->prefix_bytes, plan->usable_bytes, owner);
    xc3_allocator_record record = {
        .pointer = (uintptr_t)(raw + plan->prefix_bytes),
        .usable_bytes = plan->usable_bytes,
        .charged_bytes = plan->charged_bytes,
        .extent_bytes = plan->extent_bytes,
        .owner = owner,
    };
    ++core->live_allocations;
    *out = record;
    return XC3_ALLOCATOR_OK;
}

static xc3_allocator_status allocate_large(xc3_allocator_core *core,
                                           const xc3_allocator_plan *plan,
                                           uintptr_t owner,
                                           xc3_allocator_record *out) {
    large_group *group = &core->groups[plan->allocation_class.group_index];
    large_range *range = group->available;
    bool fresh = range == NULL;
    if (fresh) {
        if (group->total_count == UINT32_MAX)
            return XC3_ALLOCATOR_LIMIT_REACHED;
        range = calloc(1, sizeof(*range));
        if (range == NULL)
            return XC3_ALLOCATOR_OUT_OF_MEMORY;
    }
    xc3_allocator_status status = reserve_range_metadata(core);
    void *backing = NULL;
    xc3_allocator_backing_info info = {0};
    if (status == XC3_ALLOCATOR_OK)
        status = acquire_backing(core, plan->backend_bytes,
                                  plan->charged_bytes,
                                  plan->backend_alignment, &backing, &info);
    if (status != XC3_ALLOCATOR_OK) {
        if (fresh)
            free(range);
        return status;
    }
    if (fresh) {
        range->next = group->all;
        group->all = range;
        ++group->total_count;
    } else {
        group->available = range->free_next;
        --group->free_count;
    }
    range->free_next = NULL;
    range->base = backing;
    range->backend_bytes = info.committed_bytes;
    range->reserved_bytes = info.reserved_bytes;
    range->live = true;
    range->record = (xc3_allocator_record){
        .pointer = (uintptr_t)backing,
        .usable_bytes = plan->usable_bytes,
        .charged_bytes = plan->charged_bytes,
        .extent_bytes = plan->extent_bytes,
        .owner = owner,
    };
    backing_range held = {
        .base = (uintptr_t)backing,
        .end = (uintptr_t)backing + info.reserved_bytes,
        .small = false,
        .object.large = range,
    };
    insert_range(core, held);
    ++group->live_count;
    ++core->live_large_count;
    ++core->live_allocations;
    core->live_large_bytes += plan->charged_bytes;
    *out = range->record;
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_allocator_core_allocate(xc3_allocator_core *core,
                                                size_t bytes, size_t alignment,
                                                uintptr_t owner,
                                                xc3_allocator_record *out) {
    if (core == NULL || out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    xc3_allocator_plan plan;
    xc3_allocator_status status = xc3_allocator_core_plan(bytes, alignment, &plan);
    if (status != XC3_ALLOCATOR_OK)
        return status;
    pthread_mutex_lock(&core->mutex);
    if (!core->active)
        status = XC3_ALLOCATOR_SHUT_DOWN;
    else if (core->poisoned)
        status = XC3_ALLOCATOR_BACKEND_INVALID;
    else if (plan.allocation_class.small)
        status = allocate_small(core, &plan, owner, out);
    else
        status = allocate_large(core, &plan, owner, out);
    pthread_mutex_unlock(&core->mutex);
    return status;
}

static bool small_payload(const small_slab *slab, uintptr_t address,
                           size_t *out_slot, xc3_allocator_record *out) {
    xc3_allocator_class allocation_class;
    (void)xc3_allocator_core_class(slab->class_index, &allocation_class);
    uintptr_t base = (uintptr_t)slab->base;
    size_t offset = (size_t)(address - base);
    if (offset >= allocation_class.unit_bytes)
        return false;
    size_t slot = offset / (size_t)allocation_class.slot_bytes;
    size_t prefix = slab->prefixes[slot];
    uintptr_t payload = base + slot * (size_t)allocation_class.slot_bytes + prefix;
    if (prefix == 0 || payload != address)
        return false;
    *out_slot = slot;
    *out = (xc3_allocator_record){
        .pointer = payload,
        .usable_bytes = allocation_class.slot_bytes - prefix,
        .charged_bytes = allocation_class.slot_bytes,
        .extent_bytes = allocation_class.slot_bytes,
        .owner = slab->owners[slot],
    };
    return true;
}

xc3_allocator_status xc3_allocator_core_query_info(xc3_allocator_core *core,
                                                  const void *payload,
                                                  xc3_allocator_record *out) {
    if (core == NULL || out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&core->mutex);
    xc3_allocator_status status = XC3_ALLOCATOR_NOT_FOUND;
    if (!core->active) {
        status = XC3_ALLOCATOR_SHUT_DOWN;
    } else {
        size_t index = containing_range(core, (uintptr_t)payload);
        if (index != SIZE_MAX) {
            backing_range *range = &core->ranges[index];
            xc3_allocator_record record;
            size_t slot;
            if (range->small && small_payload(range->object.slab,
                                              (uintptr_t)payload, &slot, &record)) {
                *out = record;
                status = XC3_ALLOCATOR_OK;
            } else if (!range->small && range->object.large->live &&
                       range->object.large->record.pointer == (uintptr_t)payload) {
                *out = range->object.large->record;
                status = XC3_ALLOCATOR_OK;
            }
        }
    }
    pthread_mutex_unlock(&core->mutex);
    return status;
}

xc3_allocator_status xc3_allocator_core_visit_live(
    xc3_allocator_core *core, const void *exact_payload,
    size_t minimum_usable_bytes, xc3_allocator_live_visitor visitor,
    void *context) {
    if (core == NULL || visitor == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&core->mutex);
    xc3_allocator_status status = XC3_ALLOCATOR_NOT_FOUND;
    if (!core->active) {
        status = XC3_ALLOCATOR_SHUT_DOWN;
    } else {
        size_t index = containing_range(core, (uintptr_t)exact_payload);
        if (index != SIZE_MAX) {
            backing_range *range = &core->ranges[index];
            xc3_allocator_record record = {0};
            size_t slot;
            bool found;
            if (range->small) {
                found = small_payload(range->object.slab,
                                      (uintptr_t)exact_payload, &slot, &record);
            } else {
                found = range->object.large->live &&
                        range->object.large->record.pointer ==
                            (uintptr_t)exact_payload;
                if (found)
                    record = range->object.large->record;
            }
            if (found) {
                if (record.usable_bytes < minimum_usable_bytes) {
                    status = XC3_ALLOCATOR_INVALID_ARGUMENT;
                } else {
                    visitor(context, &record);
                    status = XC3_ALLOCATOR_OK;
                }
            }
        }
    }
    pthread_mutex_unlock(&core->mutex);
    return status;
}

xc3_allocator_status xc3_allocator_core_free(xc3_allocator_core *core,
                                            const void *payload,
                                            xc3_allocator_record *out) {
    if (core == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&core->mutex);
    xc3_allocator_status status = XC3_ALLOCATOR_NOT_FOUND;
    if (!core->active) {
        status = XC3_ALLOCATOR_SHUT_DOWN;
    } else {
        size_t index = containing_range(core, (uintptr_t)payload);
        if (index != SIZE_MAX) {
            backing_range *held = &core->ranges[index];
            xc3_allocator_record record;
            size_t slot;
            if (held->small && small_payload(held->object.slab, (uintptr_t)payload,
                                             &slot, &record)) {
                small_slab *slab = held->object.slab;
                if (slab->free_count == 0)
                    available_add(core, slab);
                ++slab->free_count;
                slab->prefixes[slot] = 0;
                slab->owners[slot] = 0;
                slab->next_hint = slot;
                --core->live_allocations;
                status = XC3_ALLOCATOR_OK;
            } else if (!held->small && held->object.large->live &&
                       held->object.large->record.pointer == (uintptr_t)payload) {
                large_range *range = held->object.large;
                record = range->record;
                uint32_t group_index = 0;
                uint64_t extent = record.extent_bytes;
                while (extent > UNIT_BYTES) {
                    extent >>= 1;
                    ++group_index;
                }
                large_group *group = &core->groups[group_index];
                if (core->config.backend.free(core->config.backend.context,
                                               range->base) != XC3_ALLOCATOR_OK) {
                    status = reject_release(core, held);
                    pthread_mutex_unlock(&core->mutex);
                    return status;
                }
                remove_range(core, index);
                range->base = NULL;
                range->backend_bytes = 0;
                range->reserved_bytes = 0;
                range->live = false;
                memset(&range->record, 0, sizeof(range->record));
                range->free_next = group->available;
                group->available = range;
                ++group->free_count;
                --group->live_count;
                --core->live_large_count;
                --core->live_allocations;
                core->live_large_bytes -= record.charged_bytes;
                status = XC3_ALLOCATOR_OK;
            }
            if (status == XC3_ALLOCATOR_OK && out != NULL)
                *out = record;
        }
    }
    pthread_mutex_unlock(&core->mutex);
    return status;
}

xc3_allocator_status xc3_allocator_core_query_owner(xc3_allocator_core *core,
                                                   const void *address,
                                                   uintptr_t *out) {
    if (core == NULL || out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    uintptr_t pointer = (uintptr_t)address;
    pthread_mutex_lock(&core->mutex);
    xc3_allocator_status status = XC3_ALLOCATOR_NOT_FOUND;
    if (!core->active) {
        status = XC3_ALLOCATOR_SHUT_DOWN;
    } else {
        size_t index = containing_range(core, pointer);
        if (index != SIZE_MAX) {
            backing_range *range = &core->ranges[index];
            if (range->small) {
                small_slab *slab = range->object.slab;
                xc3_allocator_class allocation_class;
                (void)xc3_allocator_core_class(slab->class_index, &allocation_class);
                size_t offset = (size_t)(pointer - range->base);
                if (offset < allocation_class.unit_bytes) {
                    size_t slot = offset / (size_t)allocation_class.slot_bytes;
                    if (slab->prefixes[slot] != 0) {
                        *out = slab->owners[slot];
                        status = XC3_ALLOCATOR_OK;
                    }
                }
            } else {
                large_range *large = range->object.large;
                if (large->live && pointer - range->base < large->record.extent_bytes) {
                    *out = (uintptr_t)large->record.owner;
                    status = XC3_ALLOCATOR_OK;
                }
            }
        }
    }
    pthread_mutex_unlock(&core->mutex);
    return status;
}

bool xc3_allocator_core_owns_range(xc3_allocator_core *core, const void *address) {
    if (core == NULL)
        return false;
    uintptr_t pointer = (uintptr_t)address;
    pthread_mutex_lock(&core->mutex);
    bool result = core->active && pointer >= core->heap_begin && pointer < core->heap_end;
    pthread_mutex_unlock(&core->mutex);
    return result;
}

xc3_allocator_status xc3_allocator_core_snapshot(xc3_allocator_core *core,
                                                void *out, size_t bytes) {
    if (core == NULL || out == NULL || bytes < XC3_ALLOCATOR_SNAPSHOT_BYTES)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&core->mutex);
    if (!core->active) {
        pthread_mutex_unlock(&core->mutex);
        return XC3_ALLOCATOR_SHUT_DOWN;
    }
    unsigned char *data = out;
    memset(data, 0, XC3_ALLOCATOR_SNAPSHOT_BYTES);
    store_le(data, core->config.budget, 8);
    store_le(data + 8, core->small_unit_bytes + core->live_large_bytes, 8);
    store_le(data + 0x38, core->acquisitions, 8);
    store_le(data + 0x40, core->highwater, 8);
    store_le(data + 0x48, core->config.commit_capacity, 8);
    size_t row_index = 0;
    for (uint32_t index = 0; index < SMALL_CLASS_COUNT; ++index) {
        xc3_allocator_class allocation_class;
        (void)xc3_allocator_core_class(index, &allocation_class);
        unsigned char *row = data + 0x50 + row_index++ * 0x38;
        row[4] = (unsigned char)index;
        store_le(row + 8, allocation_class.slot_bytes, 8);
    }
    for (uint32_t index = 0; index < XC3_ALLOCATOR_LARGE_GROUP_COUNT; ++index) {
        unsigned char *row = data + 0x50 + row_index++ * 0x38;
        large_group *group = &core->groups[index];
        store_le(row, 1, 4);
        row[4] = (unsigned char)(21u + index * 2u);
        store_le(row + 8, (uint64_t)UNIT_BYTES << index, 8);
        store_le(row + 0x10, group->live_count, 8);
        store_le(row + 0x18, group->free_count, 8);
        store_le(row + 0x20, group->total_count, 8);
    }
    pthread_mutex_unlock(&core->mutex);
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_allocator_core_usage(xc3_allocator_core *core,
                                             uint64_t *out_budget,
                                             uint64_t *out_used) {
    if (core == NULL || out_budget == NULL || out_used == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&core->mutex);
    if (!core->active) {
        pthread_mutex_unlock(&core->mutex);
        return XC3_ALLOCATOR_SHUT_DOWN;
    }
    *out_budget = core->config.budget;
    *out_used = core->small_unit_bytes + core->live_large_bytes;
    pthread_mutex_unlock(&core->mutex);
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_allocator_core_memory_stats(
    xc3_allocator_core *core, xc3_allocator_memory_stats *out) {
    if (core == NULL || out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&core->mutex);
    if (!core->active) {
        pthread_mutex_unlock(&core->mutex);
        return XC3_ALLOCATOR_SHUT_DOWN;
    }
    xc3_allocator_memory_stats stats = {0};
    for (uint32_t index = 0; index < SMALL_CLASS_COUNT; ++index) {
        xc3_allocator_class allocation_class;
        (void)xc3_allocator_core_class(index, &allocation_class);
        for (small_slab *slab = core->slabs[index]; slab != NULL; slab = slab->next) {
            ++stats.small_slabs;
            stats.small_unit_bytes += allocation_class.unit_bytes;
            stats.small_actual_bytes += slab->backend_bytes;
            stats.live_allocations += slab->slot_count - slab->free_count;
            if (slab->free_count == slab->slot_count) {
                ++stats.empty_small_slabs;
                stats.empty_small_unit_bytes += allocation_class.unit_bytes;
                stats.empty_small_actual_bytes += slab->backend_bytes;
            }
        }
    }
    for (uint32_t index = 0; index < XC3_ALLOCATOR_LARGE_GROUP_COUNT; ++index) {
        for (large_range *range = core->groups[index].all;
             range != NULL; range = range->next) {
            if (range->live) {
                ++stats.live_allocations;
                ++stats.live_large_ranges;
                stats.live_large_c_bytes += range->record.charged_bytes;
                stats.live_large_r_bytes += range->record.extent_bytes;
                stats.live_large_actual_bytes += range->backend_bytes;
            }
        }
    }
    stats.backing_ranges = core->range_count;
    stats.acquisitions = core->acquisitions;
    stats.highwater = core->highwater;
    *out = stats;
    pthread_mutex_unlock(&core->mutex);
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_allocator_core_ratio(xc3_allocator_core *core, float *out) {
    if (out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    uint64_t budget, used;
    xc3_allocator_status status = xc3_allocator_core_usage(core, &budget, &used);
    if (status != XC3_ALLOCATOR_OK)
        return status;
    volatile float numerator = (float)used;
    volatile float denominator = (float)budget;
    volatile float result = numerator / denominator;
    *out = result;
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_allocator_core_backend_error(
    xc3_allocator_core *core, xc3_allocator_backend_error *out) {
    if (core == NULL || out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&core->mutex);
    *out = core->backend_error;
    pthread_mutex_unlock(&core->mutex);
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_allocator_core_shutdown(
    xc3_allocator_core *core, bool force, xc3_allocator_shutdown_report *out) {
    if (core == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&core->mutex);
    xc3_allocator_shutdown_report report = {
        .live_allocations = core->live_allocations,
        .small_slabs = core->slab_count,
        .live_large_ranges = core->live_large_count,
        .quarantined_candidates = core->backend_error.quarantined_candidates,
        .forced = force,
    };
    if (core->live_allocations != 0 && !force) {
        if (out != NULL)
            *out = report;
        pthread_mutex_unlock(&core->mutex);
        return XC3_ALLOCATOR_BUSY;
    }
    for (size_t index = 0; index < core->range_count; ++index) {
        backing_range *range = &core->ranges[index];
        if (range->released)
            continue;
        if (core->config.backend.free(core->config.backend.context,
                                      (void *)range->base) != XC3_ALLOCATOR_OK) {
            xc3_allocator_status status = reject_release(core, range);
            if (out != NULL)
                *out = report;
            pthread_mutex_unlock(&core->mutex);
            return status;
        }
        range->released = true;
        ++report.released_ranges;
    }
    core->active = false;
    for (uint32_t index = 0; index < XC3_ALLOCATOR_LARGE_GROUP_COUNT; ++index) {
        large_range *range = core->groups[index].all;
        while (range != NULL) {
            large_range *next = range->next;
            free(range);
            range = next;
        }
        memset(&core->groups[index], 0, sizeof(core->groups[index]));
    }
    for (uint32_t index = 0; index < SMALL_CLASS_COUNT; ++index) {
        small_slab *slab = core->slabs[index];
        while (slab != NULL) {
            small_slab *next = slab->next;
            free(slab->owners);
            free(slab->prefixes);
            free(slab);
            slab = next;
        }
        core->slabs[index] = core->available_slabs[index] = NULL;
    }
    free(core->ranges);
    core->ranges = NULL;
    core->range_count = core->range_capacity = 0;
    core->small_unit_bytes = core->live_large_bytes = 0;
    core->live_allocations = core->slab_count = core->live_large_count = 0;
    if (out != NULL)
        *out = report;
    pthread_mutex_unlock(&core->mutex);
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_allocator_core_destroy(xc3_allocator_core *core) {
    if (core == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&core->mutex);
    bool active = core->active;
    pthread_mutex_unlock(&core->mutex);
    if (active)
        return XC3_ALLOCATOR_BUSY;
    pthread_mutex_destroy(&core->mutex);
    free(core);
    return XC3_ALLOCATOR_OK;
}

const char *xc3_allocator_core_status_string(xc3_allocator_status status) {
    switch (status) {
        case XC3_ALLOCATOR_OK: return "ok";
        case XC3_ALLOCATOR_INVALID_ARGUMENT: return "invalid_argument";
        case XC3_ALLOCATOR_OUT_OF_MEMORY: return "out_of_memory";
        case XC3_ALLOCATOR_BACKEND_INVALID: return "backend_invalid";
        case XC3_ALLOCATOR_NOT_FOUND: return "not_found";
        case XC3_ALLOCATOR_BUSY: return "busy";
        case XC3_ALLOCATOR_SHUT_DOWN: return "shut_down";
        case XC3_ALLOCATOR_LIMIT_REACHED: return "limit_reached";
        case XC3_ALLOCATOR_BACKING_IO_ERROR: return "backing_io_error";
    }
    return "unknown_status";
}
