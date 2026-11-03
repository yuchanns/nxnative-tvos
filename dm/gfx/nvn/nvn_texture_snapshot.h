#ifndef NVN_TEXTURE_SNAPSHOT_H
#define NVN_TEXTURE_SNAPSHOT_H

#include <stdint.h>
#include <stddef.h>

// Buffer copies use block rows, including the clamped edge of small mip levels.
typedef struct { uint32_t block_width, block_height, bytes, options; } nvn_snapshot_plane;
typedef struct {
    uint32_t width, height, levels, layers, planes;
    nvn_snapshot_plane plane[2];
    uint32_t depth;   // Zero for arrays/cubes; 3D depth shrinks at each mip.
} nvn_snapshot_layout;
typedef struct { uint32_t plane, level, slice, row; uint64_t offset; } nvn_snapshot_cursor;
typedef struct {
    uint32_t level, slice, y, width, height, row_bytes, options;
    uint64_t offset, bytes;
    uint32_t z;
} nvn_snapshot_chunk;

enum { NVN_SNAPSHOT_CHUNK = 4u << 20 };
#define NVN_SNAPSHOT_LIMIT (512ull << 20)

static inline uint32_t nvn_snapshot_dim(uint32_t value, uint32_t level) {
    value >>= level;
    return value ? value : 1;
}
static inline uint32_t nvn_snapshot_row(uint32_t width, nvn_snapshot_plane p) {
    return (((width + p.block_width - 1) / p.block_width * p.bytes) + 255u) & ~255u;
}
static inline uint64_t nvn_snapshot_size(const nvn_snapshot_layout *layout) {
    if (!layout || !layout->width || layout->width > 16384 || !layout->height || layout->height > 16384 ||
        !layout->levels || layout->levels > 15 || !layout->layers || layout->layers > 2048 ||
        !layout->planes || layout->planes > 2 || layout->depth > 2048 || (layout->depth && layout->layers != 1)) return 0;
    uint64_t total = 0;
    for (uint32_t plane = 0; plane < layout->planes; plane++) {
        nvn_snapshot_plane p = layout->plane[plane];
        if (!p.block_width || p.block_width > 12 || !p.block_height || p.block_height > 12 || !p.bytes || p.bytes > 16) return 0;
        for (uint32_t level = 0; level < layout->levels; level++) {
            uint32_t width = nvn_snapshot_dim(layout->width, level), height = nvn_snapshot_dim(layout->height, level);
            uint32_t slices = layout->depth ? nvn_snapshot_dim(layout->depth, level) : layout->layers;
            uint64_t size = (uint64_t)nvn_snapshot_row(width, p) * ((height + p.block_height - 1) / p.block_height) * slices;
            if (size > NVN_SNAPSHOT_LIMIT - total) return 0;
            total += size;
        }
    }
    return total;
}
// The caller validates the layout once with nvn_snapshot_size(). The cursor is
// copied before an asynchronous transfer and published only after its success.
static inline int nvn_snapshot_next(const nvn_snapshot_layout *layout, nvn_snapshot_cursor *cursor, nvn_snapshot_chunk *chunk) {
    if (cursor->plane >= layout->planes) return 0;
    nvn_snapshot_plane p = layout->plane[cursor->plane];
    uint32_t width = nvn_snapshot_dim(layout->width, cursor->level), height = nvn_snapshot_dim(layout->height, cursor->level);
    uint32_t row_bytes = nvn_snapshot_row(width, p), rows = (height + p.block_height - 1) / p.block_height;
    uint32_t count = NVN_SNAPSHOT_CHUNK / row_bytes;
    if (count > rows - cursor->row) count = rows - cursor->row;
    uint32_t y = cursor->row * p.block_height, pixels = count * p.block_height;
    if (pixels > height - y) pixels = height - y;
    *chunk = (nvn_snapshot_chunk){cursor->level, layout->depth ? 0 : cursor->slice, y, width, pixels, row_bytes, p.options,
        cursor->offset, (uint64_t)row_bytes * count, layout->depth ? cursor->slice : 0};
    cursor->offset += chunk->bytes;
    cursor->row += count;
    if (cursor->row == rows) {
        cursor->row = 0;
        uint32_t slices = layout->depth ? nvn_snapshot_dim(layout->depth, cursor->level) : layout->layers;
        if (++cursor->slice == slices) {
            cursor->slice = 0;
            if (++cursor->level == layout->levels) { cursor->level = 0; cursor->plane++; }
        }
    }
    return 1;
}
#endif
