// Exercise translated bindings with independent argument buffers for two draws.
static void mixed_argument_bindings(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { const device uint *input [[id(0)]]; texture2d<float> texture [[id(1)]]; sampler sample [[id(2)]];"
        "device uint *output [[id(3)]]; array<texture2d<float>, 2> textures [[id(4)]]; array<sampler, 2> samples [[id(6)]];"
        "texture2d<float, access::write> image [[id(8)]]; };\n"
        "kernel void mixed(constant Args &a [[buffer(0)]]) {"
        "uint value = a.input[0] + uint(round(a.texture.sample(a.sample, float2(0.5)).r));"
        "a.output[0] = value; a.output[1] = uint(round(a.textures[0].sample(a.samples[0], float2(0.5)).r +"
        "a.textures[1].sample(a.samples[1], float2(0.5)).r)); a.output[2] = a.input[1];"
        "a.image.write(float4(value), uint2(a.input[1], 0)); }\n" options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvShader *shader = [NvShader new]; shader->stage = 5; shader->fn = [library newFunctionWithName:@"mixed"];
    shader->rf.nb = 7;
    shader->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .aid = 0};
    shader->rf.b[1] = (nvn_rbind){.kind = NVN_RB_TEX, .aid = 1, .dtt = MTLTextureType2D};
    shader->rf.b[2] = (nvn_rbind){.kind = NVN_RB_SAMP, .aid = 2};
    shader->rf.b[3] = (nvn_rbind){.kind = NVN_RB_SBUFP, .aid = 3, .rw = 1};
    shader->rf.b[4] = (nvn_rbind){.kind = NVN_RB_TEXD, .aid = 4, .n = 2, .dtt = MTLTextureType2D};
    shader->rf.b[5] = (nvn_rbind){.kind = NVN_RB_SAMPD, .aid = 6, .n = 2};
    shader->rf.b[6] = (nvn_rbind){.kind = NVN_RB_IMG, .aid = 8, .dtt = MTLTextureType2D, .rw = 1};
    shader->ae = [shader->fn newArgumentEncoderWithBufferIndex:0]; shader->arg_len = shader->ae.encodedLength;
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:shader->fn error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    size_t page = (size_t)getpagesize(); uint8_t *memory;
    if (posix_memalign((void **)&memory, page, page)) abort();
    memset(memory, 0, page); mtl_pool_add(memory, page);
    const uint32_t inputs[8] = {31, 0, 0, 0, 59, 1, 0, 0}; memcpy(memory + 256, inputs, sizeof inputs);
    __strong id<MTLTexture> textures[3]; tex_t guest[3] = {0}; pool_t pools[3] = {0};
    for (unsigned i = 0; i < 3; i++) {
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:4 height:4 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        textures[i] = [g_dev newTextureWithDescriptor:descriptor];
        float pixels[16]; for (unsigned j = 0; j < 16; j++) pixels[j] = i == 0 ? 7 : i == 1 ? 11 : 0;
        [textures[i] replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:pixels bytesPerRow:16];
        guest[i] = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial(), .pool = &pools[i]};
        guest[i].d = (nvn_texdesc){.width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1,
            .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F};
        serial_put(guest[i].serial, textures[i]); mtl_reg_tex(720 + i, &guest[i], NULL);
    }
    id<MTLBuffer> readback = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    state_reset(); mtl_sub_begin();
    for (unsigned i = 0; i < 2; i++) {
        S.ubo[5][0] = (p_bind_buf){.addr = (uint64_t)(memory + 256 + i * 16), .size = 8};
        S.ssbo[5][0] = (p_bind_buf){.addr = (uint64_t)(memory + 512 + i * 16), .size = 12};
        S.tex[5][0] = 720 + i; S.tex[5][1] = 721 - i; S.img[5][0] = 722;
        args_dirty_all();
        id<MTLComputeCommandEncoder> encoder = enc_c_begin(nil); enc_c_pso(encoder, pipeline);
        encode_args(encoder, shader, 5, -1);
        enc_c_dispatch(encoder, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(encoder);
    }
    id<MTLBlitCommandEncoder> encoder = enc_b_begin(g_cb);
    [encoder copyFromTexture:textures[2] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
        sourceSize:MTLSizeMake(4, 4, 1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:1024];
    enc_b_end(encoder);
    EXPECT("mixed argument submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("mixed argument GPU succeeds", 0, mtl_last_commit_error());
    for (unsigned i = 0; i < 2; i++) {
        const uint32_t *output = (const uint32_t *)(memory + 512 + i * 16);
        EXPECT("independent argument buffer retains its resources", i ? 70 : 38, output[0]);
        EXPECT("texture and sampler arrays are encoded by element", 18, output[1]);
        EXPECT("argument buffer subrange retains its offset", i, output[2]);
        EXPECT("writable argument texture reaches blit consumer", i ? 70 : 38, (uint32_t)((const float *)readback.contents)[i]);
    }
    for (unsigned i = 0; i < 3; i++) mtl_tex_release(&guest[i]);
    mtl_pool_del(memory); mtl_drain(); free(memory); state_reset();
}

static void converted_indirect_indices(void) {
    size_t page = (size_t)getpagesize(); uint8_t *memory;
    if (posix_memalign((void **)&memory, page, page)) abort();
    memset(memory, 0, page); mtl_pool_add(memory, page); state_reset();
    for (unsigned indexed = 0; indexed < 4; indexed++) for (int primitive = 4; primitive <= 7; primitive++) {
        if (primitive == 5 || (!indexed && primitive == 4)) continue;
        uint32_t command[5] = {8, 3, indexed ? 2 : 17, indexed ? (uint32_t)-3 : 9, 9};
        uint8_t bytes[64] = {0}; unsigned size = indexed ? 1u << (indexed - 1) : 1;
        for (unsigned i = 0; i < 10; i++) { uint32_t value = 1 + 3 * i; memcpy(bytes + i * size, &value, size); }
        p_draw arrays = {.prim = primitive, .ind = (uint64_t)(memory + 128)};
        p_draw_idx elements = {.prim = primitive, .itype = (int)indexed - 1,
            .ind = (uint64_t)(memory + 128), .ibuf = (uint64_t)(memory + 256)};
        mtl_sub_begin(); mtl_sub_update(command, (uint64_t)(memory + 128), sizeof command);
        mtl_sub_update(bytes, (uint64_t)(memory + 256), sizeof bytes);
        ixc_run(indexed ? NULL : &arrays, indexed ? &elements : NULL);
        const ixc_ent *entry = ixc_find(indexed ? NULL : &arrays, indexed ? &elements : NULL);
        EXPECT("indirect conversion produces a record", 1, entry && entry->args);
        if (!entry || !entry->args) abort();
        NSUInteger argument_offset, index_offset;
        id<MTLBuffer> arguments = gpu_buffer_required(entry->args, 20, &argument_offset);
        id<MTLBuffer> indices = gpu_buffer_required(entry->ixb, entry->ixlen, &index_offset);
        EXPECT("indirect conversion submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
        EXPECT("indirect conversion GPU succeeds", 0, mtl_last_commit_error());
        const uint32_t *result = (const uint32_t *)((const uint8_t *)arguments.contents + argument_offset);
        unsigned count = primitive == 7 ? 12 : primitive == 6 ? 18 : 8;
        EXPECT("GPU-written count determines converted count", count, result[0]);
        EXPECT("converted arguments preserve instances", 3, result[1]);
        EXPECT("converted indices start at zero", 0, result[2]);
        EXPECT("converted arguments preserve signed base vertex", indexed ? (uint32_t)-3 : 17, result[3]);
        EXPECT("converted arguments preserve base instance", 9, result[4]);
        const unsigned quads[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        const unsigned fans[] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 6, 0, 6, 7};
        const uint32_t *output = (const uint32_t *)((const uint8_t *)indices.contents + index_offset);
        for (unsigned i = 0; i < count; i++) {
            unsigned position = primitive == 7 ? quads[i] : primitive == 6 ? fans[i] : i;
            EXPECT("converted index topology", indexed ? 1 + 3 * (position + 2) : position, output[i]);
        }
    }
    uint8_t source[19]; for (unsigned i = 0; i < sizeof source; i++) source[i] = i == 18 ? 255 : i * 7;
    p_draw_idx draw = {.prim = 4, .itype = 0, .count = sizeof source, .ibuf = (uint64_t)(memory + 517)};
    mtl_sub_begin(); mtl_sub_update(source, draw.ibuf, sizeof source); wide_run(&draw);
    const wide_ent *entry = wide_find(&draw); EXPECT("GPU index widening produces output", 1, entry && entry->gpu);
    if (!entry || !entry->gpu) abort();
    NSUInteger offset; id<MTLBuffer> output = gpu_buffer_required(entry->gpu, 2 * sizeof source, &offset);
    EXPECT("GPU index widening completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("GPU index widening succeeds", 0, mtl_last_commit_error());
    const uint16_t *values = (const uint16_t *)((const uint8_t *)output.contents + offset);
    for (unsigned i = 0; i < sizeof source; i++) EXPECT("unaligned GPU-written indices are widened exactly", source[i], values[i]);
    mtl_pool_del(memory); mtl_drain(); free(memory); state_reset();
}

#include "gen/legacy_samplers.h"
static void compact_argument_samplers(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@(legacy_sampler_source) options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvShader *shader = [NvShader new]; shader->stage = 5; shader->fn = [library newFunctionWithName:@"main0"];
    shader->rf.nb = 19;
    shader->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .aid = 0, .dtt = MTLTextureType2D};
    shader->rf.b[1] = (nvn_rbind){.kind = NVN_RB_SBUFP, .aid = 1, .rw = 1};
    for (unsigned i = 0; i < 17; i++) {
        shader->rf.b[i + 2] = (nvn_rbind){.kind = NVN_RB_SAMP, .aid = i + 2, .reg = i};
        shader->sampler_ids[i] = i + 2;
    }
    shader->sampler_count = 17; shader->sampler_table_arg = 19; shader->sampler_map_arg = 35;
    shader->ae = [shader->fn newArgumentEncoderWithBufferIndex:0]; shader->arg_len = shader->ae.encodedLength;
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:shader->fn error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    size_t page = getpagesize(); uint8_t *memory;
    if (posix_memalign((void **)&memory, page, page)) abort();
    memset(memory, 0, page); mtl_pool_add(memory, page);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:4 height:1 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:td];
    float pixels[] = {2, 6, 14, 30};
    [texture replaceRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0 withBytes:pixels bytesPerRow:16];
    pool_t pool = {0}; tex_t guest = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial(), .pool = &pool};
    guest.d = (nvn_texdesc){.width = 4, .height = 1, .depth = 1, .levels = 1, .samples = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_R32F};
    serial_put(guest.serial, texture); mtl_reg_tex(760, &guest, NULL);
    for (unsigned i = 0; i < 17; i++) {
        samp_par p = {.minf = (i % 4) >= 2, .magf = (i % 4) >= 2, .ws = (i % 2) == 0, .wt = 0, .wr = 0, .lod_max = 1000, .aniso = 1};
        unsigned color = (i / 4) % 3;
        p.border[0] = p.border[1] = p.border[2] = color == 1;
        p.border[3] = color != 0;
        mtl_reg_samp(760 + i, &p);
    }
    pthread_mutex_lock(&g_rmu);
    EXPECT("unused white borders do not change sampler state", 1, samp_by_id(760) == samp_by_id(764));
    EXPECT("unused black borders do not change sampler state", 1, samp_by_id(760) == samp_by_id(768));
    pthread_mutex_unlock(&g_rmu);
    state_reset(); mtl_sub_begin();
    for (unsigned draw = 0; draw < 2; draw++) {
        S.tex[5][0] = 760;
        S.ssbo[5][0] = (p_bind_buf){.addr = (uint64_t)(memory + draw * 128), .size = 17 * 4};
        for (unsigned i = 0; i < 17; i++) S.samp[5][i] = (uint64_t)(760 + (draw ? 16 - i : i)) << 20;
        args_dirty_all();
        id<MTLComputeCommandEncoder> encoder = enc_c_begin(nil); enc_c_pso(encoder, pipeline);
        encode_args(encoder, shader, 5, -1); enc_c_dispatch(encoder, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(encoder);
    }
    EXPECT("compact sampler submission completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("compact sampler GPU succeeds", 0, mtl_last_commit_error());
    const unsigned values[4] = {6, 30, 4, 30};
    for (unsigned draw = 0; draw < 2; draw++) for (unsigned i = 0; i < 17; i++)
        EXPECT("logical samplers retain filtering and wrap state", values[(draw ? 16 - i : i) % 4], ((uint32_t *)(memory + draw * 128))[i]);
    uint64_t slots[17], physical[16]; uint16_t ids[17]; uint32_t map[17];
    for (unsigned i = 0; i < 17; i++) { slots[i] = i + 1; ids[i] = i; }
    EXPECT("seventeen distinct samplers fail without aliasing", 0, argument_sampler_map(slots, ids, 17, physical, map));
    mtl_tex_release(&guest); mtl_pool_del(memory); mtl_drain(); free(memory); state_reset();
}

static void sampler_border_addressing(void) {
    if (![g_dev supportsFamily:MTLGPUFamilyApple7] && ![g_dev supportsFamily:MTLGPUFamilyMac2]) return;
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void border(texture3d<float> image [[texture(0)]], sampler sample [[sampler(0)]],"
        "constant float4 &coordinate [[buffer(0)]], device float *output [[buffer(1)]]) {"
        "output[0] = image.sample(sample, coordinate.xyz).r; }\n" options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"border"] error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType3D; td.pixelFormat = MTLPixelFormatR32Float;
    td.width = td.height = td.depth = 1; td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:td]; float pixel = 0.25f;
    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:&pixel bytesPerRow:4 bytesPerImage:4];
    id<MTLBuffer> output = [g_dev newBufferWithLength:9 * sizeof(float) options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [g_dev newCommandQueue]; id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline]; [encoder setTexture:texture atIndex:0];
    for (unsigned axis = 0; axis < 3; axis++) for (unsigned color = 0; color < 3; color++) {
        samp_par p = {.ws = 7, .wt = 7, .wr = 7, .lod_max = 1000, .aniso = 1, .border = {1, 1, 1, 1}};
        if (color != 2) {
            if (axis == 0) p.ws = 5; else if (axis == 1) p.wt = 5; else p.wr = 5;
            p.border[0] = p.border[1] = p.border[2] = color;
        }
        unsigned index = axis * 3 + color; mtl_reg_samp(800 + index, &p);
        pthread_mutex_lock(&g_rmu); id<MTLSamplerState> sampler = samp_by_id(800 + index); pthread_mutex_unlock(&g_rmu);
        EXPECT("border sampler exists", 1, sampler != nil);
        float coordinate[4] = {0.5f, 0.5f, 0.5f, 0}; coordinate[axis] = -1;
        [encoder setSamplerState:sampler atIndex:0]; [encoder setBytes:coordinate length:sizeof coordinate atIndex:0];
        [encoder setBuffer:output offset:index * sizeof(float) atIndex:1];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    }
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    EXPECT("border sampling GPU succeeds", MTLCommandBufferStatusCompleted, command.status);
    for (unsigned axis = 0; axis < 3; axis++) for (unsigned color = 0; color < 3; color++)
        EXPECT("only border addressing uses border color", color == 2 ? 1 : 4 * color,
            (unsigned)lroundf(4 * ((const float *)output.contents)[axis * 3 + color]));
}
