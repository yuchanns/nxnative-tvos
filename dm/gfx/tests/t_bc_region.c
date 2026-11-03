// CPU-only compressed-copy bounds and padded-stride tests.
#include "../tex/nvn_bc_region.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #condition); } } while (0)

static void shape(uint32_t format, unsigned width, unsigned height, unsigned depth) {
    nvn_bc_format_info info; CHECK(nvn_bc_format(format, &info));
    size_t full_row = ((size_t)width + 3) / 4 * info.block_bytes;
    size_t full_rows = ((size_t)height + 3) / 4;
    size_t full_plane = full_row * full_rows, total = full_plane * depth;
    unsigned char *source = malloc(total), *destination = malloc(total);
    for (size_t i = 0; i < total; i++) source[i] = (unsigned char)(i * 37 + i / 251);
    for (unsigned y = 0; y < height; y += 4) for (unsigned x = 0; x < width; x += 4) {
        unsigned w = width - x, h = height - y;
        size_t row_bytes = ((size_t)w + 3) / 4 * info.block_bytes;
        size_t rows = ((size_t)h + 3) / 4;
        for (unsigned padded = 0; padded < 2; padded++) {
            size_t row = row_bytes + padded * 3 * info.block_bytes;
            size_t image = row * rows + padded * 2 * info.block_bytes;
            nvn_bc_region region;
            CHECK(!nvn_bc_region_init(&region, format, width, height, depth, x, y, 0, w, h, depth, row, image));
            CHECK(region.span == (depth - 1) * image + (rows - 1) * row + row_bytes);
            unsigned char *packed = malloc(region.span + 16);
            memset(packed, 0xa5, region.span + 16);
            memset(destination, 0x7c, total);
            for (unsigned z = 0; z < region.depth; z++) for (size_t by = 0; by < region.rows; by++) {
                size_t src = z * full_plane + (region.y_blocks + by) * full_row + region.x_bytes;
                size_t dst = z * region.image_stride + by * region.row_stride;
                memcpy(packed + dst, source + src, region.row_bytes);
                memcpy(destination + src, packed + dst, region.row_bytes);
            }
            for (unsigned z = 0; z < depth; z++) for (size_t by = 0; by < full_rows; by++)
                for (size_t bx = 0; bx < full_row; bx++) {
                    size_t i = z * full_plane + by * full_row + bx;
                    CHECK(destination[i] == (by >= y / 4 && bx >= x / 4 * info.block_bytes ? source[i] : 0x7c));
                }
            for (size_t i = region.span; i < region.span + 16; i++) CHECK(packed[i] == 0xa5);
            free(packed);
        }
    }
    free(destination); free(source);
}

int main(void) {
    for (unsigned format = 0x41; format <= 0x50; format++) {
        for (unsigned level = 0; level < 5; level++) {
            unsigned w = 30 >> level, h = 18 >> level;
            shape(format, w ? w : 1, h ? h : 1, level % 3 + 1);
        }
        nvn_bc_format_info info; nvn_bc_format(format, &info);
        nvn_bc_region r;
        CHECK(!nvn_bc_region_init(&r, format, 7, 5, 6, 4, 4, 5, 3, 1, 0, 0, 0));
        CHECK(r.depth == 1 && r.z == 5 && r.row_bytes == info.block_bytes && r.span == info.block_bytes);
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 1, 1, 0, 0, 4, 4, 1, 0, 0));
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 1, 0, 0, 0, 3, 4, 1, 0, 0));
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 1, 4, 0, 0, 8, 4, 1, 0, 0));
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 1, 0, 0, 0, 4, 4, 2, 0, 0));
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 1, -4, 0, 0, 4, 4, 1, 0, 0));
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 1, 0, 0, 0, 4, 4, -1, 0, 0));
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 1, 0, 0, 0, 8, 8, 1, info.block_bytes, 0));
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 1, 0, 0, 0, 8, 8, 1, 0, info.block_bytes));
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 3, 0, 0, 0, 8, 8, 3, 0, SIZE_MAX - 15));
        CHECK(nvn_bc_region_init(&r, format, 8, 8, 1, 0, 0, 0, 8, 8, 1, SIZE_MAX - 15, 0));
    }
    printf("BC copy region: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
