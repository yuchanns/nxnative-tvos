// Opt-in device regression. This runs on the target GPU before guest execution;
// it requires no game assets, native BC support, or desktop GPU execution.
static unsigned g_bc_test_checks, g_bc_test_failures;
#define BC_CHECK(value) do { g_bc_test_checks++; if (!(value)) { g_bc_test_failures++; MLOG("[RED] BC self-test line %d: %s", __LINE__, #value); } } while (0)

static tex_t bc_test_texture(uint32_t format, unsigned target, void *memory, pool_t *pool) {
    tex_t result = {0};
    result.magic = MAGIC('T', 'E', 'X', 0); result.serial = nvn_new_serial();
    result.d = (nvn_texdesc){.target = target, .format = format, .width = 30, .height = 18,
        .depth = target == 2 ? 5 : target == 4 ? 3 : 1, .levels = 3};
    result.mem = memory; result.pool = pool;
    nvn_tex_layout(&result.d, &result.L);
    MTLTextureDescriptor *descriptor = tex_desc(&result, 0);
    id<MTLTexture> host = [g_dev newTextureWithDescriptor:descriptor];
    if (!host) MDIE("BC self-test texture creation failed for %#x target %u", format, target);
    res_add(host); tex_put(result.serial, host);
    bc_blocks_ensure(&result, host);
    return result;
}

static void bc_test_pixels(tex_t *texture, unsigned level, unsigned layer, unsigned z,
                            const uint8_t *encoded, size_t bytes) {
    id<MTLTexture> host = tex_mtl(texture);
    nvn_bc_format_info info; nvn_bc_format(texture->d.format, &info);
    unsigned width = mip_extent(host.width, level), height = mip_extent(host.height, level);
    uint32_t astc_format = 0;
    int astc = nvn_bc_astc_format(texture->d.format, g_nvn_bc_astc, &astc_format) && host.pixelFormat == (MTLPixelFormat)astc_format;
    unsigned block = astc ? g_nvn_bc_astc : 1;
    size_t tight = astc ? ((width + block - 1) / block) * 16 : width * info.pixel_bytes;
    size_t rows = (height + block - 1) / block, row = (tight + 255) & ~(size_t)255;
    uint8_t *reference = malloc(tight * rows);
    BC_CHECK(reference != NULL);
    if (!reference) return;
    if (astc) BC_CHECK(!nvn_bc_astc_encode(texture->d.format, encoded, bytes, width, height, block,
        reference, tight * rows, NULL, NULL, NULL));
    else BC_CHECK(!nvn_bc_decode(texture->d.format, encoded, bytes, ((width + 3) / 4) * info.block_bytes,
        width, height, reference, tight * rows, tight));
    id<MTLBuffer> result = [g_dev newBufferWithLength:row * rows options:MTLResourceStorageModeShared];
    mtl_sub_begin();
    enc_b_copy_texture_buffer(blit(), host, layer, level, MTLOriginMake(0, 0, z), MTLSizeMake(width, height, 1), result, 0, row, row * rows);
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull)); BC_CHECK(!mtl_last_commit_error());
    for (size_t y = 0; y < rows; y++) BC_CHECK(!memcmp((uint8_t *)result.contents + y * row, reference + y * tight, tight));
    free(reference);
}

static void bc_test_case(uint32_t format, unsigned target, unsigned astc) {
    g_nvn_bc_astc = astc;
    const size_t allocation = 1 << 16;
    uint8_t *memory = NULL;
    if (posix_memalign((void **)&memory, getpagesize(), allocation)) MDIE("BC self-test pool allocation failed");
    memset(memory, 0, allocation); mtl_pool_add(memory, allocation);
    pool_t pool = {0};
    mtl_sub_begin();
    tex_t first = bc_test_texture(format, target, memory, &pool);
    tex_t second = bc_test_texture(format, target, memory + 8192, &pool);
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull));
    const unsigned level = 1, width = 15, height = 9, depth = target == 1 ? 1 : 2;
    nvn_bc_format_info info; nvn_bc_format(format, &info);
    size_t block_row = 4 * info.block_bytes, plane = block_row * 3;
    uint8_t *expected = calloc(depth, plane);
    NVNcopyRegion region = {4, 4, 0, 11, 5, depth};
    size_t row = 5 * info.block_bytes, image = 13 * info.block_bytes;
    size_t span = (depth - 1) * image + row + 3 * info.block_bytes;
    uint8_t *input = memory + 16384, *output = memory + 32768;
    view_t view = {.set_levels = 1, .base_level = level, .nlevels = 1};
    for (unsigned pass = 0; pass < 2; pass++) {
        uint8_t payload[512]; memset(payload, 0xcc, sizeof payload);
        for (unsigned z = 0; z < depth; z++) for (unsigned y = 0; y < 2; y++) for (unsigned x = 0; x < 3; x++) {
            uint8_t *block = payload + z * image + y * row + x * info.block_bytes;
            for (unsigned i = 0; i < info.block_bytes; i++) block[i] = (uint8_t)(17 * i + 23 * x + 71 * z + pass * 33 + y);
            if (format == 0x4d || format == 0x4e) block[0] = (block[0] & 0x80) | 0x40;
            if (format >= 0x4f) block[0] &= ~3u;
            memcpy(expected + z * plane + (y + 1) * block_row + (x + 1) * info.block_bytes, block, info.block_bytes);
        }
        memset(output, 0xa5, 4096);
        id<MTLBuffer> producer = [g_dev newBufferWithBytes:payload length:span options:MTLResourceStorageModeShared];
        mtl_sub_begin();
        void *buffer; uint64_t offset; BC_CHECK(mtl_addr((uint64_t)input, span, &buffer, &offset));
        // Exercise a producer in the same submission, not just CPU input.
        enc_b_copy_buffer(blit(), producer, 0, (__bridge id<MTLBuffer>)buffer, offset, span);
        S.copy_stride = row; S.copy_img_stride = image;
        p_copy_b2t copy = {.tex = &first, .has_view = 1, .view = view, .r = region, .src = (uint64_t)input};
        copy_b2t(&copy);
        p_copy_t2t texture_copy = {.src = &first, .dst = &second, .has_sv = 1, .has_dv = 1, .sv = view, .dv = view,
            .sr = {0, 0, 0, width, height, depth}, .dr = {0, 0, 0, width, height, depth}};
        copy_t2t(&texture_copy);
        S.copy_stride = block_row; S.copy_img_stride = plane;
        p_copy_b2t read = {.tex = &second, .has_view = 1, .view = view,
            .r = {0, 0, 0, width, height, depth}, .src = (uint64_t)output};
        copy_t2b(&read);
        BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull)); BC_CHECK(!mtl_last_commit_error());
        BC_CHECK(!memcmp(output, expected, plane * depth));
        BC_CHECK(output[plane * depth] == 0xa5);
        memset(output, 0xa5, 4096);
        mtl_read_texels(&second, level, 0, &read.r, output, block_row, plane);
        BC_CHECK(!memcmp(output, expected, plane * depth));
    }
    for (unsigned z = 0; z < depth; z++)
        bc_test_pixels(&second, level, target == 2 ? 0 : z, target == 2 ? z : 0, expected + z * plane, plane);

    // Overlapping alias copies need a snapshot before any destination write.
    mtl_sub_begin();
    p_copy_t2t overlap = {.src = &second, .dst = &second, .has_sv = 1, .has_dv = 1, .sv = view, .dv = view,
        .sr = {0, 4, 0, 8, 4, depth}, .dr = {4, 4, 0, 8, 4, depth}};
    copy_t2t(&overlap);
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull)); BC_CHECK(!mtl_last_commit_error());
    for (unsigned z = 0; z < depth; z++)
        memmove(expected + z * plane + block_row + info.block_bytes, expected + z * plane + block_row, 2 * info.block_bytes);

    // Block-format partial clears must preserve the unselected words.
    view_t raw_view = view; raw_view.set_format = 1;
    raw_view.format = info.block_bytes == 8 ? NVN_FORMAT_RGBA16UI : NVN_FORMAT_RGBA32UI;
    p_clear_tex clear = {.tex = &second, .has_view = 1, .view = raw_view, .r = {1, 1, 0, 1, 1, 1}, .mask = 5, .is_uint = 1};
    clear.c.u[0] = 0x1234; clear.c.u[2] = 0x5678;
    uint8_t *changed = expected + block_row + info.block_bytes;
    if (info.block_bytes == 8) { uint16_t a = 0x1234, b = 0x5678; memcpy(changed, &a, 2); memcpy(changed + 4, &b, 2); }
    else { memcpy(changed, &clear.c.u[0], 4); memcpy(changed + 8, &clear.c.u[2], 4); }
    mtl_sub_begin(); clear_tex(&clear);
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull)); BC_CHECK(!mtl_last_commit_error());
    NVNcopyRegion full = {0, 0, 0, width, height, depth};
    mtl_read_texels(&second, level, 0, &full, output, block_row, plane);
    BC_CHECK(!memcmp(output, expected, plane * depth));
    bc_test_pixels(&second, level, 0, 0, expected, plane);

    // Reading a raw descriptor sees the original BC bytes, never ASTC bytes.
    raw_view.set_layers = 1; raw_view.min_layer = 0; raw_view.nlayers = 1;
    const uint8_t swizzle[4] = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha};
    mtl_sub_begin();
    id<MTLTexture> raw = bc_raw_view(&second, tex_mtl(&second), &raw_view, swizzle);
    id<MTLBuffer> raw_bytes = [g_dev newBufferWithLength:plane options:MTLResourceStorageModeShared];
    enc_b_copy_texture_buffer(blit(), raw, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 3, 1), raw_bytes, 0, block_row, plane);
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull)); BC_CHECK(!mtl_last_commit_error());
    BC_CHECK(!memcmp(raw_bytes.contents, expected, plane));
    // A write through a raw descriptor must become visible to BC sampling.
    p_clear_tex raw_clear = {.r = {0, 0, 0, 1, 1, 1}, .mask = 15, .is_uint = 1};
    mtl_sub_begin(); clear_tex_tri(raw, 0, 0, &raw_clear); bc_raw_note(raw); bc_raw_flush();
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull)); BC_CHECK(!mtl_last_commit_error());
    memset(expected, 0, info.block_bytes);
    mtl_read_texels(&second, level, 0, &full, output, block_row, plane);
    BC_CHECK(!memcmp(output, expected, plane * depth));
    bc_test_pixels(&second, level, 0, 0, expected, plane);

    // CPU patching after GPU mutation must retain untouched blocks and layers.
    mtl_bc_cpu_write_prepare(&second);
    uint8_t *linear = calloc(depth, plane);
    for (unsigned z = 0; z < depth; z++) {
        if (target == 2 && z) break;
        BC_CHECK(!nvn_deswizzle(&second.d, level, target == 2 ? 0 : z,
            second.mem, linear + (target == 2 ? 0 : z * plane), block_row));
    }
    BC_CHECK(!memcmp(linear, expected, plane * depth));
    memset(linear + 2 * info.block_bytes, 0, info.block_bytes);
    memset(expected + 2 * info.block_bytes, 0, info.block_bytes);
    BC_CHECK(!nvn_swizzle(&second.d, level, 0, linear, block_row, second.mem));
    mtl_tex_cpu_written(&second);
    mtl_read_texels(&second, level, 0, &full, output, block_row, plane);
    BC_CHECK(!memcmp(output, expected, plane * depth));
    mtl_sub_begin();
    (void)tex_mtl(&second);
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull)); BC_CHECK(!mtl_last_commit_error());
    bc_test_pixels(&second, level, 0, 0, expected, plane);
    free(linear);
    free(expected);
    mtl_tex_release(&first); mtl_tex_release(&second); mtl_pool_del(memory); mtl_finish_gpu();
    free(memory);
    MLOG("BC self-test format=%#x target=%u ASTC=%u checks=%u failures=%u", format, target, astc, g_bc_test_checks, g_bc_test_failures);
}

static void bc_test_emit(uint8_t *commands, size_t *used, unsigned op, const void *payload, size_t size) {
    size_t bytes = (sizeof(op_hdr) + size + 7) & ~(size_t)7;
    if (*used + bytes > 4096) MDIE("BC self-test command list overflow");
    op_hdr header = {op, (uint32_t)bytes};
    memcpy(commands + *used, &header, sizeof header);
    if (size) memcpy(commands + *used + sizeof header, payload, size);
    *used += bytes;
}

static void bc_test_batch(unsigned astc) {
    g_nvn_bc_astc = astc;
    uint8_t *memory = NULL;
    if (posix_memalign((void **)&memory, getpagesize(), 1 << 16)) MDIE("BC batch self-test allocation failed");
    memset(memory, 0, 1 << 16); mtl_pool_add(memory, 1 << 16);
    pool_t pool = {0};
    mtl_sub_begin();
    tex_t texture = bc_test_texture(0x41, 4, memory, &pool);
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull));
    uint8_t payload[1024];
    for (unsigned i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 17 + i / 11);
    id<MTLBuffer> producer = [g_dev newBufferWithBytes:payload length:sizeof payload options:MTLResourceStorageModeShared];
    uint8_t commands[4096] = {0}; size_t used = 0;
    size_t offsets[3], lengths[3], offset = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned level = i == 3 ? 1 : i, width = MAX(30u >> level, 1u), height = MAX(18u >> level, 1u);
        size_t bytes = ((width + 3) / 4) * ((height + 3) / 4) * 8;
        if (i == 3) offset = 512;
        offsets[level] = offset; lengths[level] = bytes;
        p_copy_stride row = {.stride = 0, .image = 0}, image = {.stride = 0, .image = 1};
        bc_test_emit(commands, &used, OP_COPY_STRIDE, &row, sizeof row);
        bc_test_emit(commands, &used, OP_COPY_STRIDE, &image, sizeof image);
        p_copy_b2t copy = {.tex = &texture, .has_view = 1,
            .view = {.set_levels = 1, .base_level = level, .nlevels = 1, .set_layers = 1, .min_layer = 1, .nlayers = 1},
            .r = {0, 0, 0, width, height, 1}, .src = (uint64_t)(memory + 16384 + offset)};
        bc_test_emit(commands, &used, OP_COPY_B2T, &copy, sizeof copy);
        offset += bytes;
    }
    bc_test_emit(commands, &used, OP_END, NULL, 0);
    mtl_bc_copy_timing before, after; mtl_bc_copy_timing_snapshot(&before);
    mtl_sub_begin();
    void *buffer; uint64_t buffer_offset;
    BC_CHECK(mtl_range_addr((uint64_t)(memory + 16384), sizeof payload, &buffer, &buffer_offset));
    // Input bytes exist only after the producer in this same submission runs.
    enc_b_copy_buffer(blit(), producer, 0, (__bridge id<MTLBuffer>)buffer, buffer_offset, sizeof payload);
    mtl_exec(commands);
    BC_CHECK(!g_bc_copy_pending_n);
    mtl_bc_copy_timing_snapshot(&after);
    BC_CHECK(after.syncs - before.syncs == (bc_copy_batch_on() ? 1 : 4));
    BC_CHECK(after.planes - before.planes == (bc_copy_batch_on() ? 3 : 4));
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull)); BC_CHECK(!mtl_last_commit_error());
    for (unsigned level = 0; level < 3; level++) {
        unsigned width = MAX(30u >> level, 1u), height = MAX(18u >> level, 1u);
        NVNcopyRegion region = {0, 0, 0, width, height, 1};
        memset(memory + 32768, 0xa5, 4096);
        mtl_read_texels(&texture, level, 1, &region, memory + 32768, ((width + 3) / 4) * 8, lengths[level]);
        BC_CHECK(!memcmp(memory + 32768, payload + offsets[level], lengths[level]));
        BC_CHECK(memory[32768 + lengths[level]] == 0xa5);
        bc_test_pixels(&texture, level, 1, 0, payload + offsets[level], lengths[level]);
    }

    // A barrier terminates the batch even when later copies target the same plane.
    used = 0;
    for (unsigned i = 0; i < 2; i++) {
        p_copy_b2t copy = {.tex = &texture, .has_view = 1,
            .view = {.set_levels = 1, .base_level = 0, .nlevels = 1},
            .r = {0, 0, 0, 30, 18, 1}, .src = (uint64_t)(memory + 16384 + i * 512)};
        bc_test_emit(commands, &used, OP_COPY_B2T, &copy, sizeof copy);
        bc_test_emit(commands, &used, OP_BARRIER, NULL, 0);
    }
    bc_test_emit(commands, &used, OP_END, NULL, 0);
    mtl_bc_copy_timing_snapshot(&before);
    mtl_sub_begin(); mtl_exec(commands);
    mtl_bc_copy_timing_snapshot(&after);
    BC_CHECK(after.syncs - before.syncs == 2 && !g_bc_copy_pending_n);
    BC_CHECK(mtl_wait(mtl_sub_commit(), 10000000000ull)); BC_CHECK(!mtl_last_commit_error());
    bc_test_pixels(&texture, 0, 0, 0, payload + 512, lengths[0]);
    mtl_tex_release(&texture); mtl_pool_del(memory); mtl_finish_gpu(); free(memory);
    MLOG("BC batch self-test ASTC=%u enabled=%d checks=%u failures=%u", astc, bc_copy_batch_on(), g_bc_test_checks, g_bc_test_failures);
}

int mtl_bc_selftest(void) {
    mtl_init();
    int saved_decode = g_nvn_bc_decode, saved_astc = g_nvn_bc_astc;
    g_nvn_bc_decode = 1;
    const unsigned targets[] = {1, 4, 2};
    const unsigned modes[] = {0, 4, 6};
    for (unsigned mode = 0; mode < 3; mode++) for (uint32_t format = 0x41; format <= 0x50; format++) {
        if (mode && !nvn_bc_astc_format(format, modes[mode], NULL)) continue;
        for (unsigned shape = 0; shape < 3; shape++) bc_test_case(format, targets[shape], modes[mode]);
    }
    bc_test_batch(4); bc_test_batch(6);
    S.copy_stride = S.copy_img_stride = 0;
    g_nvn_bc_decode = saved_decode; g_nvn_bc_astc = saved_astc;
    MLOG("BC SELFTEST COMPLETE checks=%u failures=%u", g_bc_test_checks, g_bc_test_failures);
    return g_bc_test_failures ? -1 : 0;
}
#undef BC_CHECK
