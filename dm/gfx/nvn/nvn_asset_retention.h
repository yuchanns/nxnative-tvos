#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint64_t used_frame, bytes;
    size_t index;
    int keep;
} nvn_asset_retention_item;

static inline int nvn_asset_retention_recent_first(const void *left, const void *right) {
    const nvn_asset_retention_item *a = left, *b = right;
    if (a->used_frame != b->used_frame) return a->used_frame > b->used_frame ? -1 : 1;
    return a->index < b->index ? -1 : a->index != b->index;
}

// Candidates have already passed GPU completion and reloadability checks. Keep
// the most recent ones that fit; an oversized entry must not block smaller ones.
static inline uint64_t nvn_asset_retention_select(nvn_asset_retention_item *items,
        size_t count, uint64_t frame, uint64_t max_age, uint64_t budget) {
    uint64_t remaining = budget;
    if (count > 1 && budget) qsort(items, count, sizeof *items, nvn_asset_retention_recent_first);
    for (size_t i = 0; i < count; i++) {
        nvn_asset_retention_item *item = &items[i];
        item->keep = item->used_frame <= frame && frame - item->used_frame <= max_age &&
                     item->bytes && item->bytes <= remaining;
        if (item->keep) remaining -= item->bytes;
    }
    return budget - remaining;
}
