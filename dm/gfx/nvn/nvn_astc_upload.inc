static int astc_direct_staging(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("DM_NVN_ASTC_DIRECT_STAGING");
        enabled = !value || strcmp(value, "0");
        MLOG("ASTC direct staging: %s", enabled ? "on" : "off");
    }
    return enabled;
}

static void astc_miss_trace(const tex_t *t, uint32_t width, uint32_t height,
        NSUInteger level, NSUInteger layer, uint32_t z, unsigned block, size_t bytes,
        int result, const nvn_astc_cache_report *report) {
    static FILE *file; static dispatch_once_t once; static _Atomic unsigned lines;
    dispatch_once(&once, ^{
        const char *path = getenv("DM_NVN_ASTC_MISS_TRACE"), *uploads = getenv("DM_NVN_UPLOAD_TRACE");
        char sibling[PATH_MAX];
        if ((!path || !*path) && uploads && *uploads) {
            int length = snprintf(sibling, sizeof sibling, "%s.misses.tsv", uploads);
            if (length > 0 && (size_t)length < sizeof sibling) path = sibling;
        }
        if (path && *path) {
            file = fopen(path, "w");
            if (file) {
                setvbuf(file, NULL, _IOLBF, 0);
                fprintf(file, "frame\tserial\tformat\tw\th\tmip\tlayer\tz\tblock\tbytes\tkey\treadonly\twritable\twrite_attempted\twrite_errno\tresult\n");
            } else MLOG("[RED] cannot open ASTC miss trace: %s", path);
        }
    });
    if (!file) return;
    unsigned line = atomic_fetch_add_explicit(&lines, 1, memory_order_relaxed);
    if (line >= 65536) {
        if (line == 65536) MLOG("ASTC miss trace reached its 65536-plane limit");
        return;
    }
    fprintf(file, "%llu\t%llu\t%u\t%u\t%u\t%lu\t%lu\t%u\t%u\t%zu\t%s\t%s\t%s\t%d\t%d\t%d\n",
        (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)t->serial, t->d.format,
        width, height, (unsigned long)level, (unsigned long)layer, z, block, bytes, report->key.hex,
        nvn_bc_astc_cache_status_name(report->readonly_status), nvn_bc_astc_cache_status_name(report->writable_status),
        report->write_attempted, report->write_error, result);
}

static id<MTLTexture> pkg_upload_astc_planes(const tex_t *t, id<MTLTexture> mt, nvn_bc_format_info info,
                                            NvPkg *pk, int restore, NvBcBlocks *initial) {
    const uint8_t *source = t->mem;
    uint8_t *gathered = NULL;
    if (t->pool && nvn_sparse_pool(t->pool)) {
        gathered = malloc(t->L.size ? t->L.size : 1);
        if (!gathered) MDIE("ASTC source gather allocation failed");
        UP_TIME(data_ns, nvn_sparse_copy(t->pool, gathered, (uint64_t)(uintptr_t)t->mem, t->L.size));
        source = gathered;
    }
    const unsigned block = g_nvn_bc_astc;
    NvBcBlocks *blocks = bc_blocks_get(mt);
    int source_read = 0;
    const char *cache = getenv("DM_NVN_ASTC_CACHE"), *readonly_cache = getenv("DM_NVN_ASTC_CACHE_RO");
    NSUInteger layers = mt.textureType == MTLTextureType3D ? 1 : mt.arrayLength * (mt.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    size_t key_count = 0, key_index = 0;
    for (NSUInteger level = 0; level < mt.mipmapLevelCount; level++)
        key_count += layers * (mt.textureType == MTLTextureType3D ? MAX(mt.depth >> level, 1u) : 1);
    if (pk && pkg_cache_initial_reloadable(t, pk->sparse)) pkg_astc_keys_prepare(pk, key_count);
    nvn_astc_cache_key *keys = pk && pk->astc_key_count == key_count ? pk->astc_keys : NULL;
    for (NSUInteger level = 0; level < mt.mipmapLevelCount; level++) {
        uint32_t width = MAX(mt.width >> level, 1u), height = MAX(mt.height >> level, 1u);
        uint32_t depth = mt.textureType == MTLTextureType3D ? MAX(mt.depth >> level, 1u) : 1;
        size_t bc_pitch = ((size_t)width + 3) / 4 * info.block_bytes;
        size_t bc_plane = bc_pitch * (((size_t)height + 3) / 4);
        size_t pitch = ((size_t)width + block - 1) / block * 16, rows = ((size_t)height + block - 1) / block;
        size_t plane = pitch * rows, aligned_pitch = (pitch + 255) & ~(size_t)255;
        size_t rows_per_chunk = MAX(UP_COMPRESSED_CHUNK / aligned_pitch, (size_t)1);
        int direct = pitch == aligned_pitch && plane <= UP_COMPRESSED_CHUNK && up_shared_enabled() && astc_direct_staging();
        uint8_t *linear = NULL, *scratch = direct ? NULL : malloc(plane);
        if (!direct && !scratch) MDIE("ASTC transcode staging allocation failed");
        for (NSUInteger layer = 0; layer < layers; layer++) {
            int source_ready = 0;
            for (uint32_t z = 0; z < depth; z++) {
                NSUInteger initial_offset = initial ? bc_blocks_offset(initial, level, layer, z) : 0;
                if (initial && ![initial->initial_planes containsIndex:initial_offset]) { key_index++; continue; }
                // Select a completed slot before writing any payload. Cache
                // reads and CPU encoding never submit this slice to the GPU.
                size_t direct_off = 0;
                id<MTLBuffer> direct_buffer = direct ? up_shared_alloc(plane, &direct_off) : nil;
                uint8_t *encoded = direct ? (uint8_t *)direct_buffer.contents + direct_off : scratch;
                int hit = 0;
                nvn_astc_cache_key *key = keys ? &keys[key_index] : NULL;
                if (restore && key)
                    UP_TIME(transcode_ns, hit = nvn_bc_astc_cache_read(key, encoded, plane, cache, readonly_cache));
                if (hit) atomic_fetch_add(&g_astc_direct_hits, 1);
                else {
                    if (!initial && !source_ready) {
                        if (!linear) linear = malloc(bc_plane * depth);
                        if (!linear) MDIE("ASTC source staging allocation failed");
                        int error;
                        UP_TIME(data_ns, error = nvn_deswizzle(&t->d, (int)level, (int)layer, source, linear, bc_pitch));
                        if (error) MDIE("ASTC source deswizzle failed");
                        if (blocks) memcpy((uint8_t *)bc_blocks_buffer(blocks).contents + bc_blocks_offset(blocks, level, layer, 0), linear, bc_plane * depth);
                        source_ready = 1;
                        source_read = 1;
                    }
                    const uint8_t *plane_source = initial ? (const uint8_t *)bc_blocks_buffer(initial).contents + initial_offset :
                        linear + (size_t)z * bc_plane;
                    int error; nvn_astc_cache_report report;
                    UP_TIME(transcode_ns, error = nvn_bc_astc_encode_report(t->d.format, plane_source, bc_plane, width, height, block,
                                           encoded, plane, cache, readonly_cache, &hit, key, &report));
                    if (!hit) astc_miss_trace(t, width, height, level, layer, z, block, plane, error, &report);
                    if (error) MDIE("ASTC transcode failed: format %#x %ux%u", t->d.format, width, height);
                }
                key_index++;
                if (hit) g_astc_hits++; else g_astc_misses++;
                g_astc_bytes += plane; g_bp_up_bytes += plane;
                if (direct) {
                    [g_up_texs addObject:mt];
                    up_blit_add(direct_buffer, direct_off, pitch, plane, MTLSizeMake(width, height, 1), mt, layer, level);
                    g_up_blits[g_nup_blits - 1].origin = MTLOriginMake(0, 0, z);
                    atomic_fetch_add(&g_astc_staging_direct_bytes, plane);
                    up_compressed_chunk_end();
                } else for (size_t y = 0; y < rows; y += rows_per_chunk) {
                    size_t count = MIN(rows_per_chunk, rows - y), bytes = aligned_pitch * count;
                    size_t staging_off;
                    id<MTLBuffer> staging = up_shared_alloc(bytes, &staging_off);
                    UP_TIME(data_ns, for (size_t r = 0; r < count; r++) memcpy((uint8_t *)staging.contents + staging_off + r * aligned_pitch, encoded + (y + r) * pitch, pitch));
                    [g_up_texs addObject:mt];
                    up_blit_add(staging, staging_off, aligned_pitch, bytes, MTLSizeMake(width, MIN(count * block, height - y * block), 1), mt, layer, level);
                    g_up_blits[g_nup_blits - 1].origin = MTLOriginMake(0, y * block, z);
                    atomic_fetch_add(&g_astc_staging_copied_bytes, pitch * count);
                    up_compressed_chunk_end();
                }
                if (initial) bc_blocks_initial_done(initial, initial_offset, 0);
            }
        }
        free(scratch); free(linear);
    }
    free(gathered);
    if (blocks && !initial) { blocks->gpu_written = 0; blocks->revision++; }
    up_compressed_texture_end();
    if (source_read && g_nvn_astc_source_cold && pk && !pk->sparse && !(t->d.flags & NVN_TEXTURE_FLAGS_IMAGE)) {
        // All GPU work now owns independent staging. Prefer reclaiming this
        // duplicate source over a recently uploaded texture or hot guest data.
        size_t advised;
        UP_TIME(cold_ns, advised = dm_guest_memory_cold(t->mem, t->L.size));
        atomic_fetch_add_explicit(&g_astc_cold_bytes, advised, memory_order_relaxed);
    }
    mt.label = @"dm.bc-astc";
    return mt;
}

static id<MTLTexture> pkg_upload_astc(const tex_t *t, id<MTLTexture> mt, nvn_bc_format_info info, NvPkg *pk, int restore) {
    return pkg_upload_astc_planes(t, mt, info, pk, restore, nil);
}
static void bc_blocks_initial_sample(const tex_t *t, id<MTLTexture> mt) {
    NvBcBlocks *blocks = bc_blocks_get(mt);
    if (!blocks || !blocks->initial_planes.count) return;
    nvn_bc_format_info info;
    if (!nvn_bc_format(t->d.format, &info)) MDIE("deferred BC texture lost its encoded format");
    // Only untouched planes remain pending. Their encoded bytes were copied
    // before any GPU writes, so no replay submission needs to be waited on.
    pkg_upload_astc_planes(t, mt, info, nil, 0, blocks);
}
