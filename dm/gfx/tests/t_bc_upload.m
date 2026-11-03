// Compare software BC sampling with native BC, then exercise upload shapes and retirement.
#include "../nvn/nvn_mtl.m"
#include <math.h>
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(condition) do { checks++; if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); failed++; } } while (0)
static float srgb_encode(float value) { return value <= 0.0031308f ? value * 12.92f : 1.055f * powf(value, 1.0f / 2.4f) - 0.055f; }

static void fill_blocks(uint8_t *data, size_t size, uint32_t format, unsigned seed) {
    nvn_bc_format_info info; nvn_bc_format(format, &info);
    uint32_t state = seed + 1;
    for (size_t i = 0; i < size; i++) { state = state * 1664525u + 1013904223u; data[i] = state >> 24; }
    for (size_t i = 0; i < size; i += info.block_bytes) {
        if (format == 0x4d || format == 0x4e) {
            unsigned mode = seed % 8, mask = (1u << (mode + 1)) - 1;
            data[i] = (data[i] & ~mask) | (1u << mode);
        }
        if (format >= 0x4f) data[i] &= ~3u;
    }
}

static tex_t fixture(nvn_texdesc desc, void **allocation) {
    tex_t texture = {0}; texture.d = desc; nvn_tex_layout(&desc, &texture.L);
    size_t page = getpagesize(), bytes = (texture.L.size + page - 1) & ~(page - 1);
    if (posix_memalign(allocation, page, bytes)) abort();
    memset(*allocation, 0, bytes); mtl_pool_add(*allocation, bytes);
    texture.mem = *allocation; texture.pool = calloc(1, sizeof *texture.pool);
    texture.serial = nvn_new_serial(); texture.has_layout = 1; texture.magic = MAGIC('T', 'E', 'X', 0);
    for (unsigned level = 0; level < desc.levels; level++) {
        uint32_t row, rows, depth, bpb; uint64_t base; unsigned th, td;
        if (nvn_deswizzle_geom(&desc, level, 0, &row, &rows, &depth, &base, &th, &td, &bpb)) abort();
        size_t size = (size_t)row * rows * depth; uint8_t *linear = malloc(size);
        for (unsigned layer = 0; layer < texture.L.layers; layer++) {
            fill_blocks(linear, size, desc.format, level + layer * 17);
            if (nvn_swizzle(&desc, level, layer, linear, row, texture.mem)) abort();
        }
        free(linear);
    }
    nvn_dirty_add_range(*allocation, bytes);
    return texture;
}

static void release_fixture(tex_t *texture, void *allocation) {
    nvn_dirty_remove_range(allocation); mtl_pool_del(allocation); mtl_drain(); free(allocation); free(texture->pool);
}

static void native_compare(id<MTLComputePipelineState> pipeline, uint32_t format) {
    void *memory;
    tex_t texture = fixture((nvn_texdesc){.target = 1, .format = format, .width = 16, .height = 8, .depth = 1, .levels = 1}, &memory);
    mtl_sub_begin();
    g_nvn_bc_decode = 0; id<MTLTexture> native = pkg_upload(&texture, NULL, nil);
    g_nvn_bc_decode = 1; id<MTLTexture> unpacked = pkg_upload(&texture, NULL, nil);
    CHECK(native && unpacked && native.pixelFormat != unpacked.pixelFormat);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull));
    id<MTLBuffer> values = [g_dev newBufferWithLength:16 * 8 * 2 * 4 * sizeof(float) options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
    [encoder setComputePipelineState:pipeline]; [encoder setTexture:native atIndex:0]; [encoder setTexture:unpacked atIndex:1];
    [encoder setBuffer:values offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(16, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 4, 1)];
    [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted]; CHECK(!cb.error);
    const float *result = values.contents; float maximum = 0;
    for (size_t i = 0; i < 16 * 8 * 4; i++) {
        float a = result[i * 2], b = result[i * 2 + 1];
        // Native BC can retain interpolation precision that the 8-bit fallback quantizes.
        // Apply the same two-code-value bound before sRGB's nonlinear amplification.
        if (((format >= 0x45 && format <= 0x48) || format == 0x4e) && i % 4 < 3) { a = srgb_encode(a); b = srgb_encode(b); }
        float error = fabsf(a - b), tolerance = format == 0x4a || format == 0x4c ? 2.05f / 127 : 2.05f / 255;
        if (format >= 0x4f) tolerance = fmaxf(0.001f, fabsf(a) * 0.001f);
        maximum = fmaxf(maximum, error / fmaxf(1, fabsf(a)));
        if (!isfinite(a) || !isfinite(b) || error > tolerance) { failed++; break; }
    }
    checks++; printf("native BC format %#x maximum normalized error %.7f\n", format, maximum);
    defer_obj_release(native); defer_obj_release(unpacked); release_fixture(&texture, memory);
}

static void upload_shape(nvn_texdesc desc) {
    void *memory; tex_t texture = fixture(desc, &memory);
    mtl_sub_begin(); g_nvn_bc_decode = 1;
    id<MTLTexture> unpacked = pkg_upload(&texture, NULL, nil);
    CHECK(unpacked != nil); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull));
    nvn_bc_format_info info; nvn_bc_format(desc.format, &info);
    for (unsigned level = 0; level < desc.levels; level++) {
        uint32_t row, rows, depth, bpb; uint64_t base; unsigned th, td;
        nvn_deswizzle_geom(&desc, level, 0, &row, &rows, &depth, &base, &th, &td, &bpb);
        size_t compressed_size = (size_t)row * rows * depth;
        uint8_t *linear = malloc(compressed_size);
        uint32_t w = MAX((unsigned)unpacked.width >> level, 1u), h = MAX((unsigned)unpacked.height >> level, 1u);
        size_t pitch = ((size_t)w * info.pixel_bytes + 255) & ~(size_t)255, image = pitch * h;
        uint8_t *expected = calloc(depth, image);
        for (unsigned layer = 0; layer < texture.L.layers; layer++) {
            fill_blocks(linear, compressed_size, desc.format, level + layer * 17);
            for (unsigned z = 0; z < depth; z++)
                CHECK(!nvn_bc_decode(desc.format, linear + (size_t)z * row * rows, (size_t)row * rows, row, w, h, expected + z * image, image, pitch));
            id<MTLBuffer> readback = [g_dev newBufferWithLength:image * depth options:MTLResourceStorageModeShared];
            memset(readback.contents, 0xa5, readback.length);
            id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> encoder = [cb blitCommandEncoder];
            [encoder copyFromTexture:unpacked sourceSlice:layer sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, depth)
                           toBuffer:readback destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:image * (depth > 1)];
            [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted]; CHECK(!cb.error);
            int equal = 1;
            for (unsigned z = 0; z < depth; z++) for (unsigned y = 0; y < h; y++)
                equal &= !memcmp((uint8_t *)readback.contents + z * image + y * pitch, expected + z * image + y * pitch, (size_t)w * info.pixel_bytes);
            CHECK(equal);
        }
        free(expected); free(linear);
    }
    printf("BC upload target=%d %ux%ux%u levels=%u checked\n", desc.target, desc.width, desc.height, desc.depth, desc.levels);
    defer_obj_release(unpacked); release_fixture(&texture, memory);
}

static id<MTLRenderPipelineState> block_writer(unsigned bytes) {
    static id<MTLRenderPipelineState> pipelines[2];
    unsigned slot = bytes == 16;
    if (pipelines[slot]) return pipelines[slot];
    NSString *source = [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vertex_main(uint i[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}\n"
        "fragment %s4 fragment_main(constant %s4 &block[[buffer(0)]]){return block;}\n", slot ? "uint" : "ushort", slot ? "uint" : "ushort"];
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
    descriptor.colorAttachments[0].pixelFormat = slot ? MTLPixelFormatRGBA32Uint : MTLPixelFormatRGBA16Uint;
    pipelines[slot] = [g_dev newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!pipelines[slot]) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    return pipelines[slot];
}

static void dynamic_blocks(nvn_texdesc desc) {
    void *memory; tex_t texture = fixture(desc, &memory); mtl_tex_init(&texture);
    nvn_bc_format_info info; nvn_bc_format(desc.format, &info);
    g_nvn_bc_decode = 1; mtl_sub_begin();
    id<MTLTexture> decoded = tex_mtl(&texture);
    CHECK(decoded && !bc_blocks_get(decoded));
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull));
    unsigned level = desc.levels - 2, width = MAX(desc.width >> level, 1u), height = MAX(desc.height >> level, 1u);
    unsigned depth = desc.target == 2 ? MAX(desc.depth >> level, 1u) : 1;
    unsigned layer = desc.target == 2 ? 0 : (unsigned)decoded.arrayLength - 1, z = depth - 1;
    unsigned bw = (width + 3) / 4, bh = (height + 3) / 4;
    uint8_t writes[3][16];
    for (unsigned pass = 0; pass < 3; pass++) {
        fill_blocks(writes[pass], info.block_bytes, desc.format, 40 + pass);
        for (unsigned c = 0; c < info.pixel_bytes; c++) {
            writes[pass][c * 8] = pass == 0 ? 0x80 : pass == 1 ? 16 : 240;
            writes[pass][c * 8 + 1] = pass == 1 ? 224 : 32;
        }
        mtl_sub_begin();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        view_t view = {.set_format = 1, .format = info.block_bytes == 8 ? NVN_FORMAT_RGBA16UI : NVN_FORMAT_RGBA32UI,
                       .set_levels = 1, .base_level = level, .nlevels = 1, .set_layers = 1, .min_layer = desc.target == 2 ? z : layer, .nlayers = 1};
        attach(rp.colorAttachments[0], &texture, &view, 1);
        CHECK(rp.colorAttachments[0].texture == g_bcrt_sh[0]);
        CHECK(rp.colorAttachments[0].texture.width == bw && rp.colorAttachments[0].texture.height == bh);
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTLRenderCommandEncoder> encoder = enc_r_begin(rp);
        [encoder setRenderPipelineState:block_writer(info.block_bytes)];
        [encoder setFragmentBytes:writes[pass] length:info.block_bytes atIndex:0];
        [encoder setViewport:(MTLViewport){0, 0, bw, bh, 0, 1}];
        [encoder setScissorRect:(MTLScissorRect){0, 0, pass == 0 ? bw : MAX(bw / 2, 1u), bh}];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        enc_r_end(encoder); bcrt_flush();
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull));
    }
    NvBcBlocks *blocks = bc_blocks_get(decoded); CHECK(blocks != nil);
    CHECK(blocks == bc_blocks_ensure(&texture, decoded));
    for (unsigned lv = 0; lv < desc.levels; lv++) {
        uint32_t row, rows, nz, bpb; uint64_t base; unsigned th, td;
        nvn_deswizzle_geom(&desc, lv, 0, &row, &rows, &nz, &base, &th, &td, &bpb);
        size_t encoded_bytes = (size_t)row * rows * nz; uint8_t *expected_blocks = malloc(encoded_bytes);
        unsigned w = MAX(desc.width >> lv, 1u), h = MAX(desc.height >> lv, 1u);
        size_t pitch = ((size_t)w * info.pixel_bytes + 255) & ~(size_t)255, image = pitch * h;
        uint8_t *expected = calloc(nz, image);
        for (unsigned sl = 0; sl < blocks->layers; sl++) {
            fill_blocks(expected_blocks, encoded_bytes, desc.format, lv + sl * 17);
            if (lv == level && sl == layer) for (unsigned y = 0; y < bh; y++) for (unsigned x = 0; x < bw; x++)
                memcpy(expected_blocks + (size_t)z * row * rows + y * row + x * info.block_bytes,
                       writes[x < MAX(bw / 2, 1u) ? 2 : 0], info.block_bytes);
            CHECK(!memcmp((uint8_t *)blocks->buffer.contents + bc_blocks_offset(blocks, lv, sl, 0), expected_blocks, encoded_bytes));
            for (unsigned plane = 0; plane < nz; plane++)
                CHECK(!nvn_bc_decode(desc.format, expected_blocks + (size_t)plane * row * rows, (size_t)row * rows, row, w, h, expected + plane * image, image, pitch));
            id<MTLBuffer> result = [g_dev newBufferWithLength:image * nz options:MTLResourceStorageModeShared];
            mtl_sub_begin();
            enc_b_copy_texture_buffer(blit(), decoded, sl, lv, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, nz), result, 0, pitch, image);
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull));
            int equal = 1;
            for (unsigned plane = 0; plane < nz; plane++) for (unsigned y = 0; y < h; y++)
                equal &= !memcmp((uint8_t *)result.contents + plane * image + y * pitch, expected + plane * image + y * pitch, (size_t)w * info.pixel_bytes);
            CHECK(equal);
        }
        free(expected); free(expected_blocks);
    }
    // Reusing an idle uploaded allocation must refresh its encoded copy too.
    mtl_sub_begin(); CHECK(pkg_upload(&texture, NULL, decoded) == decoded);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull));
    uint32_t row, rows, nz, bpb; uint64_t base; unsigned th, td;
    nvn_deswizzle_geom(&desc, level, layer, &row, &rows, &nz, &base, &th, &td, &bpb);
    size_t size = (size_t)row * rows * nz; uint8_t *initial = malloc(size);
    fill_blocks(initial, size, desc.format, level + layer * 17);
    CHECK(!memcmp((uint8_t *)blocks->buffer.contents + bc_blocks_offset(blocks, level, layer, 0), initial, size)); free(initial);
    mtl_tex_release(&texture);
    release_fixture(&texture, memory);
    printf("Dynamic BC format=%#x target=%d %ux%ux%u checked\n", desc.format, desc.target, desc.width, desc.height, desc.depth);
}

#include "astc_upload_test.inc"
int main(void) { @autoreleasepool {
    setenv("DM_NVN_BC_DECODE", "1", 1); mtl_init();
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void compare(texture2d<float,access::read>a[[texture(0)]],texture2d<float,access::read>b[[texture(1)]],"
        "device float*out[[buffer(0)]],uint2 p[[thread_position_in_grid]]){float4 x=a.read(p),y=b.read(p);"
        "for(uint c=0;c<4;c++){uint i=((p.y*16+p.x)*4+c)*2;out[i]=x[c];out[i+1]=y[c];}}" options:nil error:&error];
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"compare"] error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    if (!g_dev.supportsBCTextureCompression) { fprintf(stderr, "Native BC comparison needs a BC-capable test GPU\n"); return 1; }
    for (uint32_t format = 0x41; format <= 0x50; format++) native_compare(pipeline, format);
    nvn_texdesc cases[] = {
        {.target = 1, .format = 0x4d, .width = 30, .height = 18, .depth = 1, .levels = 4},
        {.target = 4, .format = 0x4d, .width = 30, .height = 18, .depth = 3, .levels = 4},
        {.target = 8, .format = 0x4d, .width = 16, .height = 16, .depth = 1, .levels = 3},
        {.target = 9, .format = 0x4d, .width = 16, .height = 16, .depth = 2, .levels = 3},
        {.target = 2, .format = 0x4d, .width = 30, .height = 18, .depth = 5, .levels = 3},
        {.target = 1, .format = 0x42, .width = 2048, .height = 1025, .depth = 1, .levels = 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) upload_shape(cases[i]);
    for (uint32_t format = 0x49; format <= 0x4c; format++) {
        dynamic_blocks((nvn_texdesc){.target = 1, .format = format, .width = 30, .height = 18, .depth = 1, .levels = 3});
        dynamic_blocks((nvn_texdesc){.target = 4, .format = format, .width = 30, .height = 18, .depth = 3, .levels = 3});
        dynamic_blocks((nvn_texdesc){.target = 2, .format = format, .width = 30, .height = 18, .depth = 5, .levels = 3});
        dynamic_blocks((nvn_texdesc){.target = 8, .format = format, .width = 16, .height = 16, .depth = 1, .levels = 3});
    }
    astc_cache_contract();
    for (unsigned block = 4; block <= 6; block += 2) {
        g_nvn_bc_astc = block;
        for (uint32_t format = 0x41; format <= 0x4e; format++) if (nvn_bc_astc_format(format, block, NULL)) astc_sample_compare(pipeline, format);
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) astc_upload_shape(cases[i]);
    }
    g_nvn_bc_astc = 0;
    printf("BC upload checks=%d failed=%d\n", checks, failed);
    return failed != 0;
} }
