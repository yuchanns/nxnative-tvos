static void astc_cache_contract(void) {
    NSString *base = [NSTemporaryDirectory() stringByAppendingPathComponent:@"dm-astc-test.XXXXXX"];
    char *directory = strdup(base.fileSystemRepresentation);
    CHECK(mkdtemp(directory) != NULL);
    uint8_t source[64], first[64], second[64];
    fill_blocks(source, sizeof source, 0x44, 42);
    int hit = -1;
    CHECK(!nvn_bc_astc_encode(0x44, source, 64, 7, 5, 4, first, 64, directory, NULL, &hit)); CHECK(hit == 0);
    CHECK(!nvn_bc_astc_encode(0x44, source, 64, 7, 5, 4, second, 64, directory, NULL, &hit)); CHECK(hit == 1);
    CHECK(!memcmp(first, second, 64));
    NSString *root = [NSString stringWithUTF8String:directory];
    NSArray *files = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:root error:NULL]; CHECK(files.count == 1);
    NSString *file = [root stringByAppendingPathComponent:files[0]];
    FILE *corrupt = fopen(file.fileSystemRepresentation, "r+b"); CHECK(corrupt != NULL);
    fseek(corrupt, 40, SEEK_SET); fputc(first[0] ^ 0xff, corrupt); fclose(corrupt);
    CHECK(!nvn_bc_astc_encode(0x44, source, 64, 7, 5, 4, second, 64, directory, NULL, &hit)); CHECK(hit == 0);
    CHECK(!memcmp(first, second, 64));
    CHECK(!nvn_bc_astc_encode(0x44, source, 64, 7, 5, 4, second, 64, "/dev/null/unwritable", directory, &hit)); CHECK(hit == 1);
    CHECK(!truncate(file.fileSystemRepresentation, 47));
    CHECK(!nvn_bc_astc_encode(0x44, source, 64, 7, 5, 4, second, 64, directory, NULL, &hit)); CHECK(hit == 0);
    CHECK(!memcmp(first, second, 64));
    source[1] ^= 0xff;
    CHECK(!nvn_bc_astc_encode(0x44, source, 64, 7, 5, 4, second, 64, directory, NULL, &hit)); CHECK(hit == 0);
    CHECK([[NSFileManager defaultManager] contentsOfDirectoryAtPath:root error:NULL].count == 2);
    CHECK(!nvn_bc_astc_encode(0x44, source, 64, 7, 5, 4, second, 64, "/dev/null/unwritable", NULL, &hit)); CHECK(hit == 0);
    CHECK(nvn_bc_astc_encode(0x44, source, 63, 7, 5, 4, second, 64, NULL, NULL, NULL) != 0);
    CHECK(nvn_bc_astc_encode(0x44, source, 64, 7, 5, 4, second, 63, NULL, NULL, NULL) != 0);
    CHECK(nvn_bc_astc_encode(0x44, source, 64, 7, 5, 4, source, 64, NULL, NULL, NULL) != 0);
    CHECK(nvn_bc_astc_encode(0x4f, source, 64, 7, 5, 4, second, 64, NULL, NULL, NULL) != 0);
    CHECK(nvn_bc_astc_encode(0x44, source, 64, 7, 5, 5, second, 64, NULL, NULL, NULL) != 0);
    CHECK([[NSFileManager defaultManager] removeItemAtPath:root error:NULL]); free(directory);
}

static void astc_sample_compare(id<MTLComputePipelineState> pipeline, uint32_t format) {
    void *memory;
    tex_t texture = fixture((nvn_texdesc){.target = 1, .format = format, .width = 16, .height = 8, .depth = 1, .levels = 1}, &memory);
    mtl_sub_begin(); g_nvn_bc_decode = 0;
    id<MTLTexture> native = pkg_upload(&texture, NULL, nil);
    g_nvn_bc_decode = 1; id<MTLTexture> astc = pkg_upload(&texture, NULL, nil);
    uint32_t format_expected = 0; nvn_bc_astc_format(format, g_nvn_bc_astc, &format_expected);
    CHECK(astc.pixelFormat == format_expected); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull));
    id<MTLBuffer> values = [g_dev newBufferWithLength:16 * 8 * 2 * 4 * sizeof(float) options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
    [encoder setComputePipelineState:pipeline]; [encoder setTexture:native atIndex:0]; [encoder setTexture:astc atIndex:1];
    [encoder setBuffer:values offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(16, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 4, 1)];
    [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted]; CHECK(!cb.error);
    const float *result = values.contents; double squared_error = 0; float maximum = 0;
    nvn_bc_format_info info; nvn_bc_format(format, &info);
    for (size_t i = 0; i < 16 * 8 * 4; i++) {
        float a = result[i * 2], b = result[i * 2 + 1];
        if (info.pixel_format == MTLPixelFormatRGBA8Unorm_sRGB && i % 4 < 3) { a = srgb_encode(a); b = srgb_encode(b); }
        CHECK(isfinite(a) && isfinite(b));
        float error = fabsf(a - b); squared_error += error * error; maximum = fmaxf(maximum, error);
    }
    double mse = squared_error / (16 * 8 * 4);
    // Random BC blocks are deliberately difficult. This catches wrong format,
    // channel order, and gross corruption; real-scene quality is checked visually.
    CHECK(mse < (g_nvn_bc_astc == 4 ? 0.025 : 0.08));
    printf("ASTC %ux%u format=%#x MSE=%.7f max=%.5f\n", g_nvn_bc_astc, g_nvn_bc_astc, format, mse, maximum);
    defer_obj_release(native); defer_obj_release(astc); release_fixture(&texture, memory);
}

static void astc_upload_shape(nvn_texdesc desc) {
    void *memory; tex_t texture = fixture(desc, &memory);
    mtl_sub_begin(); id<MTLTexture> astc = pkg_upload(&texture, NULL, nil);
    CHECK(astc != nil); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull));
    const unsigned block = g_nvn_bc_astc;
    for (unsigned level = 0; level < desc.levels; level++) {
        uint32_t row, rows, depth, bpb; uint64_t base; unsigned th, td;
        nvn_deswizzle_geom(&desc, level, 0, &row, &rows, &depth, &base, &th, &td, &bpb);
        uint32_t w = MAX((unsigned)astc.width >> level, 1u), h = MAX((unsigned)astc.height >> level, 1u);
        size_t bc_plane = (size_t)row * rows, bc_size = bc_plane * depth;
        size_t pitch = ((size_t)w + block - 1) / block * 16, encoded_rows = ((size_t)h + block - 1) / block;
        size_t size = pitch * encoded_rows, aligned_pitch = (pitch + 255) & ~(size_t)255, image = aligned_pitch * encoded_rows;
        uint8_t *linear = malloc(bc_size), *expected = malloc(size);
        for (unsigned layer = 0; layer < texture.L.layers; layer++) {
            fill_blocks(linear, bc_size, desc.format, level + layer * 17);
            id<MTLBuffer> readback = [g_dev newBufferWithLength:image * depth options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> encoder = [cb blitCommandEncoder];
            [encoder copyFromTexture:astc sourceSlice:layer sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, depth)
                           toBuffer:readback destinationOffset:0 destinationBytesPerRow:aligned_pitch destinationBytesPerImage:image * (depth > 1)];
            [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted]; CHECK(!cb.error);
            for (unsigned z = 0; z < depth; z++) {
                CHECK(!nvn_bc_astc_encode(desc.format, linear + z * bc_plane, bc_plane, w, h, block, expected, size, NULL, NULL, NULL));
                int equal = 1;
                for (size_t y = 0; y < encoded_rows; y++) equal &= !memcmp(expected + y * pitch, (uint8_t *)readback.contents + z * image + y * aligned_pitch, pitch);
                CHECK(equal);
            }
        }
        free(linear); free(expected);
    }
    defer_obj_release(astc); release_fixture(&texture, memory);
}
