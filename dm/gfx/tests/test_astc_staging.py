"""Check production ASTC upload placement using CPU-only buffer mocks."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <objc/runtime.h>
#include "tex/nvn_astc_cache.h"

static unsigned checks, failures, deswizzles, cold_calls, restore_reads;
static NSMutableArray<NSData *> *captured_sources;
#define CHECK(value) do { checks++; if (!(value)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)
#define MLOG(...) do {} while (0)
#define MDIE(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)
#define UP_TIME(field, statement) do { statement; } while (0)
enum { UP_COMPRESSED_CHUNK = 4u << 20, NVN_TEXTURE_FLAGS_IMAGE = 4,
    NVN_TEXTURE_FLAGS_COMPRESSIBLE = 8, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS = 1,
    NVN_MEMORY_POOL_FLAGS_VIRTUAL = 2 };
typedef struct { unsigned flags; } TestPool;
typedef struct { uint32_t width, height, depth, levels, format, flags, samples; } TestDesc;
typedef struct {
    TestDesc d;
    uint64_t serial;
    const uint8_t *mem;
    const TestPool *pool;
    int has_layout;
    struct { size_t size; } L;
} tex_t;
typedef struct { unsigned block_bytes; } nvn_bc_format_info;
static _Atomic uint64_t g_cur_frame;
static uint64_t g_sub_serial = 1;
static int on_sub_thread(void);
static void buffer_cache_use(id<MTLBuffer> buffer) { (void)buffer; }
@interface NvPkg : NSObject {
@public
    int sparse;
    size_t astc_key_count;
    nvn_astc_cache_key *astc_keys;
}
@end
@implementation NvPkg
- (void)dealloc { free(astc_keys); }
@end
@interface MockBuffer : NSObject
@property NSMutableData *storage;
@property NSUInteger payloadBytes;
@property(readonly) void *contents;
@property(readonly) NSUInteger length;
@end
@implementation MockBuffer
- (void *)contents { return self.storage.mutableBytes; }
- (NSUInteger)length { return self.storage.length; }
@end
@interface MockTexture : NSObject
@property NSUInteger width, height, depth, arrayLength, mipmapLevelCount;
@property MTLTextureType textureType;
@property MTLPixelFormat pixelFormat;
@property NSString *label;
@property NSMutableDictionary<NSString *, NSMutableData *> *planes;
@end
@implementation MockTexture @end
typedef struct {
    __unsafe_unretained id<MTLBuffer> src;
    size_t off, row, image;
    MTLSize size;
    MTLOrigin origin;
    __unsafe_unretained id<MTLTexture> dst;
    NSUInteger slice, level;
} up_blit;
static up_blit *g_up_blits;
static int g_nup_blits, g_up_blitcap;
static NSMutableArray *g_up_texs, *live_buffers;
static unsigned g_nvn_bc_astc = 4;
static int g_nvn_astc_source_cold = 1, shared_enabled = 1, cache_hit = 1, encode_hit = 1;
static _Atomic uint64_t g_astc_direct_hits, g_astc_hits, g_astc_misses, g_astc_bytes, g_astc_cold_bytes;
static _Atomic uint64_t g_astc_staging_direct_bytes, g_astc_staging_copied_bytes;
static uint64_t g_bp_up_bytes;
static int up_shared_enabled(void) { return shared_enabled; }
static id<MTLBuffer> up_shared_alloc(size_t bytes, size_t *off) {
    CHECK(bytes > 0 && bytes <= UP_COMPRESSED_CHUNK);
    MockBuffer *buffer = [MockBuffer new];
    buffer.storage = [NSMutableData dataWithLength:bytes + 512];
    buffer.payloadBytes = bytes;
    memset(buffer.contents, 0xA5, buffer.length);
    [live_buffers addObject:buffer]; *off = 256;
    return (id)buffer;
}
static void up_blit_add(id<MTLBuffer> src, size_t off, size_t row, size_t image, MTLSize size,
                         id<MTLTexture> dst, NSUInteger slice, NSUInteger level) {
    if (g_nup_blits == g_up_blitcap) {
        g_up_blitcap = g_up_blitcap ? g_up_blitcap * 2 : 64;
        g_up_blits = realloc(g_up_blits, g_up_blitcap * sizeof *g_up_blits);
    }
    g_up_blits[g_nup_blits++] = (up_blit){src, off, row, image, size, MTLOriginMake(0, 0, 0), dst, slice, level};
}
static NSString *plane_key(NSUInteger level, NSUInteger layer, NSUInteger z) {
    return [NSString stringWithFormat:@"%lu/%lu/%lu", (unsigned long)level, (unsigned long)layer, (unsigned long)z];
}
static void up_compressed_chunk_end(void) {}
static void up_compressed_texture_end(void) {
    // Delay all reads until every CPU write is complete. The separate upload
    // batch test exercises the real allocator and completion-based reuse.
    for (int i = 0; i < g_nup_blits; i++) {
        up_blit *x = &g_up_blits[i]; MockTexture *mt = (id)x->dst;
        size_t w = MAX(mt.width >> x->level, 1u), h = MAX(mt.height >> x->level, 1u);
        unsigned block = g_nvn_bc_astc;
        size_t pitch = (w + block - 1) / block * 16, rows = (h + block - 1) / block;
        NSString *key = plane_key(x->level, x->slice, x->origin.z);
        NSMutableData *plane = mt.planes[key];
        if (!plane) mt.planes[key] = plane = [NSMutableData dataWithLength:pitch * rows];
        size_t count = (x->size.height + block - 1) / block, first = x->origin.y / block;
        CHECK(x->origin.y % block == 0);
        CHECK(x->off == 256 && x->size.width == w && x->size.depth == 1 && !x->origin.x);
        CHECK(first <= rows && count <= rows - first && x->row >= pitch);
        CHECK(x->off <= x->src.length && x->image <= x->src.length - x->off);
        CHECK(x->image == x->row * count);
        for (size_t r = 0; r < count; r++)
            memcpy((uint8_t *)plane.mutableBytes + (first + r) * pitch,
                   (uint8_t *)x->src.contents + x->off + r * x->row, pitch);
    }
    for (MockBuffer *buffer in live_buffers) {
        uint8_t *p = buffer.contents;
        int intact = 1;
        for (size_t i = 0; i < 256; i++) intact &= p[i] == 0xA5 && p[256 + buffer.payloadBytes + i] == 0xA5;
        CHECK(intact);
    }
    g_nup_blits = 0; [g_up_texs removeAllObjects]; [live_buffers removeAllObjects];
}
static int nvn_sparse_pool(const void *pool) { return pool != NULL; }
static void nvn_sparse_copy(const void *pool, void *dst, uint64_t src, size_t size) {
    CHECK(pool != NULL); memcpy(dst, (const void *)(uintptr_t)src, size);
}
static int pkg_cache_initial_reloadable(const tex_t *t, int sparse) { (void)t; return !sparse; }
static void pkg_astc_keys_prepare(NvPkg *pk, size_t count) {
    if (!pk->astc_keys) { pk->astc_keys = calloc(count, sizeof *pk->astc_keys); pk->astc_key_count = count; }
}
static uint8_t seed_for(unsigned level, unsigned layer, unsigned z) { return (uint8_t)(17 + level * 7 + layer * 13 + z * 23); }
static void payload(void *output, size_t size, uint8_t seed) {
    for (size_t i = 0; i < size; i++) ((uint8_t *)output)[i] = (uint8_t)(seed + i * 11);
}
static int nvn_deswizzle(const TestDesc *d, int level, int layer, const void *source, void *output, size_t pitch) {
    CHECK(source != NULL); deswizzles++;
    size_t h = MAX(d->height >> level, 1u), depth = MAX(d->depth >> level, 1u);
    size_t plane = pitch * ((h + 3) / 4);
    for (size_t z = 0; z < depth; z++) memset((uint8_t *)output + z * plane, seed_for(level, layer, (unsigned)z), plane);
    return 0;
}
int nvn_bc_astc_cache_read(const nvn_astc_cache_key *key, void *output, size_t size,
                                  const char *cache, const char *readonly_cache) {
    (void)cache; (void)readonly_cache; restore_reads++;
    if (cache_hit) payload(output, size, (uint8_t)key->hex[0]);
    return cache_hit;
}
int nvn_bc_astc_encode_report(uint32_t format, const void *source, size_t source_size,
        uint32_t width, uint32_t height, unsigned block, void *output, size_t output_size,
        const char *cache, const char *readonly_cache, int *hit, nvn_astc_cache_key *key,
        nvn_astc_cache_report *report) {
    (void)format; (void)cache; (void)readonly_cache;
    CHECK(block == g_nvn_bc_astc && source_size == (size_t)((width + 3) / 4) * ((height + 3) / 4) * 8);
    CHECK(output_size == (size_t)((width + block - 1) / block) * ((height + block - 1) / block) * 16);
    if (captured_sources) [captured_sources addObject:[NSData dataWithBytes:source length:source_size]];
    uint8_t seed = *(const uint8_t *)source;
    payload(output, output_size, seed); *hit = encode_hit;
    if (key) key->hex[0] = seed;
    memset(report, 0, sizeof *report);
    return 0;
}
const char *nvn_bc_astc_cache_status_name(nvn_astc_cache_status status) { (void)status; return "mock"; }
int nvn_bc_astc_format(uint32_t format, unsigned block, uint32_t *pixel_format) {
    (void)block;
    if (pixel_format) *pixel_format = MTLPixelFormatASTC_4x4_LDR;
    return format == 66;
}
static int nvn_bc_format(uint32_t format, nvn_bc_format_info *info) {
    if (info) info->block_bytes = 8;
    return format == 66;
}
static int g_nvn_bc_decode = 1;
static size_t dm_guest_memory_cold(const void *memory, size_t size) { CHECK(memory != NULL); cold_calls++; return size; }
'''


BLOCK_MOCK = r"""
static NvBcBlocks *bc_blocks_ensure(const tex_t *t, id<MTLTexture> texture) {
    (void)t;
    NvBcBlocks *blocks = bc_blocks_get(texture);
    if (blocks) return blocks;
    blocks = [NvBcBlocks new]; blocks->format = 66;
    blocks->levels = texture.mipmapLevelCount;
    blocks->layers = texture.textureType == MTLTextureType3D ? 1 : texture.arrayLength *
        (texture.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    size_t size = 0;
    for (NSUInteger level = 0; level < blocks->levels; level++) {
        blocks->offsets[level] = size;
        blocks->rows[level] = ((MAX(texture.width >> level, 1u) + 3) / 4) * 8;
        blocks->planes[level] = blocks->rows[level] * ((MAX(texture.height >> level, 1u) + 3) / 4);
        blocks->depths[level] = texture.textureType == MTLTextureType3D ? MAX(texture.depth >> level, 1u) : 1;
        size += blocks->planes[level] * blocks->depths[level] * blocks->layers;
    }
    MockBuffer *buffer = [MockBuffer new]; buffer.storage = [NSMutableData dataWithLength:size];
    blocks->buffer = (id)buffer;
    for (NSUInteger level = 0; level < blocks->levels; level++)
        for (NSUInteger layer = 0; layer < blocks->layers; layer++)
            for (NSUInteger z = 0; z < blocks->depths[level]; z++)
                memset((uint8_t *)buffer.contents + bc_blocks_offset(blocks, level, layer, z),
                    seed_for((unsigned)level, (unsigned)layer, (unsigned)z), blocks->planes[level]);
    objc_setAssociatedObject(texture, &g_bc_blocks_key, blocks, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return blocks;
}
"""

DEFERRED_TEST = r"""
static void run_deferred(unsigned depth, unsigned layers, MTLTextureType type, int overwrite_all) {
    MockTexture *mt = [MockTexture new];
    mt.width = 96; mt.height = 65; mt.depth = depth; mt.mipmapLevelCount = 7;
    mt.arrayLength = layers; mt.textureType = type; mt.pixelFormat = MTLPixelFormatASTC_4x4_LDR;
    mt.planes = [NSMutableDictionary new];
    uint8_t memory[256]; memset(memory, 0xE7, sizeof memory);
    tex_t t = {.d = {96, 65, type == MTLTextureType3D ? depth : 1, 7, 66, NVN_TEXTURE_FLAGS_IMAGE, 1},
        .mem = memory, .L.size = sizeof memory};
    captured_sources = [NSMutableArray new];
    uint64_t before_deferred = g_bc_initial_deferred, before_overwritten = g_bc_initial_overwritten;
    uint64_t before_read = g_bc_initial_read;
    CHECK(bc_blocks_defer_initial(&t, (id)mt));
    NvBcBlocks *blocks = bc_blocks_get((id)mt);
    CHECK(blocks != nil && blocks->initial_planes.count > 0 && !captured_sources.count && !mt.planes.count);
    NSUInteger total = blocks->initial_planes.count, overwritten = 0;
    CHECK(g_bc_initial_deferred - before_deferred == total);
    NSMutableArray<NSData *> *expected_sources = [NSMutableArray new];
    NSMutableDictionary *expected_outputs = [NSMutableDictionary new];
    unsigned index = 0;
    for (NSUInteger level = 0; level < blocks->levels; level++) {
        size_t w = MAX(mt.width >> level, 1u), h = MAX(mt.height >> level, 1u);
        size_t bytes = ((w + g_nvn_bc_astc - 1) / g_nvn_bc_astc) * ((h + g_nvn_bc_astc - 1) / g_nvn_bc_astc) * 16;
        for (NSUInteger layer = 0; layer < blocks->layers; layer++) for (NSUInteger z = 0; z < blocks->depths[level]; z++) {
            NSUInteger offset = bc_blocks_offset(blocks, level, layer, z);
            uint8_t *source = (uint8_t *)blocks->buffer.contents + offset;
            NSString *key = plane_key(level, layer, z);
            NSMutableData *output = [NSMutableData dataWithLength:bytes];
            if (overwrite_all || index++ % 2 == 0) {
                // Model the sampling output already produced by a GPU copy.
                // Deferred initial uploads must never replace that output.
                memset(output.mutableBytes, 0x71, bytes); mt.planes[key] = output;
                bc_blocks_initial_done(blocks, offset, 1); overwritten++;
                bc_blocks_initial_done(blocks, offset, 1);
            } else {
                // The retained source is independent of subsequent pool reuse.
                // Mixed block bytes also detect a partially lost source plane.
                source[blocks->planes[level] - 1] ^= 0x5A;
                [expected_sources addObject:[NSData dataWithBytes:source length:blocks->planes[level]]];
                payload(output.mutableBytes, bytes, source[0]);
            }
            expected_outputs[key] = output;
        }
    }
    blocks->gpu_written = 1; blocks->revision = 17;
    unsigned old_deswizzles = deswizzles, old_cold = cold_calls;
    bc_blocks_initial_sample(&t, (id)mt);
    CHECK(!blocks->initial_planes && blocks->gpu_written && blocks->revision == 17);
    CHECK(deswizzles == old_deswizzles && cold_calls == old_cold);
    CHECK(g_bc_initial_overwritten - before_overwritten == overwritten);
    CHECK(g_bc_initial_read - before_read == total - overwritten);
    CHECK([captured_sources isEqualToArray:expected_sources]);
    CHECK([mt.planes isEqualToDictionary:expected_outputs]);
    NSUInteger calls = captured_sources.count;
    bc_blocks_initial_sample(&t, (id)mt);
    CHECK(captured_sources.count == calls);
    captured_sources = nil;
    mt.pixelFormat = MTLPixelFormatRGBA8Unorm;
    CHECK(!bc_blocks_defer_initial(&t, (id)mt));
}
"""


INITIAL_MOCK = r"""
static _Atomic int g_sub_thread_set = 1;
static int submitting = 1;
static unsigned immediate_uploads;
static int on_sub_thread(void) { return submitting; }
static id<MTLTexture> pkg_upload(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse) {
    (void)t; (void)stamp; immediate_uploads++; return reuse;
}
"""
INITIAL_TEST = r"""
static void run_initial_eligibility(void) {
    MockTexture *mt = [MockTexture new];
    mt.width = mt.height = 64; mt.depth = mt.mipmapLevelCount = mt.arrayLength = 1;
    mt.textureType = MTLTextureType2D; mt.pixelFormat = MTLPixelFormatASTC_4x4_LDR;
    TestPool pool = {NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS};
    uint8_t memory[32] = {1};
    tex_t t = {.d = {64, 64, 1, 1, 66, NVN_TEXTURE_FLAGS_IMAGE, 1},
        .mem = memory, .pool = &pool, .L.size = sizeof memory};
    unsigned before = immediate_uploads;
    CHECK(tex_init_fill(&t, (id)mt, 0) && immediate_uploads == before + 1);
    CHECK(tex_init_fill(&t, (id)mt, 1) && immediate_uploads == before + 1);
    t.has_layout = 1; CHECK(!tex_init_fill(&t, (id)mt, 1)); t.has_layout = 0;
    t.d.samples = 4; CHECK(!tex_init_fill(&t, (id)mt, 1)); t.d.samples = 1;
    t.d.flags |= NVN_TEXTURE_FLAGS_COMPRESSIBLE; CHECK(!tex_init_fill(&t, (id)mt, 1)); t.d.flags = NVN_TEXTURE_FLAGS_IMAGE;
    pool.flags |= NVN_MEMORY_POOL_FLAGS_VIRTUAL; CHECK(!tex_init_fill(&t, (id)mt, 1)); pool.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS;
    pool.flags = 0; CHECK(!tex_init_fill(&t, (id)mt, 1)); pool.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS;
    submitting = 0; CHECK(!tex_init_fill(&t, (id)mt, 1)); submitting = 1;
    g_sub_thread_set = 0; CHECK(!tex_init_fill(&t, (id)mt, 1)); g_sub_thread_set = 1;
    mt.pixelFormat = MTLPixelFormatRGBA8Unorm;
    CHECK(tex_init_fill(&t, (id)mt, 1) && immediate_uploads == before + 2);
    memset(memory, 0, sizeof memory);
    CHECK(!tex_init_fill(&t, (id)mt, 1));
    mt.pixelFormat = MTLPixelFormatASTC_4x4_LDR;
    CHECK(tex_init_fill(&t, (id)mt, 1) && immediate_uploads == before + 2);
}
"""

def extract_function(source, name):
    start = source.index("static ", source.rfind("\n", 0, source.index(name + "(")))
    opening = source.index("{", start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (source[cursor] == "{") - (source[cursor] == "}")
        cursor += 1
    return source[start:cursor] + "\n"

TEST = r'''
static void run_shape(unsigned width, unsigned height, unsigned depth, unsigned levels,
                      unsigned layers, MTLTextureType type, unsigned flags, int sparse, int one_shot) {
    MockTexture *mt = [MockTexture new];
    mt.width = width; mt.height = height; mt.depth = depth; mt.mipmapLevelCount = levels;
    mt.arrayLength = layers; mt.textureType = type; mt.planes = [NSMutableDictionary new];
    uint8_t memory[256] = {0};
    tex_t t = {.d = {width, height, type == MTLTextureType3D ? depth : 1, levels, 66, flags, 1},
               .mem = memory, .pool = sparse ? (TestPool *)memory : NULL, .L.size = sizeof memory};
    NvPkg *pk = one_shot ? nil : [NvPkg new]; if (pk) pk->sparse = sparse;
    unsigned slices = type == MTLTextureType3D ? 1 : layers * (type == MTLTextureTypeCubeArray ? 6 : 1);
    uint64_t expected_direct = 0, expected_copy = 0;
    unsigned block = g_nvn_bc_astc;
    for (unsigned l = 0; l < levels; l++) {
        size_t w = MAX(width >> l, 1u), h = MAX(height >> l, 1u);
        size_t pitch = (w + block - 1) / block * 16, plane = pitch * ((h + block - 1) / block);
        unsigned nz = type == MTLTextureType3D ? MAX(depth >> l, 1u) : 1;
        if (pitch % 256 == 0 && plane <= UP_COMPRESSED_CHUNK && shared_enabled && astc_direct_staging()) expected_direct += plane * slices * nz;
        else expected_copy += plane * slices * nz;
    }
    for (int pass = 0; pass < 3; pass++) {
        [mt.planes removeAllObjects];
        cache_hit = pass != 2; encode_hit = pass != 2;
        unsigned before_reads = deswizzles, before_cold = cold_calls;
        uint64_t before_direct = g_astc_staging_direct_bytes, before_copy = g_astc_staging_copied_bytes;
        CHECK(pkg_upload_astc(&t, (id)mt, (nvn_bc_format_info){8}, pk, pass != 0) == (id)mt);
        CHECK(g_astc_staging_direct_bytes - before_direct == expected_direct);
        CHECK(g_astc_staging_copied_bytes - before_copy == expected_copy);
        if (pass == 1 && pk && !sparse) CHECK(deswizzles == before_reads);
        else CHECK(deswizzles == before_reads + levels * slices);
        int source_read = deswizzles != before_reads;
        CHECK(cold_calls == before_cold + (unsigned)(source_read && pk && !sparse && !(flags & NVN_TEXTURE_FLAGS_IMAGE)));
        for (unsigned l = 0; l < levels; l++) {
            size_t w = MAX(width >> l, 1u), h = MAX(height >> l, 1u);
            size_t plane = (w + block - 1) / block * 16 * ((h + block - 1) / block);
            unsigned nz = type == MTLTextureType3D ? MAX(depth >> l, 1u) : 1;
            uint8_t *expected = malloc(plane);
            for (unsigned layer = 0; layer < slices; layer++) for (unsigned z = 0; z < nz; z++) {
                payload(expected, plane, seed_for(l, layer, z));
                NSData *actual = mt.planes[plane_key(l, layer, z)];
                CHECK(actual.length == plane && !memcmp(actual.bytes, expected, plane));
            }
            free(expected);
        }
    }
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "unset")) unsetenv("DM_NVN_ASTC_DIRECT_STAGING");
    else setenv("DM_NVN_ASTC_DIRECT_STAGING", argv[1], 1);
    shared_enabled = atoi(argv[2]);
    g_up_texs = [NSMutableArray new]; live_buffers = [NSMutableArray new];
    for (unsigned block = 4; block <= 6; block += 2) {
        g_nvn_bc_astc = block;
        run_initial_eligibility();
        run_deferred(1, 1, MTLTextureType2D, 0);
        run_deferred(1, 1, MTLTextureType2D, 1);
        run_deferred(1, 3, MTLTextureType2DArray, 0);
        run_deferred(5, 1, MTLTextureType3D, 0);
        run_deferred(1, 2, MTLTextureTypeCubeArray, 0);
        run_shape(64, 65, 1, 7, 1, MTLTextureType2D, 0, 0, 0);
        run_shape(96, 97, 1, 7, 1, MTLTextureType2D, 0, 0, 0);
        run_shape(61, 35, 1, 6, 1, MTLTextureType2D, 0, 0, 0);
        run_shape(2048, 2048, 1, 12, 1, MTLTextureType2D, 0, 0, 0);
        run_shape(3072, 3072, 1, 1, 1, MTLTextureType2D, 0, 0, 0);
        run_shape(4096, 1031, 1, 3, 1, MTLTextureType2D, 0, 0, 0);
        run_shape(192, 64, 1, 8, 3, MTLTextureType2DArray, 0, 0, 0);
        run_shape(64, 64, 5, 7, 1, MTLTextureType3D, 0, 0, 0);
        run_shape(96, 96, 5, 7, 1, MTLTextureType3D, 0, 0, 0);
        run_shape(64, 64, 1, 7, 2, MTLTextureTypeCubeArray, 0, 0, 0);
        run_shape(64, 64, 1, 7, 1, MTLTextureType2D, NVN_TEXTURE_FLAGS_IMAGE, 0, 0);
        run_shape(64, 64, 1, 7, 1, MTLTextureType2D, 0, 1, 0);
        run_shape(64, 64, 1, 7, 1, MTLTextureType2D, 0, 0, 1);
    }
    CHECK(restore_reads > 0 && g_astc_hits > 0 && g_astc_misses > 0 && !g_nup_blits);
    CHECK(g_astc_bytes == g_bp_up_bytes && g_astc_bytes == g_astc_staging_direct_bytes + g_astc_staging_copied_bytes);
    free(g_up_blits);
    printf("direct=%s shared=%d: %u checks, %u failures; no Metal device created\n", argv[1], shared_enabled, checks, failures);
    return failures != 0;
} }
'''

with tempfile.TemporaryDirectory(prefix="nvn-astc-staging-") as directory:
    root = Path(directory)
    source, binary = root / "staging.m", root / "staging"
    blocks = (ROOT / "nvn/nvn_bc_blocks.inc").read_text()
    block_type = blocks[blocks.index("@interface NvBcBlocks"):blocks.index("@interface NvBcRawView")]
    block_helpers = "static char g_bc_blocks_key;\n" + "".join(
        extract_function(blocks, name) for name in ("bc_blocks_get", "bc_blocks_buffer", "bc_blocks_offset"))
    deferred_helpers = "static uint64_t g_bc_initial_deferred, g_bc_initial_overwritten, g_bc_initial_read;\n" + "".join(
        extract_function(blocks, name) for name in ("bc_blocks_defer_initial", "bc_blocks_initial_done"))
    source.write_text(PREFIX + block_type + block_helpers + BLOCK_MOCK + deferred_helpers +
        (ROOT / "nvn/nvn_astc_upload.inc").read_text() + INITIAL_MOCK +
        "".join(extract_function((ROOT / "nvn/nvn_mtl.m").read_text(), name)
                for name in ("mem_any", "tex_init_fill")) + INITIAL_TEST + DEFERRED_TEST + TEST)
    subprocess.run([
        "clang", "-std=gnu11", "-fobjc-arc", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-I", str(ROOT), "-framework", "Foundation", str(source), "-o", str(binary),
    ], check=True)
    environment = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
    for direct, shared in (("0", "1"), ("1", "1"), ("unset", "1"), ("1", "0")):
        subprocess.run([str(binary), direct, shared], env=environment, check=True)
