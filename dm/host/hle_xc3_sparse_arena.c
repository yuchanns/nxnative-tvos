#include "xc3_sparse_arena.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_UNIT ((size_t)65536)

typedef struct arena_range arena_range;
struct arena_range {
    arena_range *next;
    uintptr_t base;
    size_t reserved;
    size_t committed;
    uint64_t leases;
    bool retired;
    bool faulted;
};

typedef struct {
    arena_range *range;
    uint64_t generation;
    bool active;
} lease_slot;

struct xc3_sparse_arena {
    pthread_mutex_t mutex;
    xc3_sparse_arena_config config;
    uintptr_t end;
    arena_range *ranges;
    lease_slot *leases;
    size_t lease_capacity;
    uint64_t lease_count;
    uint64_t charged;
    uint64_t acquisitions;
    uint64_t highwater;
    bool active;
    bool poisoned;
};

static bool power_of_two(size_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

xc3_allocator_status xc3_sparse_arena_create(
    const xc3_sparse_arena_config *config, xc3_sparse_arena **out) {
    if (config == NULL || out == NULL || config->base == NULL ||
        config->span_bytes == 0 || config->commit_limit == 0 ||
        config->commit_limit > config->span_bytes ||
        !power_of_two(config->page_size) || config->page_size > ARENA_UNIT ||
        ARENA_UNIT % config->page_size != 0 ||
        (uintptr_t)config->base % ARENA_UNIT != 0 ||
        config->span_bytes % ARENA_UNIT != 0 ||
        config->commit_limit % config->page_size != 0 ||
        config->span_bytes > UINTPTR_MAX - (uintptr_t)config->base ||
        config->vm.commit == NULL || config->vm.decommit == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    xc3_sparse_arena *arena = calloc(1, sizeof(*arena));
    if (arena == NULL)
        return XC3_ALLOCATOR_OUT_OF_MEMORY;
    if (pthread_mutex_init(&arena->mutex, NULL) != 0) {
        free(arena);
        return XC3_ALLOCATOR_OUT_OF_MEMORY;
    }
    arena->config = *config;
    arena->end = (uintptr_t)config->base + config->span_bytes;
    arena->active = true;
    *out = arena;
    return XC3_ALLOCATOR_OK;
}

static arena_range *find_range(xc3_sparse_arena *arena, uintptr_t address,
                               arena_range **out_previous) {
    arena_range *previous = NULL;
    for (arena_range *range = arena->ranges; range != NULL; range = range->next) {
        if (address < range->base)
            break;
        if (address - range->base < range->reserved) {
            if (out_previous != NULL)
                *out_previous = previous;
            return range;
        }
        previous = range;
    }
    return NULL;
}

static bool find_gap(xc3_sparse_arena *arena, size_t bytes, size_t alignment,
                     uintptr_t *out, arena_range **out_previous) {
    uintptr_t cursor = (uintptr_t)arena->config.base;
    arena_range *previous = NULL;
    for (arena_range *range = arena->ranges;; range = range->next) {
        if (cursor > UINTPTR_MAX - (alignment - 1u))
            return false;
        uintptr_t start = (cursor + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
        uintptr_t limit = range != NULL ? range->base : arena->end;
        if (start <= limit && bytes <= limit - start) {
            *out = start;
            *out_previous = previous;
            return true;
        }
        if (range == NULL)
            return false;
        cursor = range->base + range->reserved;
        previous = range;
    }
}

static void insert_owned(xc3_sparse_arena *arena, arena_range *range,
                         arena_range *previous) {
    range->next = previous != NULL ? previous->next : arena->ranges;
    if (previous != NULL)
        previous->next = range;
    else
        arena->ranges = range;
    arena->charged += range->committed;
    ++arena->acquisitions;
    uint64_t end = range->base + range->reserved - (uintptr_t)arena->config.base;
    if (end > arena->highwater)
        arena->highwater = end;
}

static xc3_allocator_status arena_allocate(void *context, size_t reserve_bytes,
                                          size_t commit_bytes, size_t alignment,
                                          void **out) {
    xc3_sparse_arena *arena = context;
    if (arena == NULL || out == NULL || reserve_bytes == 0 || commit_bytes == 0 ||
        commit_bytes > reserve_bytes || reserve_bytes % ARENA_UNIT != 0 ||
        commit_bytes % ARENA_UNIT != 0 || !power_of_two(alignment) ||
        alignment < ARENA_UNIT)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&arena->mutex);
    xc3_allocator_status status = XC3_ALLOCATOR_OK;
    if (!arena->active)
        status = XC3_ALLOCATOR_SHUT_DOWN;
    else if (arena->poisoned)
        status = XC3_ALLOCATOR_BACKEND_INVALID;
    else if (arena->acquisitions == UINT64_MAX)
        status = XC3_ALLOCATOR_LIMIT_REACHED;
    else if (arena->charged > arena->config.commit_limit ||
             commit_bytes > arena->config.commit_limit - arena->charged)
        status = XC3_ALLOCATOR_OUT_OF_MEMORY;
    uintptr_t base = 0;
    arena_range *previous = NULL;
    if (status == XC3_ALLOCATOR_OK &&
        !find_gap(arena, reserve_bytes, alignment, &base, &previous))
        status = XC3_ALLOCATOR_OUT_OF_MEMORY;
    arena_range *range = NULL;
    if (status == XC3_ALLOCATOR_OK) {
        range = calloc(1, sizeof(*range));
        if (range == NULL)
            status = XC3_ALLOCATOR_OUT_OF_MEMORY;
    }
    if (status == XC3_ALLOCATOR_OK) {
        range->base = base;
        range->reserved = reserve_bytes;
        range->committed = commit_bytes;
        status = arena->config.vm.commit(arena->config.vm.context,
                                          (void *)base, commit_bytes);
        if (status == XC3_ALLOCATOR_OK) {
            insert_owned(arena, range, previous);
            *out = (void *)base;
        } else if (status == XC3_ALLOCATOR_OUT_OF_MEMORY ||
                   status == XC3_ALLOCATOR_BACKING_IO_ERROR) {
            free(range);
        } else {
            /* A hard VM failure may have changed a prefix. Never reuse R
             * unless the complete attempted prefix was made inaccessible. */
            arena->poisoned = true;
            xc3_allocator_status rolled_back = arena->config.vm.decommit(
                arena->config.vm.context, (void *)base, commit_bytes);
            if (rolled_back == XC3_ALLOCATOR_OK) {
                free(range);
            } else {
                range->faulted = true;
                insert_owned(arena, range, previous);
            }
            status = XC3_ALLOCATOR_BACKEND_INVALID;
        }
    }
    pthread_mutex_unlock(&arena->mutex);
    return status;
}

static xc3_allocator_status release_owned(xc3_sparse_arena *arena,
                                          arena_range *range,
                                          arena_range *previous) {
    xc3_allocator_status status = arena->config.vm.decommit(
        arena->config.vm.context, (void *)range->base, range->committed);
    if (status != XC3_ALLOCATOR_OK) {
        range->faulted = true;
        arena->poisoned = true;
        return XC3_ALLOCATOR_BACKEND_INVALID;
    }
    if (previous != NULL)
        previous->next = range->next;
    else
        arena->ranges = range->next;
    arena->charged -= range->committed;
    free(range);
    return XC3_ALLOCATOR_OK;
}

static xc3_allocator_status arena_free(void *context, void *live_base) {
    xc3_sparse_arena *arena = context;
    if (arena == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&arena->mutex);
    arena_range *previous = NULL;
    arena_range *range = find_range(arena, (uintptr_t)live_base, &previous);
    xc3_allocator_status status = XC3_ALLOCATOR_NOT_FOUND;
    if (!arena->active) {
        status = XC3_ALLOCATOR_SHUT_DOWN;
    } else if (range != NULL && range->base == (uintptr_t)live_base && !range->retired) {
        if (range->leases != 0) {
            range->retired = true;
            status = XC3_ALLOCATOR_OK;
        } else {
            status = release_owned(arena, range, previous);
        }
    }
    pthread_mutex_unlock(&arena->mutex);
    return status;
}

static xc3_allocator_status arena_query(void *context, const void *live_base,
                                       xc3_allocator_backing_info *out) {
    xc3_sparse_arena *arena = context;
    if (arena == NULL || out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&arena->mutex);
    arena_range *range = find_range(arena, (uintptr_t)live_base, NULL);
    xc3_allocator_status status = XC3_ALLOCATOR_NOT_FOUND;
    if (!arena->active)
        status = XC3_ALLOCATOR_SHUT_DOWN;
    else if (range != NULL && range->base == (uintptr_t)live_base && !range->retired) {
        if (range->faulted) {
            status = XC3_ALLOCATOR_BACKEND_INVALID;
        } else {
            *out = (xc3_allocator_backing_info){range->reserved, range->committed};
            status = XC3_ALLOCATOR_OK;
        }
    }
    pthread_mutex_unlock(&arena->mutex);
    return status;
}

xc3_allocator_backend xc3_sparse_arena_backend(xc3_sparse_arena *arena) {
    if (arena == NULL)
        return (xc3_allocator_backend){0};
    return (xc3_allocator_backend){.context = arena, .allocate = arena_allocate,
                                   .free = arena_free, .query = arena_query};
}

typedef struct {
    uintptr_t address;
    xc3_sparse_span current;
    xc3_sparse_span result;
    bool have_current;
    bool found;
} span_builder;

static void emit_span(span_builder *builder, uintptr_t base, uintptr_t end,
                       xc3_sparse_span_state state) {
    if (base == end || builder->found)
        return;
    xc3_sparse_span *current = &builder->current;
    if (builder->have_current && current->state == state &&
        current->base + current->bytes == base) {
        current->bytes += end - base;
        return;
    }
    if (builder->have_current && builder->address >= current->base &&
        builder->address - current->base < current->bytes) {
        builder->result = *current;
        builder->found = true;
        return;
    }
    *current = (xc3_sparse_span){base, end - base, state};
    builder->have_current = true;
}

xc3_allocator_status xc3_sparse_arena_query_span(
    xc3_sparse_arena *arena, uintptr_t address, xc3_sparse_span *out) {
    if (arena == NULL || out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&arena->mutex);
    uintptr_t begin = (uintptr_t)arena->config.base;
    if (address < begin || address >= arena->end) {
        pthread_mutex_unlock(&arena->mutex);
        return XC3_ALLOCATOR_NOT_FOUND;
    }
    span_builder builder = {.address = address};
    uintptr_t cursor = begin;
    for (arena_range *range = arena->ranges; range != NULL && !builder.found;
         range = range->next) {
        emit_span(&builder, cursor, range->base, XC3_SPARSE_FREE);
        if (range->faulted) {
            emit_span(&builder, range->base, range->base + range->reserved,
                       XC3_SPARSE_FAULTED);
        } else {
            emit_span(&builder, range->base, range->base + range->committed,
                       XC3_SPARSE_COMMITTED);
            emit_span(&builder, range->base + range->committed,
                       range->base + range->reserved, XC3_SPARSE_RESERVED);
        }
        cursor = range->base + range->reserved;
    }
    if (!builder.found) {
        emit_span(&builder, cursor, arena->end, XC3_SPARSE_FREE);
        if (!builder.found)
            builder.result = builder.current;
    }
    *out = builder.result;
    pthread_mutex_unlock(&arena->mutex);
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_sparse_arena_get_stats(
    xc3_sparse_arena *arena, xc3_sparse_arena_stats *out) {
    if (arena == NULL || out == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&arena->mutex);
    xc3_sparse_arena_stats stats = {
        .span_bytes = arena->config.span_bytes,
        .commit_limit = arena->config.commit_limit,
        .charged_bytes = arena->charged,
        .lease_count = arena->lease_count,
        .acquisitions = arena->acquisitions,
        .highwater = arena->highwater,
        .active = arena->active,
        .poisoned = arena->poisoned,
    };
    for (arena_range *range = arena->ranges; range != NULL; range = range->next) {
        ++stats.ranges;
        stats.reserved_bytes += range->reserved;
        if (range->faulted) {
            ++stats.faulted_ranges;
            stats.uncertain_bytes += range->committed;
        } else {
            stats.committed_bytes += range->committed;
        }
        if (range->retired) {
            ++stats.pending_ranges;
            stats.pending_reserved_bytes += range->reserved;
            if (!range->faulted)
                stats.pending_committed_bytes += range->committed;
        }
    }
    *out = stats;
    pthread_mutex_unlock(&arena->mutex);
    return XC3_ALLOCATOR_OK;
}

static xc3_allocator_status reserve_lease_slot(xc3_sparse_arena *arena, size_t *out) {
    for (size_t index = 0; index < arena->lease_capacity; ++index) {
        if (!arena->leases[index].active && arena->leases[index].generation != UINT64_MAX) {
            *out = index;
            return XC3_ALLOCATOR_OK;
        }
    }
    size_t previous = arena->lease_capacity;
    size_t next = previous == 0 ? 16 : previous * 2;
    if (next < previous || next > SIZE_MAX / sizeof(lease_slot))
        return XC3_ALLOCATOR_LIMIT_REACHED;
    lease_slot *slots = realloc(arena->leases, next * sizeof(*slots));
    if (slots == NULL)
        return XC3_ALLOCATOR_OUT_OF_MEMORY;
    memset(slots + previous, 0, (next - previous) * sizeof(*slots));
    arena->leases = slots;
    arena->lease_capacity = next;
    *out = previous;
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_sparse_arena_acquire_lease(
    xc3_sparse_arena *arena, uintptr_t base, size_t bytes,
    xc3_sparse_lease *out) {
    if (arena == NULL || out == NULL || bytes == 0 || bytes > UINTPTR_MAX - base)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&arena->mutex);
    xc3_allocator_status status = XC3_ALLOCATOR_OK;
    if (!arena->active)
        status = XC3_ALLOCATOR_SHUT_DOWN;
    else if (arena->poisoned)
        status = XC3_ALLOCATOR_BACKEND_INVALID;
    else if (base % arena->config.page_size != 0 || bytes % arena->config.page_size != 0)
        status = XC3_ALLOCATOR_INVALID_ARGUMENT;
    arena_range *range = NULL;
    if (status == XC3_ALLOCATOR_OK) {
        range = find_range(arena, base, NULL);
        if (range == NULL || range->retired)
            status = XC3_ALLOCATOR_NOT_FOUND;
        else if (range->faulted)
            status = XC3_ALLOCATOR_BACKEND_INVALID;
        else if (base - range->base >= range->committed ||
                 bytes > range->committed - (base - range->base))
            status = XC3_ALLOCATOR_INVALID_ARGUMENT;
        else if (range->leases == UINT64_MAX || arena->lease_count == UINT64_MAX)
            status = XC3_ALLOCATOR_LIMIT_REACHED;
    }
    size_t index = 0;
    if (status == XC3_ALLOCATOR_OK)
        status = reserve_lease_slot(arena, &index);
    if (status == XC3_ALLOCATOR_OK) {
        lease_slot *slot = &arena->leases[index];
        ++slot->generation;
        slot->active = true;
        slot->range = range;
        ++range->leases;
        ++arena->lease_count;
        *out = (xc3_sparse_lease){index + 1u, slot->generation};
    }
    pthread_mutex_unlock(&arena->mutex);
    return status;
}

xc3_allocator_status xc3_sparse_arena_release_lease(
    xc3_sparse_arena *arena, xc3_sparse_lease lease) {
    if (arena == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&arena->mutex);
    if (lease.slot == 0 || lease.slot > arena->lease_capacity || lease.generation == 0) {
        pthread_mutex_unlock(&arena->mutex);
        return XC3_ALLOCATOR_NOT_FOUND;
    }
    lease_slot *slot = &arena->leases[lease.slot - 1u];
    if (!slot->active || slot->generation != lease.generation) {
        pthread_mutex_unlock(&arena->mutex);
        return XC3_ALLOCATOR_NOT_FOUND;
    }
    arena_range *range = slot->range;
    if (range->leases == 1 && range->retired) {
        arena_range *previous = NULL;
        (void)find_range(arena, range->base, &previous);
        xc3_allocator_status status = release_owned(arena, range, previous);
        if (status != XC3_ALLOCATOR_OK) {
            pthread_mutex_unlock(&arena->mutex);
            return status;
        }
    } else {
        --range->leases;
    }
    --arena->lease_count;
    slot->active = false;
    slot->range = NULL;
    pthread_mutex_unlock(&arena->mutex);
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_sparse_arena_shutdown(xc3_sparse_arena *arena) {
    if (arena == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&arena->mutex);
    if (arena->lease_count != 0) {
        pthread_mutex_unlock(&arena->mutex);
        return XC3_ALLOCATOR_BUSY;
    }
    while (arena->ranges != NULL) {
        xc3_allocator_status status = release_owned(arena, arena->ranges, NULL);
        if (status != XC3_ALLOCATOR_OK) {
            pthread_mutex_unlock(&arena->mutex);
            return status;
        }
    }
    arena->active = false;
    pthread_mutex_unlock(&arena->mutex);
    return XC3_ALLOCATOR_OK;
}

xc3_allocator_status xc3_sparse_arena_destroy(xc3_sparse_arena *arena) {
    if (arena == NULL)
        return XC3_ALLOCATOR_INVALID_ARGUMENT;
    pthread_mutex_lock(&arena->mutex);
    bool busy = arena->active || arena->ranges != NULL || arena->lease_count != 0;
    pthread_mutex_unlock(&arena->mutex);
    if (busy)
        return XC3_ALLOCATOR_BUSY;
    pthread_mutex_destroy(&arena->mutex);
    free(arena->leases);
    free(arena);
    return XC3_ALLOCATOR_OK;
}
