// Encoded blocks are authoritative for copies and raw views. The host texture
// is a derived sampling representation; never copy BC bytes into ASTC or RGBA.
static void clear_tex_tri(id<MTLTexture> t, unsigned lv, unsigned l0, const p_clear_tex *c);
static _Atomic uint64_t g_bc_copy_planes, g_bc_copy_total_ns, g_bc_copy_wait_ns;
static _Atomic uint64_t g_bc_copy_convert_ns, g_bc_copy_upload_ns, g_bc_copy_syncs;
void mtl_bc_copy_timing_snapshot(mtl_bc_copy_timing *out) {
    out->planes = atomic_load_explicit(&g_bc_copy_planes, memory_order_relaxed);
    out->total_ns = atomic_load_explicit(&g_bc_copy_total_ns, memory_order_relaxed);
    out->wait_ns = atomic_load_explicit(&g_bc_copy_wait_ns, memory_order_relaxed);
    out->convert_ns = atomic_load_explicit(&g_bc_copy_convert_ns, memory_order_relaxed);
    out->upload_ns = atomic_load_explicit(&g_bc_copy_upload_ns, memory_order_relaxed);
    out->syncs = atomic_load_explicit(&g_bc_copy_syncs, memory_order_relaxed);
}
static uint64_t bc_copy_wait(void) {
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush(); res_commit();
    id<MTLCommandBuffer> cb = g_cb;
    if (!cb) MDIE("BC conversion requires a replay submission");
    gpu_tag(cb);
    uint32_t segment = g_seg++;
    seg_clk *clock = NULL;
    if (seg_ts()) {
        if (!g_sclk) { g_sclk = calloc(1, sizeof *g_sclk); atomic_store(&g_sclk->refs, 1); }
        clock = g_sclk; atomic_fetch_add(&clock->refs, 1);
    }
    NvCommitOptions *options = [NvCommitOptions new];
    [options addFeedbackHandler:^(id<MTLCommandBuffer> done) {
        if (clock) {
            if (segment < SEG_CLK_N) {
                atomic_store(&clock->t[segment][0], gpu_s_guest_ns(done.GPUStartTime));
                atomic_store(&clock->t[segment][1], gpu_s_guest_ns(done.GPUEndTime));
            }
            seg_clk_put(clock);
        }
    }];
    replay_commit(cb, options, nil, 0);
    uint64_t waiting = now_ns();
    [cb waitUntilCompleted];
    uint64_t waited = now_ns() - waiting;
    atomic_fetch_add_explicit(&g_bc_copy_syncs, 1, memory_order_relaxed);
    if (cb.error) MDIE("BC copy source GPU work failed: %s", cb.error.description.UTF8String);
    g_cb = command_buffer(g_replay_q);
    return waited;
}

static void bc_blocks_materialize_ready(id<MTLTexture> texture, NvBcBlocks *blocks,
                                        NSUInteger level, NSUInteger layer, NSUInteger z, int source_ready) {
    nvn_bc_format_info info; nvn_bc_format(blocks->format, &info);
    NSUInteger width = mip_extent(texture.width, level), height = mip_extent(texture.height, level);
    NSUInteger offset = bc_blocks_offset(blocks, level, layer, z);
    bc_blocks_initial_done(blocks, offset, 1);
    blocks->gpu_written = 1;
    blocks->revision++;
    g_ser_gen++;
    if (bc_blocks_gpu_sample(blocks, texture)) {
        size_t row = (width * info.pixel_bytes + 255) & ~(size_t)255;
        id<MTLBuffer> pixels = [g_dev newBufferWithLength:row * height options:MTLResourceStorageModePrivate];
        if (!pixels) MDIE("cannot allocate BC copy decode staging");
        bc_blocks_decode(blocks, offset, width, height, pixels, row);
        enc_b_copy_buffer_texture(blit(), pixels, 0, row, row * height,
            MTLSizeMake(width, height, 1), texture, layer, level, MTLOriginMake(0, 0, z));
        return;
    }
    // A source buffer may have been written earlier in this same submission.
    // Complete that segment before CPU decoding, then upload before later draws.
    // Convert the whole selected plane: BC4x4 and ASTC6x6 boundaries differ.
    uint64_t copy_started = now_ns(), waited = source_ready ? 0 : bc_copy_wait();
    unsigned block = 1; uint32_t astc_format = 0;
    int astc = nvn_bc_astc_format(blocks->format, g_nvn_bc_astc, &astc_format) &&
        texture.pixelFormat == (MTLPixelFormat)astc_format;
    if (astc) block = g_nvn_bc_astc;
    size_t tight_row = astc ? ((width + block - 1) / block) * 16 : width * info.pixel_bytes;
    size_t rows = (height + block - 1) / block, bytes = tight_row * rows;
    uint8_t *converted = malloc(bytes);
    if (!converted) MDIE("cannot allocate BC copy conversion plane");
    const uint8_t *source = (const uint8_t *)bc_blocks_buffer(blocks).contents + offset;
    int error;
    uint64_t converting = now_ns();
    if (astc) {
        int hit; nvn_astc_cache_report report;
        uint64_t started = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        error = nvn_bc_astc_encode_report(blocks->format, source, blocks->planes[level], width, height,
            block, converted, bytes, getenv("DM_NVN_ASTC_CACHE"), getenv("DM_NVN_ASTC_CACHE_RO"), &hit, NULL, &report);
        uint64_t elapsed = clock_gettime_nsec_np(CLOCK_MONOTONIC) - started;
        g_bc_astc_copy_ns += elapsed; g_bc_astc_copy_max_ns = MAX(g_bc_astc_copy_max_ns, elapsed);
        if (hit) g_bc_astc_copy_hits++;
        else {
            g_bc_astc_copy_misses++;
            tex_t trace = {.serial = blocks->serial, .d = {.format = blocks->format}};
            astc_miss_trace(&trace, width, height, level, layer, z, block, bytes, error, &report);
        }
    } else error = nvn_bc_decode(blocks->format, source, blocks->planes[level], blocks->rows[level], width, height,
        converted, bytes, tight_row);
    uint64_t converted_ns = now_ns() - converting;
    if (error) MDIE("BC copy conversion failed for format %#x %lux%lu", blocks->format, (unsigned long)width, (unsigned long)height);
    uint64_t uploading = now_ns();
    size_t row = (tight_row + 255) & ~(size_t)255;
    size_t chunk_rows = MAX(UP_COMPRESSED_CHUNK / row, (size_t)1);
    for (size_t y = 0; y < rows; y += chunk_rows) {
        size_t count = MIN(chunk_rows, rows - y), staging_offset;
        id<MTLBuffer> staging = up_shared_alloc(row * count, &staging_offset);
        for (size_t r = 0; r < count; r++)
            memcpy((uint8_t *)staging.contents + staging_offset + r * row, converted + (y + r) * tight_row, tight_row);
        [g_up_texs addObject:texture];
        up_blit_add(staging, staging_offset, row, row * count,
            MTLSizeMake(width, MIN(count * block, height - y * block), 1), texture, layer, level);
        g_up_blits[g_nup_blits - 1].origin = MTLOriginMake(0, y * block, z);
        up_compressed_chunk_end();
    }
    free(converted);
    up_flush();
    uint64_t finished = now_ns();
    atomic_fetch_add_explicit(&g_bc_copy_wait_ns, waited, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_bc_copy_convert_ns, converted_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_bc_copy_upload_ns, finished - uploading, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_bc_copy_total_ns, finished - copy_started, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_bc_copy_planes, 1, memory_order_relaxed);
}

static void bc_blocks_materialize(id<MTLTexture> texture, NvBcBlocks *blocks,
                                  NSUInteger level, NSUInteger layer, NSUInteger z) {
    bc_blocks_materialize_ready(texture, blocks, level, layer, z, 0);
}

// Consecutive copies to one allocation have no sampling consumer between them.
// Finish their block writes together, then derive each plane from its final bytes.
// Other operations and allocation changes end the batch before they can observe it.
typedef struct {
    id<MTLTexture> texture;
    NvBcBlocks *blocks;
    NSUInteger level, layer, z;
} bc_copy_pending;
enum { BC_COPY_BATCH_CAP = 64 };
static bc_copy_pending g_bc_copy_pending[BC_COPY_BATCH_CAP];
static unsigned g_bc_copy_pending_n;
static int g_bc_copy_defer;

static int bc_copy_batch_on(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("DM_NVN_BC_COPY_BATCH");
        enabled = !value || strcmp(value, "0");
    }
    return enabled;
}

static void bc_copy_flush(void) {
    unsigned count = g_bc_copy_pending_n;
    if (!count) return;
    uint64_t start = now_ns(), waited = bc_copy_wait();
    atomic_fetch_add_explicit(&g_bc_copy_wait_ns, waited, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_bc_copy_total_ns, now_ns() - start, memory_order_relaxed);
    g_bc_copy_pending_n = 0;
    for (unsigned i = 0; i < count; i++) {
        bc_copy_pending item = g_bc_copy_pending[i];
        g_bc_copy_pending[i] = (bc_copy_pending){0};
        bc_blocks_materialize_ready(item.texture, item.blocks, item.level, item.layer, item.z, 1);
    }
}

static void bc_copy_prepare(id<MTLTexture> texture) {
    // A different allocation can alias the same encoded storage. Complete the
    // old representation before the new copy is encoded, not after its write.
    if (g_bc_copy_pending_n && (!g_bc_copy_defer || g_bc_copy_pending[0].texture != texture))
        bc_copy_flush();
}

static void bc_copy_defer_plane(id<MTLTexture> texture, NvBcBlocks *blocks,
                                 NSUInteger level, NSUInteger layer, NSUInteger z) {
    for (unsigned i = 0; i < g_bc_copy_pending_n; i++) {
        const bc_copy_pending *item = &g_bc_copy_pending[i];
        if (item->texture == texture && item->blocks == blocks && item->level == level &&
            item->layer == layer && item->z == z) return;
    }
    if (g_bc_copy_pending_n == BC_COPY_BATCH_CAP) bc_copy_flush();
    g_bc_copy_pending[g_bc_copy_pending_n++] = (bc_copy_pending){texture, blocks, level, layer, z};
}

static int bc_copy_batch_keeps(unsigned op, const void *payload) {
    if (op == OP_COPY_STRIDE) return 1;
    return op == OP_COPY_B2T && tex_bc_decoded(((const p_copy_b2t *)payload)->tex);
}

static nvn_bc_region bc_copy_region(const tex_t *texture, id<MTLTexture> host, const view_t *view,
                                     int has_view, const NVNcopyRegion *region, size_t row, size_t image) {
    unsigned level = has_view && view->set_levels ? view->base_level : 0;
    unsigned layer = has_view && view->set_layers ? view->min_layer : 0;
    if (level >= host.mipmapLevelCount) MDIE("BC copy mip level out of bounds");
    unsigned width = mip_extent(host.width, level), height = mip_extent(host.height, level);
    unsigned slices = host.textureType == MTLTextureType3D ? mip_extent(host.depth, level) :
        host.arrayLength * (host.textureType == MTLTextureTypeCube || host.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    if (layer >= slices) MDIE("BC copy base layer out of bounds");
    NVNcopyRegion r = *region;
    if (has_view && view->set_format && !nvn_bc_format(view->format, NULL)) {
        int bw, bh; nvn_bc_format_info info; nvn_bc_format(texture->d.format, &info);
        if (nvn_fmt_bpb(view->format, &bw, &bh) != info.block_bytes || bw != 1 || bh != 1 ||
            r.xoffset < 0 || r.yoffset < 0 || r.width <= 0 || r.height <= 0 ||
            (unsigned)r.xoffset >= (width + 3) / 4 || (unsigned)r.yoffset >= (height + 3) / 4 ||
            (unsigned)r.width > (width + 3) / 4 - (unsigned)r.xoffset ||
            (unsigned)r.height > (height + 3) / 4 - (unsigned)r.yoffset)
            MDIE("BC copy requires a compatible block-format view");
        r.xoffset *= 4; r.yoffset *= 4;
        r.width = MIN((unsigned)r.width * 4, width - r.xoffset);
        r.height = MIN((unsigned)r.height * 4, height - r.yoffset);
    }
    nvn_bc_region result;
    if (nvn_bc_region_init(&result, texture->d.format, width, height, slices - layer,
        r.xoffset, r.yoffset, r.zoffset, r.width, r.height, r.depth, row, image))
        MDIE("invalid BC copy region format %#x level %u xyz=%d,%d,%d size=%d,%d,%d row=%zu image=%zu",
            texture->d.format, level, r.xoffset, r.yoffset, r.zoffset, r.width, r.height, r.depth, row, image);
    return result;
}

static NSUInteger bc_copy_offset(NvBcBlocks *blocks, unsigned level, unsigned base_layer,
                                 int is3d, const nvn_bc_region *region, unsigned z) {
    unsigned slice = base_layer + region->z + z;
    return bc_blocks_offset(blocks, level, is3d ? 0 : slice, is3d ? slice : 0) +
        region->y_blocks * blocks->rows[level] + region->x_bytes;
}

static void bc_copy_buffer(const p_copy_b2t *copy, id<MTLTexture> texture, int to_texture) {
    bc_copy_prepare(texture);
    rp_flush();
    unsigned level = copy->has_view && copy->view.set_levels ? copy->view.base_level : 0;
    unsigned layer = copy->has_view && copy->view.set_layers ? copy->view.min_layer : 0;
    nvn_bc_region region = bc_copy_region(copy->tex, texture, &copy->view, copy->has_view,
        &copy->r, S.copy_stride, S.copy_img_stride);
    void *storage; uint64_t offset;
    if (!mtl_range_addr(copy->src, region.span, &storage, &offset)) MDIE("BC copy buffer is outside a memory pool");
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)storage;
    NvBcBlocks *blocks = bc_blocks_ensure(copy->tex, texture);
    int is3d = texture.textureType == MTLTextureType3D;
    id<MTLBlitCommandEncoder> encoder = blit();
    for (unsigned z = 0; z < region.depth; z++) {
        NSUInteger block_offset = bc_copy_offset(blocks, level, layer, is3d, &region, z);
        for (size_t y = 0; y < region.rows; y++) {
            NSUInteger buffer_offset = offset + z * region.image_stride + y * region.row_stride;
            NSUInteger destination = block_offset + y * blocks->rows[level];
            if (to_texture) enc_b_copy_buffer(encoder, buffer, buffer_offset, bc_blocks_buffer(blocks), destination, region.row_bytes);
            else enc_b_copy_buffer(encoder, bc_blocks_buffer(blocks), destination, buffer, buffer_offset, region.row_bytes);
        }
    }
    if (to_texture) {
        blocks->gpu_written = 1;
        for (unsigned z = 0; z < region.depth; z++) {
            unsigned slice = layer + region.z + z;
            if (g_bc_copy_defer && !bc_blocks_gpu_sample(blocks, texture))
                bc_copy_defer_plane(texture, blocks, level, is3d ? 0 : slice, is3d ? slice : 0);
            else bc_blocks_materialize(texture, blocks, level, is3d ? 0 : slice, is3d ? slice : 0);
        }
    } else { g_wr_epoch++; sbw_note(copy->src, region.span, SBW_GPU); }
    static unsigned logged;
    if (logged++ < 8) MLOG("BC %s copy: format %#x level %u region %dx%dx%d row %zu image %zu",
        to_texture ? "buffer-to-texture" : "texture-to-buffer", copy->tex->d.format, level,
        copy->r.width, copy->r.height, copy->r.depth, region.row_stride, region.image_stride);
}

static void bc_copy_texture(const p_copy_t2t *copy, id<MTLTexture> source, id<MTLTexture> destination) {
    if (!tex_bc_decoded(copy->src) || !tex_bc_decoded(copy->dst))
        MDIE("BC texture copy requires compatible encoded storage on both sides");
    if (copy->flags & ~(NVN_COPY_FLAGS_ENGINE_2D_BIT | NVN_COPY_FLAGS_LINEAR_FILTER_BIT))
        MDIE("BC block copies do not support mirroring or multisample resolve");
    unsigned sl = copy->has_sv && copy->sv.set_levels ? copy->sv.base_level : 0;
    unsigned dl = copy->has_dv && copy->dv.set_levels ? copy->dv.base_level : 0;
    unsigned sa = copy->has_sv && copy->sv.set_layers ? copy->sv.min_layer : 0;
    unsigned da = copy->has_dv && copy->dv.set_layers ? copy->dv.min_layer : 0;
    nvn_bc_region sr = bc_copy_region(copy->src, source, &copy->sv, copy->has_sv, &copy->sr, 0, 0);
    nvn_bc_region dr = bc_copy_region(copy->dst, destination, &copy->dv, copy->has_dv, &copy->dr, 0, 0);
    nvn_bc_format_info si, di; nvn_bc_format(copy->src->d.format, &si); nvn_bc_format(copy->dst->d.format, &di);
    if (si.block_bytes != di.block_bytes || sr.row_bytes != dr.row_bytes || sr.rows != dr.rows || sr.depth != dr.depth)
        MDIE("BC texture copy block extents differ");
    NvBcBlocks *sb = bc_blocks_ensure(copy->src, source), *db = bc_blocks_ensure(copy->dst, destination);
    int s3 = source.textureType == MTLTextureType3D, d3 = destination.textureType == MTLTextureType3D;
    // Snapshot first, including self/alias copies whose regions overlap.
    id<MTLBuffer> snapshot = [g_dev newBufferWithLength:sr.span options:MTLResourceStorageModePrivate];
    if (!snapshot) MDIE("cannot allocate BC texture copy snapshot");
    id<MTLBlitCommandEncoder> encoder = blit();
    for (unsigned z = 0; z < sr.depth; z++) for (size_t y = 0; y < sr.rows; y++)
        enc_b_copy_buffer(encoder, bc_blocks_buffer(sb), bc_copy_offset(sb, sl, sa, s3, &sr, z) + y * sb->rows[sl],
            snapshot, z * sr.image_stride + y * sr.row_stride, sr.row_bytes);
    for (unsigned z = 0; z < dr.depth; z++) for (size_t y = 0; y < dr.rows; y++)
        enc_b_copy_buffer(encoder, snapshot, z * sr.image_stride + y * sr.row_stride,
            bc_blocks_buffer(db), bc_copy_offset(db, dl, da, d3, &dr, z) + y * db->rows[dl], dr.row_bytes);
    db->gpu_written = 1;
    for (unsigned z = 0; z < dr.depth; z++) {
        unsigned slice = da + dr.z + z;
        bc_blocks_materialize(destination, db, dl, d3 ? 0 : slice, d3 ? slice : 0);
    }
}

static int bc_copy_texture_mixed(const p_copy_t2t *copy, id<MTLTexture> source, id<MTLTexture> destination) {
    int to_bc = tex_bc_decoded(copy->dst);
    const tex_t *encoded = to_bc ? copy->dst : copy->src, *plain = to_bc ? copy->src : copy->dst;
    const view_t *view = to_bc ? &copy->dv : &copy->sv, *other_view = to_bc ? &copy->sv : &copy->dv;
    int has_view = to_bc ? copy->has_dv : copy->has_sv, has_other = to_bc ? copy->has_sv : copy->has_dv;
    const NVNcopyRegion *region = to_bc ? &copy->dr : &copy->sr, *other = to_bc ? &copy->sr : &copy->dr;
    id<MTLTexture> host = to_bc ? destination : source, other_host = to_bc ? source : destination;
    nvn_bc_format_info info; nvn_bc_format(encoded->d.format, &info);
    int bw, bh, bytes = nvn_fmt_bpb(has_other && other_view->set_format ? other_view->format : plain->d.format, &bw, &bh);
    if (bytes != info.block_bytes || bw != 1 || bh != 1) return 0;
    // An equally sized pixel format can also be a regular color conversion.
    // Select raw-block copies only when both regions describe the same blocks.
    int raw_view = has_view && view->set_format && !nvn_bc_format(view->format, NULL);
    if (region->width <= 0 || region->height <= 0 ||
        (raw_view ? region->width : (region->width + 3ull) / 4) != (uint64_t)other->width ||
        (raw_view ? region->height : (region->height + 3ull) / 4) != (uint64_t)other->height ||
        (region->depth ? region->depth : 1) != (other->depth ? other->depth : 1)) return 0;
    if (copy->flags & ~(NVN_COPY_FLAGS_ENGINE_2D_BIT | NVN_COPY_FLAGS_LINEAR_FILTER_BIT))
        MDIE("BC mixed block copy does not support mirrored regions");
    nvn_bc_region r = bc_copy_region(encoded, host, view, has_view, region, 0, 0);
    unsigned level = has_view && view->set_levels ? view->base_level : 0;
    unsigned layer = has_view && view->set_layers ? view->min_layer : 0;
    unsigned other_level = has_other && other_view->set_levels ? other_view->base_level : 0;
    unsigned other_layer = has_other && other_view->set_layers ? other_view->min_layer : 0;
    if (other->width <= 0 || other->height <= 0 || other->depth < 0 || other->xoffset < 0 || other->yoffset < 0 || other->zoffset < 0 ||
        other_level >= other_host.mipmapLevelCount || r.row_bytes != (size_t)other->width * bytes || r.rows != (size_t)other->height ||
        r.depth != (unsigned)(other->depth ? other->depth : 1) ||
        (NSUInteger)other->xoffset + other->width > mip_extent(other_host.width, other_level) ||
        (NSUInteger)other->yoffset + other->height > mip_extent(other_host.height, other_level))
        MDIE("BC mixed copy block extents differ or exceed the view");
    int is3d = host.textureType == MTLTextureType3D, other3d = other_host.textureType == MTLTextureType3D;
    NSUInteger other_slices = other3d ? mip_extent(other_host.depth, other_level) : other_host.arrayLength *
        (other_host.textureType == MTLTextureTypeCube || other_host.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    if ((NSUInteger)other_layer + other->zoffset + r.depth > other_slices) MDIE("BC mixed copy layers out of bounds");
    NvBcBlocks *blocks = bc_blocks_ensure(encoded, host);
    id<MTLBlitCommandEncoder> encoder = blit();
    for (unsigned z = 0; z < r.depth; z++) {
        NSUInteger offset = bc_copy_offset(blocks, level, layer, is3d, &r, z), slice = other_layer + other->zoffset + z;
        MTLOrigin origin = MTLOriginMake(other->xoffset, other->yoffset, other3d ? slice : 0);
        MTLSize size = MTLSizeMake(other->width, other->height, 1);
        if (to_bc) enc_b_copy_texture_buffer(encoder, other_host, other3d ? 0 : slice, other_level, origin, size,
            bc_blocks_buffer(blocks), offset, blocks->rows[level], blocks->planes[level]);
        else enc_b_copy_buffer_texture(encoder, bc_blocks_buffer(blocks), offset, blocks->rows[level], blocks->planes[level],
            size, other_host, other3d ? 0 : slice, other_level, origin);
    }
    if (to_bc) for (unsigned z = 0; z < r.depth; z++) {
        unsigned slice = layer + r.z + z;
        bc_blocks_materialize(host, blocks, level, is3d ? 0 : slice, is3d ? slice : 0);
    }
    return 1;
}

static id<MTLTexture> bc_raw_view(const tex_t *texture, id<MTLTexture> base,
                                 const view_t *view, const uint8_t swizzle[4]) {
    NvBcBlocks *blocks = bc_blocks_ensure(texture, base);
    MTLPixelFormat format;
    if (!bcrt_view(base, view, 1, &format)) MDIE("BC raw view has an incompatible block size");
    NSUInteger level = view->set_levels ? view->base_level : 0;
    NSUInteger layer = view->set_layers ? view->min_layer : 0;
    int is3d = base.textureType == MTLTextureType3D;
    if (level >= blocks->levels || (!is3d && layer >= blocks->layers) || (is3d && layer))
        MDIE("BC raw view subresource out of bounds");
    NSUInteger levels = view->set_levels && view->nlevels ? view->nlevels : blocks->levels - level;
    NSUInteger layers = is3d ? 1 : view->set_layers && view->nlayers ? view->nlayers : blocks->layers - layer;
    if (!levels || levels > blocks->levels - level || !layers || layers > blocks->layers - layer)
        MDIE("BC raw view extent out of bounds");
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.pixelFormat = format;
    descriptor.textureType = view->set_target ? ttype(view->target, base.textureType) : base.textureType;
    if (descriptor.textureType == MTLTextureType2D) layers = 1;
    descriptor.width = (mip_extent(base.width, level) + 3) / 4;
    descriptor.height = (mip_extent(base.height, level) + 3) / 4;
    descriptor.depth = is3d ? mip_extent(base.depth, level) : 1;
    descriptor.arrayLength = is3d || descriptor.textureType == MTLTextureType2D ? 1 :
        descriptor.textureType == MTLTextureTypeCubeArray ? layers / 6 : layers;
    if (descriptor.textureType == MTLTextureTypeCube) descriptor.arrayLength = 1;
    descriptor.mipmapLevelCount = levels;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    for (NSUInteger l = 0; l < levels; l++) {
        if (mip_extent(descriptor.width, l) != (mip_extent(base.width, level + l) + 3) / 4 ||
            mip_extent(descriptor.height, l) != (mip_extent(base.height, level + l) + 3) / 4)
            MDIE("BC raw view mip dimensions cannot be represented by one Metal texture; select individual mip levels");
    }
    id<MTLTexture> raw = [g_dev newTextureWithDescriptor:descriptor];
    if (!raw) MDIE("cannot create BC raw-block texture view");
    id<MTLBlitCommandEncoder> encoder = blit();
    for (NSUInteger l = 0; l < levels; l++) for (NSUInteger a = 0; a < layers; a++) {
        NSUInteger current = level + l, depth = is3d ? blocks->depths[current] : 1;
        for (NSUInteger z = 0; z < depth; z++)
            enc_b_copy_buffer_texture(encoder, bc_blocks_buffer(blocks), bc_blocks_offset(blocks, current, layer + a, z),
                blocks->rows[current], blocks->planes[current],
                MTLSizeMake((mip_extent(base.width, current) + 3) / 4, (mip_extent(base.height, current) + 3) / 4, 1),
                raw, a, l, MTLOriginMake(0, 0, z));
    }
    NvBcRawView *metadata = [NvBcRawView new];
    metadata->base = base; metadata->raw = raw; metadata->blocks = blocks;
    metadata->level = level; metadata->layer = layer; metadata->level_count = levels; metadata->layer_count = layers;
    metadata->revision = blocks->revision;
    objc_setAssociatedObject(raw, &g_bc_raw_key, metadata, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    id<MTLTexture> result = [raw newTextureViewWithPixelFormat:format textureType:raw.textureType
        levels:NSMakeRange(0, levels) slices:NSMakeRange(0, is3d ? 1 : layers)
        swizzle:MTLTextureSwizzleChannelsMake(swizzle[0], swizzle[1], swizzle[2], swizzle[3])];
    if (!result) MDIE("cannot create BC raw-block swizzle view");
    objc_setAssociatedObject(result, &g_bc_raw_key, metadata, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return result;
}

static void bc_raw_flush(void) {
    if (!g_bc_raw_pending.count) return;
    NSArray<id<MTLTexture>> *pending = [g_bc_raw_pending copy];
    [g_bc_raw_pending removeAllObjects];
    for (id<MTLTexture> raw in pending) {
        NvBcRawView *view = objc_getAssociatedObject(raw, &g_bc_raw_key);
        NvBcBlocks *blocks = view->blocks;
        int is3d = view->base.textureType == MTLTextureType3D;
        id<MTLBlitCommandEncoder> encoder = blit();
        for (NSUInteger l = 0; l < view->level_count; l++) for (NSUInteger a = 0; a < view->layer_count; a++) {
            NSUInteger level = view->level + l, layer = view->layer + a;
            NSUInteger depth = is3d ? blocks->depths[level] : 1;
            for (NSUInteger z = 0; z < depth; z++)
                enc_b_copy_texture_buffer(encoder, raw, a, l, MTLOriginMake(0, 0, z),
                    MTLSizeMake(mip_extent(raw.width, l), mip_extent(raw.height, l), 1),
                    bc_blocks_buffer(blocks), bc_blocks_offset(blocks, level, layer, z), blocks->rows[level], blocks->planes[level]);
        }
        for (NSUInteger l = 0; l < view->level_count; l++) for (NSUInteger a = 0; a < view->layer_count; a++) {
            NSUInteger level = view->level + l, depth = is3d ? blocks->depths[level] : 1;
            for (NSUInteger z = 0; z < depth; z++)
                bc_blocks_materialize(view->base, blocks, level, view->layer + a, z);
        }
    }
}

static void bc_clear_texture(const p_clear_tex *copy, id<MTLTexture> texture) {
    if (!(copy->mask & 15)) return;
    if (!copy->has_view || !copy->view.set_format || nvn_bc_format(copy->view.format, NULL))
        MDIE("BC clears require an uncompressed block-format view");
    rp_flush();
    NvBcBlocks *blocks = bc_blocks_ensure(copy->tex, texture);
    (void)blocks;
    nvn_bc_region region = bc_copy_region(copy->tex, texture, &copy->view, 1, &copy->r, 0, 0);
    unsigned level = copy->view.set_levels ? copy->view.base_level : 0;
    unsigned layer = copy->view.set_layers ? copy->view.min_layer : 0;
    for (unsigned z = 0; z < region.depth; z++) {
        MTLRenderPassColorAttachmentDescriptor *attachment = [MTLRenderPassColorAttachmentDescriptor new];
        attachment.texture = texture; attachment.level = level;
        if (texture.textureType == MTLTextureType3D) attachment.depthPlane = layer + region.z + z;
        else attachment.slice = layer + region.z + z;
        if (!bcrt_attach(attachment, texture, &copy->view, 1)) MDIE("cannot attach BC clear block view");
        p_clear_tex clear = *copy; clear.has_view = 0; clear.r.zoffset = 0; clear.r.depth = 1;
        clear_tex_tri(attachment.texture, 0, 0, &clear);
        bcrt_flush();
    }
}
