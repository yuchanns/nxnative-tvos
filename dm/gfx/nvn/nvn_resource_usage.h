#pragma once
#include <stdint.h>
#include <string.h>

// Resource declarations are monotonic within one encoder, independently for
// vertex, fragment and compute. A full table falls back to declaring again.
enum { NVN_USAGE_SLOTS = 1024, NVN_USAGE_PROBES = 16 };
typedef struct { uintptr_t resource; uint64_t epoch; uint32_t mask; } nvn_usage_entry;
typedef struct { nvn_usage_entry entries[NVN_USAGE_SLOTS]; uint64_t epoch; } nvn_resource_usage;

static inline void nvn_resource_usage_reset(nvn_resource_usage *cache) {
    if (++cache->epoch == 0) {
        memset(cache->entries, 0, sizeof cache->entries);
        cache->epoch = 1;
    }
}

static inline int nvn_resource_usage_require(nvn_resource_usage *cache, uintptr_t resource, unsigned usage, unsigned stage) {
    if (!resource || stage > 2 || (usage & ~7u)) return 1;
    uint32_t mask = usage << (3 * stage);
    unsigned slot = (unsigned)(((uint64_t)(resource >> 4) * UINT64_C(11400714819323198485)) >> 54);
    for (unsigned i = 0; i < NVN_USAGE_PROBES; i++, slot = (slot + 1) & (NVN_USAGE_SLOTS - 1)) {
        nvn_usage_entry *entry = &cache->entries[slot];
        if (entry->epoch != cache->epoch) {
            *entry = (nvn_usage_entry){resource, cache->epoch, mask};
            return 1;
        }
        if (entry->resource != resource) continue;
        if ((entry->mask & mask) == mask) return 0;
        entry->mask |= mask;
        return 1;
    }
    return 1;
}
