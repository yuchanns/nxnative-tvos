// Decoded BC textures keep encoded storage only when GPU block writes need it.
// Associate storage with the Metal allocation so aliases share it and deferred
// texture retirement also retires its blocks.
@interface NvBcBlocks : NSObject {
@public
    id<MTLBuffer> buffer;
    NSUInteger offsets[15], rows[15], planes[15], depths[15], levels, layers;
    uint32_t format;
    int gpu_written;
    uint64_t revision, serial, buffer_use_submission;
    NSMutableIndexSet *initial_planes;
}
@end
@implementation NvBcBlocks @end
@interface NvBcRawView : NSObject {
@public
    id<MTLTexture> base;
    __weak id<MTLTexture> raw;
    NvBcBlocks *blocks;
    NSUInteger level, layer, level_count, layer_count;
    uint64_t revision;
}
@end
@implementation NvBcRawView @end
static char g_bc_blocks_key;
static char g_bc_raw_key;
static NSMutableArray<id<MTLTexture>> *g_bc_raw_pending;
static _Atomic int g_bc_raw_descriptors;
static void bc_raw_flush(void);
static id<MTLTexture> bc_raw_view(const tex_t *texture, id<MTLTexture> base,
                                 const view_t *view, const uint8_t swizzle[4]);
static void bc_raw_note(id<MTLTexture> texture) {
    NvBcRawView *raw = objc_getAssociatedObject(texture, &g_bc_raw_key);
    if (!raw && texture.parentTexture) raw = objc_getAssociatedObject(texture.parentTexture, &g_bc_raw_key);
    if (!raw) return;
    pkg_cache_use(raw->base, 1);
    if (!g_bc_raw_pending) g_bc_raw_pending = [NSMutableArray new];
    if (raw->raw && ![g_bc_raw_pending containsObject:raw->raw]) [g_bc_raw_pending addObject:raw->raw];
}
static NvBcBlocks *bc_blocks_get(id<MTLTexture> texture) {
    return objc_getAssociatedObject(texture, &g_bc_blocks_key);
}
static id<MTLBuffer> bc_blocks_buffer(NvBcBlocks *blocks) {
    if (!blocks) return nil;
    // Row-wise copies reuse the same reservation; trimming only runs before a
    // submission, so one use covers all accesses in that submission.
    if (!on_sub_thread() || blocks->buffer_use_submission != g_sub_serial) {
        buffer_cache_use(blocks->buffer);
        if (on_sub_thread()) blocks->buffer_use_submission = g_sub_serial;
    }
    return blocks->buffer;
}
static void bc_blocks_share(id<MTLTexture> source, id<MTLTexture> destination) {
    NvBcBlocks *blocks = bc_blocks_get(source);
    if (blocks) objc_setAssociatedObject(destination, &g_bc_blocks_key, blocks, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static NSUInteger bc_blocks_offset(NvBcBlocks *blocks, NSUInteger level, NSUInteger layer, NSUInteger z) {
    if (level >= blocks->levels || layer >= blocks->layers || z >= blocks->depths[level])
        MDIE("BC block subresource out of bounds: level %lu layer %lu depth %lu", (unsigned long)level, (unsigned long)layer, (unsigned long)z);
    return blocks->offsets[level] + (layer * blocks->depths[level] + z) * blocks->planes[level];
}
static int tex_cpu_written(uint64_t serial);
static NvBcBlocks *bc_blocks_ensure(const tex_t *t, id<MTLTexture> texture) {
    NvBcBlocks *blocks = bc_blocks_get(texture);
    if (blocks) return blocks;
    nvn_bc_format_info info;
    if (!texture || !nvn_bc_format(t->d.format, &info) || texture.mipmapLevelCount > 15)
        MDIE("invalid decoded BC texture storage");
    blocks = [NvBcBlocks new]; blocks->format = t->d.format; blocks->serial = t->serial;
    blocks->levels = texture.mipmapLevelCount;
    blocks->layers = texture.textureType == MTLTextureType3D ? 1 : texture.arrayLength *
        (texture.textureType == MTLTextureTypeCube || texture.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    NSUInteger size = 0;
    for (NSUInteger level = 0; level < blocks->levels; level++) {
        NSUInteger w = MAX(texture.width >> level, 1u), h = MAX(texture.height >> level, 1u);
        blocks->offsets[level] = size;
        blocks->rows[level] = ((w + 3) / 4) * info.block_bytes;
        blocks->planes[level] = blocks->rows[level] * ((h + 3) / 4);
        blocks->depths[level] = texture.textureType == MTLTextureType3D ? MAX(texture.depth >> level, 1u) : 1;
        size += blocks->planes[level] * blocks->depths[level] * blocks->layers;
    }
    blocks->buffer = [g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
    if (!blocks->buffer) MDIE("cannot allocate %lu bytes of encoded BC storage", (unsigned long)size);
    memset(blocks->buffer.contents, 0, size);
    // Match the initial upload. GPU-only textures without initial pool content
    // start from zero; their later writes never consult stale guest memory.
    int sparse = t->pool && nvn_sparse_pool(t->pool);
    int uploaded = t->has_layout || tex_cpu_written(t->serial) || sparse ||
        (t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) && !(t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE));
    if (t->mem && uploaded) {
        const uint8_t *source = t->mem; uint8_t *gathered = NULL;
        if (sparse) {
            gathered = malloc(t->L.size ? t->L.size : 1);
            if (!gathered) MDIE("cannot gather encoded BC storage");
            nvn_sparse_copy(t->pool, gathered, (uint64_t)(uintptr_t)t->mem, t->L.size); source = gathered;
        }
        for (NSUInteger level = 0; level < blocks->levels; level++) for (NSUInteger layer = 0; layer < blocks->layers; layer++) {
            uint8_t *destination = (uint8_t *)blocks->buffer.contents + bc_blocks_offset(blocks, level, layer, 0);
            if (nvn_deswizzle(&t->d, (int)level, (int)layer, source, destination, blocks->rows[level]))
                MDIE("cannot initialize encoded BC storage");
        }
        free(gathered);
    }
    // Every block access binds the buffer directly. Metal retains/tracks it;
    // there is no indirect pointer requiring residency-set registration.
    // In particular, deallocation must not reenter the residency-set mutex.
    objc_setAssociatedObject(texture, &g_bc_blocks_key, blocks, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    buffer_cache_track(blocks->buffer);
    if (on_sub_thread()) blocks->buffer_use_submission = g_sub_serial;
    return blocks;
}

static uint64_t g_bc_initial_deferred, g_bc_initial_overwritten, g_bc_initial_read;
static uint64_t g_bc_astc_copy_hits, g_bc_astc_copy_misses, g_bc_astc_copy_ns, g_bc_astc_copy_max_ns;
static int bc_blocks_defer_initial(const tex_t *t, id<MTLTexture> texture) {
    uint32_t format;
    if (!g_nvn_bc_decode || !nvn_bc_astc_format(t->d.format, g_nvn_bc_astc, &format) ||
        texture.pixelFormat != (MTLPixelFormat)format) return 0;
    // A copy destination needs its encoded blocks for partial writes, but its
    // old sampling representation is unnecessary until a reader uses it.
    NvBcBlocks *blocks = bc_blocks_ensure(t, texture);
    blocks->initial_planes = [NSMutableIndexSet indexSet];
    for (NSUInteger level = 0; level < blocks->levels; level++)
        for (NSUInteger layer = 0; layer < blocks->layers; layer++)
            for (NSUInteger z = 0; z < blocks->depths[level]; z++) {
                [blocks->initial_planes addIndex:bc_blocks_offset(blocks, level, layer, z)];
                g_bc_initial_deferred++;
            }
    return 1;
}
static void bc_blocks_initial_done(NvBcBlocks *blocks, NSUInteger offset, int overwritten) {
    if (![blocks->initial_planes containsIndex:offset]) return;
    [blocks->initial_planes removeIndex:offset];
    if (!blocks->initial_planes.count) blocks->initial_planes = nil;
    if (overwritten) g_bc_initial_overwritten++; else g_bc_initial_read++;
}

static void bc_blocks_materialize(id<MTLTexture> texture, NvBcBlocks *blocks,
                                  NSUInteger level, NSUInteger layer, NSUInteger z);

// Direct buffer bindings also work on Tier 1 GPUs. Decode into a linear buffer
// and blit to the selected plane, avoiding writable texture arrays on Apple5.
static id<MTLComputePipelineState> bc_blocks_pipeline(void) {
    static id<MTLComputePipelineState> pipeline;
    if (pipeline) return pipeline;
    const char *source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Params { uint width, height, block_row, pixel_row, channels, snorm; };\n"
        "kernel void decode_bc45(device const uchar *blocks [[buffer(0)]], device uchar *pixels [[buffer(1)]],"
        "constant Params &p [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {\n"
        " if (xy.x >= p.width || xy.y >= p.height) return;\n"
        " uint bit = ((xy.y & 3) * 4 + (xy.x & 3)) * 3;\n"
        " for (uint c = 0; c < p.channels; c++) {\n"
        "  device const uchar *b = blocks + (xy.y / 4) * p.block_row + (xy.x / 4) * p.channels * 8 + c * 8;\n"
        "  uint index = uint(b[2 + bit / 8]) >> (bit & 7);\n"
        "  if ((bit & 7) > 5) index |= uint(b[3 + bit / 8]) << (8 - (bit & 7));\n"
        "  index &= 7;\n"
        "  int a = p.snorm ? max(int(as_type<char>(b[0])), -127) : int(b[0]);\n"
        "  int z = p.snorm ? max(int(as_type<char>(b[1])), -127) : int(b[1]);\n"
        "  int value;\n"
        "  if (index < 2) value = index == 0 ? a : z;\n"
        "  else if (a > z) {\n"
        "   const int weights[6] = {9363,18724,28086,37450,46812,56173};\n"
        "   value = (weights[7 - index] * a + weights[index - 2] * z + 32768) >> 16;\n"
        "  } else if (index < 6) {\n"
        "   const int weights[4] = {13107,26215,39321,52429};\n"
        "   value = (weights[5 - index] * a + weights[index - 2] * z + 32768) >> 16;\n"
        "  } else value = index == 6 ? (p.snorm ? -127 : 0) : (p.snorm ? 127 : 255);\n"
        "  pixels[xy.y * p.pixel_row + xy.x * p.channels + c] = uchar(value);\n"
        " }\n}\n";
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@(source) options:nil error:&error];
    if (!library) MDIE("BC GPU decoder compilation failed: %s", error.localizedDescription.UTF8String);
    pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"decode_bc45"] error:&error];
    if (!pipeline) MDIE("BC GPU decoder pipeline failed: %s", error.localizedDescription.UTF8String);
    MLOG("BC4/BC5 GPU decoder ready");
    return pipeline;
}
#include "nvn_bc123_shader.inc"
static int bc_blocks_gpu_format(uint32_t format) { return format >= 0x41 && format <= 0x4c; }
static int bc_blocks_gpu_sample(NvBcBlocks *blocks, id<MTLTexture> texture) {
    nvn_bc_format_info info;
    return blocks && bc_blocks_gpu_format(blocks->format) && nvn_bc_format(blocks->format, &info) &&
        texture.pixelFormat == (MTLPixelFormat)info.pixel_format;
}
static void bc_blocks_encode(id<MTLComputeCommandEncoder> encoder, uint32_t format, id<MTLBuffer> blocks,
                             NSUInteger offset, NSUInteger w, NSUInteger h, id<MTLBuffer> pixels, NSUInteger row) {
    if (!bc_blocks_gpu_format(format)) MDIE("GPU BC decode is not implemented for format %#x", format);
    uint32_t channels = format >= 0x4b ? 2 : 1;
    nvn_bc_format_info info; nvn_bc_format(format, &info);
    uint32_t kind = format <= 0x48 ? ((format - 0x41) % 4 < 2 ? 1 : (format - 0x41) % 4) : channels;
    uint32_t params[] = {(uint32_t)w, (uint32_t)h, (uint32_t)((w + 3) / 4 * info.block_bytes), (uint32_t)row, kind, format == 0x4a || format == 0x4c};
    enc_c_pso(encoder, format <= 0x48 ? bc123_pipeline() : bc_blocks_pipeline());
    [encoder setBuffer:blocks offset:offset atIndex:0];
    [encoder setBuffer:pixels offset:0 atIndex:1];
    [encoder setBytes:params length:sizeof params atIndex:2];
    enc_c_dispatch(encoder, MTLSizeMake((w + 7) / 8, (h + 7) / 8, 1), MTLSizeMake(8, 8, 1));
}
static void bc_blocks_decode(NvBcBlocks *blocks, NSUInteger offset, NSUInteger w, NSUInteger h, id<MTLBuffer> pixels, NSUInteger row) {
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLComputeCommandEncoder> encoder = enc_c_begin(nil);
    bc_blocks_encode(encoder, blocks->format, bc_blocks_buffer(blocks), offset, w, h, pixels, row);
    enc_c_end(encoder);
}

static int bc_upload_gpu(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *e = getenv("DM_NVN_BC_GPU_UPLOAD"); enabled = !(e && !strcmp(e, "0")); }
    return enabled;
}
static int bc_upload_validation(uint32_t format, NSUInteger w, NSUInteger h) {
    static int enabled = -1; static unsigned checked;
    if (enabled < 0) { const char *e = getenv("DM_NVN_BC_UPLOAD_VALIDATE"); enabled = e && !strcmp(e, "1"); }
    if (!enabled || !bc_blocks_gpu_format(format) || w < 1024 || h < 1024) return 0;
    unsigned bit = 1u << (format - 0x41);
    if (checked & bit) return 0;
    checked |= bit;
    return 1;
}
static void bc_upload_compare(uint32_t format, const uint8_t *blocks, size_t block_bytes, size_t block_row,
                              uint32_t w, uint32_t h, id<MTLBuffer> pixels, size_t row) {
    size_t bytes = row * h;
    uint8_t *reference = malloc(bytes);
    if (!reference) MDIE("BC upload validation allocation failed");
    if (nvn_bc_decode(format, blocks, block_bytes, block_row, w, h, reference, bytes, row))
        MDIE("BC upload reference decode failed");
    const uint8_t *actual = pixels.contents;
    size_t width = (size_t)w * (format >= 0x4b ? 2 : 1), mismatches = 0, first = SIZE_MAX;
    for (uint32_t y = 0; y < h; y++) for (size_t x = 0; x < width; x++) {
        size_t offset = y * row + x;
        if (actual[offset] != reference[offset]) { if (!mismatches) first = offset; mismatches++; }
    }
    MLOG("BC upload validation format=%#x size=%ux%u bytes=%zu mismatches=%zu", format, w, h, width * h, mismatches);
    if (mismatches) MDIE("BC upload mismatch offset=%zu GPU=%u CPU=%u", first, actual[first], reference[first]);
    free(reference);
}
