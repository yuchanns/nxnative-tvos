#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nvn_bcdec.h"

typedef struct {
    size_t x_bytes, y_blocks, row_bytes, rows, row_stride, image_stride, span;
    uint32_t z, depth;
} nvn_bc_region;

// NVN compressed copies address texels, but transfer complete encoded blocks.
// A partial block is legal only at the edge of the selected mip level.
static inline int nvn_bc_region_init(nvn_bc_region *out, uint32_t format,
                                     uint32_t width, uint32_t height, uint32_t slices,
                                     int x, int y, int z, int w, int h, int d,
                                     size_t row_stride, size_t image_stride) {
    nvn_bc_format_info info;
    if (!out || !nvn_bc_format(format, &info) || !width || !height || !slices ||
        x < 0 || y < 0 || z < 0 || w <= 0 || h <= 0 || d < 0 || (x & 3) || (y & 3)) return -1;
    uint32_t depth = d ? (uint32_t)d : 1;
    if ((uint32_t)x > width || (uint32_t)w > width - (uint32_t)x ||
        (uint32_t)y > height || (uint32_t)h > height - (uint32_t)y ||
        (uint32_t)z >= slices || depth > slices - (uint32_t)z ||
        ((w & 3) && (uint32_t)w != width - (uint32_t)x) ||
        ((h & 3) && (uint32_t)h != height - (uint32_t)y)) return -1;
    size_t columns = ((size_t)w + 3) / 4, rows = ((size_t)h + 3) / 4;
    if (columns > SIZE_MAX / info.block_bytes) return -1;
    size_t bytes = columns * info.block_bytes;
    size_t row = row_stride ? row_stride : bytes;
    if (row < bytes || row % info.block_bytes || rows > SIZE_MAX / row) return -1;
    size_t image = image_stride ? image_stride : row * rows;
    size_t plane_span = (rows - 1) * row + bytes;
    if (image < plane_span || image % info.block_bytes ||
        (size_t)(depth - 1) > (SIZE_MAX - plane_span) / image) return -1;
    *out = (nvn_bc_region){(size_t)x / 4 * info.block_bytes, (size_t)y / 4,
        bytes, rows, row, image, (size_t)(depth - 1) * image + plane_span, (uint32_t)z, depth};
    return 0;
}
