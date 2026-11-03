#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../nvn/include/nvn.h"
#include "../tex/nvn_deswizzle.h"

static unsigned checks, failures;
#define CHECK(value) do { checks++; if (!(value)) { failures++; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); } } while (0)

static void compare_regions(nvn_texdesc d) {
    nvn_texlayout layout;
    nvn_tex_layout(&d, &layout);
    uint8_t *storage = calloc(1, layout.size);
    if (!storage) abort();
    for (unsigned level = 0; level < d.levels; level++) {
        uint32_t rb, rows, slices, bpb; uint64_t base; unsigned th, td;
        CHECK(!nvn_deswizzle_geom(&d, level, 0, &rb, &rows, &slices, &base, &th, &td, &bpb));
        size_t size = (size_t)rb * rows * slices;
        uint8_t *linear = malloc(size);
        if (!linear) abort();
        for (unsigned layer = 0; layer < layout.layers; layer++) {
            for (size_t i = 0; i < size; i++) linear[i] = (uint8_t)(i * 37 + (i >> 7) + layer * 11 + level);
            CHECK(!nvn_swizzle(&d, level, layer, linear, rb, storage));
            for (unsigned z = 0; z < slices; z++) for (unsigned sample = 0; sample < 6; sample++) {
                uint32_t x = sample ? (sample * 13u) % rb : 0;
                uint32_t y = sample ? (sample * 7u) % rows : 0;
                uint32_t w = sample ? (rb - x < 39 ? rb - x : 39) : rb;
                uint32_t h = sample ? (rows - y < 9 ? rows - y : 9) : rows;
                size_t pitch = w + 7, bytes = pitch * h + 32;
                uint8_t *output = malloc(bytes), *expected = malloc(bytes);
                if (!output || !expected) abort();
                memset(output, 0xcd, bytes); memset(expected, 0xcd, bytes);
                for (unsigned r = 0; r < h; r++)
                    memcpy(expected + 16 + r * pitch, linear + ((size_t)z * rows + y + r) * rb + x, w);
                nvn_tex_region region = {x, y, z, w, h};
                CHECK(!nvn_deswizzle_region(&d, level, layer, &region, storage, output + 16, pitch));
                CHECK(!memcmp(output, expected, bytes));
                free(output); free(expected);
            }
        }
        free(linear);
    }
    free(storage);
}

static void reject_invalid(void) {
    nvn_texdesc d = {1, NVN_FORMAT_RGBA8, 0, 0, 64, 64, 1, 1, 0, 0};
    nvn_tex_region good = {0, 0, 0, 16, 2};
    uint8_t source[16384] = {0}, destination[64], expected[64];
    memset(destination, 0x5a, sizeof destination); memcpy(expected, destination, sizeof expected);
    nvn_tex_region invalid[] = {
        {256, 0, 0, 1, 1}, {UINT32_MAX, 0, 0, 1, 1}, {0, 64, 0, 1, 1},
        {0, 0, 1, 1, 1}, {0, 0, 0, 0, 1}, {0, 0, 0, 1, 0},
        {255, 0, 0, 2, 1}, {0, 63, 0, 1, 2},
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        CHECK(nvn_deswizzle_region(&d, 0, 0, &invalid[i], source, destination, 32) == -1);
    CHECK(nvn_deswizzle_region(&d, 0, 0, &good, source, destination, 15) == -1);
    CHECK(nvn_deswizzle_region(&d, 0, 0, &good, source, destination, SIZE_MAX) == -1);
    CHECK(nvn_deswizzle_region(&d, 1, 0, &good, source, destination, 32) == -1);
    CHECK(nvn_deswizzle_region(&d, 32, 0, &good, source, destination, 32) == -1);
    CHECK(nvn_deswizzle_region(&d, 0, 1, &good, source, destination, 32) == -1);
    d.flags = NVN_TEXTURE_FLAGS_LINEAR;
    CHECK(nvn_deswizzle_region(&d, 0, 0, &good, source, destination, 32) == -1);
    CHECK(!memcmp(destination, expected, sizeof destination));
}

static void untouched_pages(void) {
    nvn_texdesc d = {1, NVN_FORMAT_R8, 0, 0, 2048, 2048, 1, 1, 0, 0};
    nvn_texlayout layout; nvn_tex_layout(&d, &layout);
    size_t page = (size_t)getpagesize();
    uint8_t *source = mmap(NULL, layout.size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (source == MAP_FAILED || mprotect(source, page, PROT_READ | PROT_WRITE)) abort();
    source[7] = 0xa9;
    nvn_tex_region region = {7, 0, 0, 1, 1};
    uint8_t output = 0;
    CHECK(!nvn_deswizzle_region(&d, 0, 0, &region, source, &output, 1));
    CHECK(output == 0xa9);
    CHECK(!munmap(source, layout.size));
}

int main(void) {
    nvn_texdesc cases[] = {
        {1, NVN_FORMAT_R8, 0, 0, 17, 9, 1, 5, 0, 0},
        {1, NVN_FORMAT_RGBA8, 0, 0, 71, 35, 1, 7, 0, 0},
        {1, NVN_FORMAT_RGBA16F, 0, 0, 19, 13, 1, 5, 0, 0},
        {1, NVN_FORMAT_RGBA32F, 0, 0, 17, 9, 1, 5, 0, 0},
        {1, NVN_FORMAT_RGBA_DXT1, 0, 0, 101, 67, 1, 7, 0, 0},
        {1, NVN_FORMAT_BPTC_UNORM, 0, 0, 101, 67, 1, 7, 0, 0},
        {1, NVN_FORMAT_RGTC1_UNORM, 0, 0, 1024, 1024, 1, 11, 0, 5},
        {4, NVN_FORMAT_RGBA8, 0, 0, 17, 9, 5, 5, 0, 0},
        {8, NVN_FORMAT_RGBA8, 0, 0, 32, 32, 1, 6, 0, 0},
        {2, NVN_FORMAT_RGBA8, 0, 0, 64, 64, 16, 7, 0, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) compare_regions(cases[i]);
    reject_invalid(); untouched_pages();
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0;
}
