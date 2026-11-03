// Publish query results before their next GPU consumer, without a CPU readback.
static id<MTLBuffer> g_vis_total;
static size_t g_vis_total_offset, g_vis_total_used;
int g_nvn_vis_reset_inline = 1;
static id<MTLBuffer> vis_total(void) {
    if (!g_vis_total) {
        g_vis_total = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        if (!g_vis_total) MDIE("visibility accumulator allocation failed");
        g_vis_total_offset = 0; g_vis_total_used = 8;
        *(uint64_t *)g_vis_total.contents = 0;
        res_add(g_vis_total);
    }
    return g_vis_total;
}
static uint64_t vis_total_gpu(void) { return buffer_gpu_address(vis_total()) + g_vis_total_offset; }
static void vis_reset(void) {
    static int initialized;
    if (!initialized) {
        const char *value = getenv("DM_NVN_VIS_RESET_INLINE");
        if (value) g_nvn_vis_reset_inline = strcmp(value, "0") != 0;
        initialized = 1;
    }
    vis_clear();
    id<MTLBuffer> total = vis_total();
    if (!g_nvn_vis_reset_inline) {
        [blit_buf(0, 0, 0, 0) fillBuffer:total range:NSMakeRange(g_vis_total_offset, 8) value:0];
        return;
    }
    // Each reset owns a new word. Earlier GPU work retains its old accumulator,
    // so resetting does not need a blit encoder or a CPU wait. A word is never
    // recycled; retired blocks stay resident until this submission completes.
    if (g_vis_total_used + 8 > total.length) {
        defer_obj_release(total); g_vis_total = nil;
        vis_total();
        return;
    }
    g_vis_total_offset = g_vis_total_used; g_vis_total_used += 8;
    *(uint64_t *)((uint8_t *)total.contents + g_vis_total_offset) = 0;
}
typedef struct { uint64_t slots, total, output, count, publish, ticks; } vis_gpu_job;
typedef struct { vis_gpu_job gpu; uint64_t destination; void *total_buffer, *output_buffer; } vis_pending_job;
static vis_pending_job *g_vis_pending; static int g_vis_pending_cap;
static void **g_vis_inputs; static int g_vis_input_n, g_vis_input_cap;
static void vis_take_input(void *buffer) {
    if (g_vis_input_n == g_vis_input_cap) {
        int capacity = g_vis_input_cap ? 2 * g_vis_input_cap : 64;
        void **inputs = realloc(g_vis_inputs, (size_t)capacity * sizeof *inputs);
        if (!inputs) MDIE("visibility input list allocation failed");
        g_vis_inputs = inputs; g_vis_input_cap = capacity;
    }
    g_vis_inputs[g_vis_input_n++] = buffer;
}
static int vis_batch_on(void) {
    static int initialized;
    if (!initialized) {
        const char *value = getenv("DM_NVN_QUERY_BATCH");
        if (value) g_nvn_query_batch = strcmp(value, "0") != 0;
        initialized = 1;
    }
    return g_nvn_query_batch;
}
static int pass_batch_on(void) {
    static int initialized;
    if (!initialized) {
        const char *value = getenv("DM_NVN_PASS_BATCH");
        if (value) g_nvn_pass_batch = strcmp(value, "0") != 0;
        initialized = 1;
    }
    return g_nvn_pass_batch;
}
static int vis_textures_on(void) {
    static int initialized;
    if (!initialized) {
        const char *value = getenv("DM_NVN_QUERY_TEXTURES");
        if (value) g_nvn_query_textures = atoi(value) <= 0 ? 0 : atoi(value) >= 2 ? 2 : 1;
        initialized = 1;
    }
    return g_nvn_query_textures;
}
static int vis_pending_overlap(uint64_t address, uint64_t size) {
    if (!address || !g_vis_pending_n) return 0;
    if (!size || address > UINT64_MAX - size) return 1;
    for (int i = 0; i < g_vis_pending_n; i++) {
        uint64_t destination = g_vis_pending[i].destination;
        if (destination && address < destination + 16 && destination < address + size) return 1;
    }
    return 0;
}
static int vis_destination_is_attachment(uint64_t destination) {
    if (!destination) return 0;
    for (int i = 0; i <= 8; i++) {
        const tex_t *texture = i == 8 ? R.rt.depth : i < R.rt.n ? R.rt.color[i] : NULL;
        if (!texture || !texture->mem) continue;
        uint64_t address = (uint64_t)(uintptr_t)texture->mem, size = texture->L.size;
        if (!size || address > UINT64_MAX - size || destination > UINT64_MAX - 16 ||
            (destination < address + size && address < destination + 16)) return 1;
    }
    return 0;
}
static int vis_texture_safe(int stage, unsigned slot) {
    if ((!vis_textures_on() && !pass_batch_on() && !discard_inline_on()) || slot >= NTEX) return 0;
    if (!g_vis_pending_n && !pass_batch_on() && !discard_inline_on()) return 1;
    unsigned texture_id = (unsigned)(S.tex[stage][slot] & 0xfffff);
    if (!texture_id) return 1;
    // Check the entire registered storage, including levels outside the view.
    // Do not create or upload a texture while deciding whether to end a pass.
    pthread_mutex_lock(&g_rmu);
    int safe = 0;
    if (g_tpool_n && texture_id >= (unsigned)g_tpool_n) safe = 1;
    else if (texture_id < (unsigned)g_ntreg && g_treg[texture_id].ok) {
        const tex_t *texture = &g_treg[texture_id].t;
        if (texture->mem && texture->pool && texture->L.size && !(texture->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL)) {
            uint64_t address = (uint64_t)(uintptr_t)texture->mem, size = texture->L.size;
            safe = address <= UINT64_MAX - size && !vis_pending_overlap(address, size);
            if (safe && g_pass_batch_active && g_enc_mode != 5) {
                // Different NVN texture serials can share the same storage.
                // Keep feedback reads outside the pass even through aliases.
                for (int i = 0; i <= 8; i++) {
                    const tex_t *target = i == 8 ? R.rt.depth : i < R.rt.n ? R.rt.color[i] : NULL;
                    if (!target) continue;
                    uint64_t start = (uint64_t)(uintptr_t)target->mem, length = target->L.size;
                    if (!start || !length || start > UINT64_MAX - length ||
                        (address < start + length && start < address + size)) { safe = 0; break; }
                }
            }
        }
    }
    if (pass_batch_on() && !safe && texture_id < (unsigned)g_ntreg && g_treg[texture_id].ok) {
        const tex_t *texture = &g_treg[texture_id].t;
        PD("batch texture unsafe stage=%d slot=%u tid=%u serial=%llu mem=%d bytes=%llu pool=%x flags=%x active=%d mode=%d pending_queries=%d",
           stage, slot, texture_id, texture->serial, texture->mem != NULL, texture->L.size,
           texture->pool ? texture->pool->flags : 0, texture->d.flags, g_pass_batch_active, g_enc_mode, g_vis_pending_n);
    }
    pthread_mutex_unlock(&g_rmu);
    return safe;
}
static int render_draw_readonly(NvProg *program, const p_draw *arrays, const p_draw_idx *indexed) {
    if (!program || S.tf_on || S.rast_discard) return 0;
    if ((arrays && (arrays->ind || arrays->mdi_cnt)) || (indexed && (indexed->ind || indexed->mdi_cnt))) return 0;
    if (indexed && (indexed->itype < 0 || indexed->itype > 2 || indexed->count < 0 ||
        vis_pending_overlap(indexed->ibuf, (uint64_t)indexed->count << indexed->itype))) return 0;
    for (int i = 0; i < 16; i++)
        if (vis_pending_overlap(S.vbuf[i].address, S.vbuf[i].size)) return 0;
    for (int stage = 0; stage < 6; stage++) {
        NvShader *shader = program->sh[stage]; if (!shader) continue;
        if (stage > 1) { PD("v draw unsafe stage=%d key=%s", stage, shader->key); return 0; }
        for (int i = 0; i < shader->rf.nb; i++) {
            const nvn_rbind *binding = &shader->rf.b[i];
            switch (binding->kind) {
                case NVN_RB_CBUF: case NVN_RB_UBOP: case NVN_RB_SBUFP: case NVN_RB_SAMPU: {
                    if (binding->reg >= 16 || (storage_usage(shader, binding) & MTLResourceUsageWrite)) {
                        PD("batch buffer unsafe stage=%d key=%s kind=%d reg=%d rw=%d proof=%x", stage, shader->key, binding->kind, binding->reg, binding->rw, shader->sb_ro);
                        return 0;
                    }
                    const p_bind_buf *buffer = binding->kind == NVN_RB_SBUFP ? &S.ssbo[stage][binding->reg] : &S.ubo[stage][binding->reg];
                    if (vis_pending_overlap(buffer->addr, buffer->size)) return 0;
                    break;
                }
                case NVN_RB_DRV0: case NVN_RB_DRV2:
                    for (int reg = 0; reg < 16; reg++)
                        if (vis_pending_overlap(S.ssbo[stage][reg].addr, S.ssbo[stage][reg].size) ||
                            vis_pending_overlap(S.ubo[stage][reg].addr, S.ubo[stage][reg].size)) return 0;
                    break;
                case NVN_RB_TEX: case NVN_RB_TBUF:
                    if (!vis_texture_safe(stage, binding->reg)) return 0;
                    break;
                case NVN_RB_C1: case NVN_RB_SUPPORT:
                case NVN_RB_SAMP: case NVN_RB_SAMPT: case NVN_RB_SAMPI: case NVN_RB_SAMPD:
                    break;
                default:
                    PD("v draw unsafe stage=%d key=%s kind=%d reg=%d", stage, shader->key, binding->kind, binding->reg);
                    return 0;   // Images and unbounded addresses retain their original pass boundaries.
            }
        }
    }
    return 1;
}
static int vis_attachmentless_safe(NvProg *program) {
    if (vis_textures_on() < 2 || !g_vis_counting || !R.has_rt || R.rt.n != 1 || !R.rt.color[0] || R.rt.depth ||
        !program || !program->sh[0] || program->sh[0]->layer_out || !program->sh[1] ||
        S.chmask.mask || S.ds.dt || S.ds.dw || S.ds.st || S.ms.a2c || S.ms.samples > 1 || S.ms.raster_samples > 1 ||
        S.ms.cov_mod || S.ms.cov2color_en || S.ms.sl_en || R.clear_d || R.clear_s ||
        g_fz.active || g_defer_nact || g_hoist_n) return 0;
    for (int i = 0; i < 8; i++) if (R.clear_c[i]) return 0;
    NvShader *fragment = program->sh[1];
    if (!fragment->rf.color_known || fragment->rf.writes_smask) return 0;
    const tex_t *target = R.rt.color[0];
    if (target->d.target != NVN_TEXTURE_TARGET_2D || target->d.samples > 1 ||
        (R.rt.has_cview[0] && R.rt.cview[0].set_target && R.rt.cview[0].target != NVN_TEXTURE_TARGET_2D)) return 0;
    return 1;
}
static int vis_pending_keeps(uint32_t operation, const void *arguments) {
    switch (operation) {
        case OP_BEGIN: case OP_END: case OP_JUMP: case OP_CALL:
        case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_STENCIL: case OP_POLY_OFFSET: case OP_VIEWPORTS:
        case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
        case OP_DRAW: case OP_DRAW_IDX: case OP_SET_RT:
            return 1;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL:
            // Recording already rebases handles into the shared descriptor
            // tables. These replay records neither read nor write GPU data.
            return 1;
        case OP_RAST_DISCARD:
            // Disabling discard only changes replay state. Keep the boundary
            // when enabling it: a following vertex-only draw may write buffers.
            return !((const p_rast_discard *)arguments)->on;
        case OP_BIND_TEX: case OP_BIND_SAMP: {
            const p_bind_handles *bindings = arguments;
            // Deferred handles are fetched on the CPU during replay.
            return (vis_textures_on() || pass_batch_on()) && !bindings->defer;
        }
        case OP_RESET_COUNTER: case OP_REPORT_COUNTER: {
            uint64_t type; memcpy(&type, arguments, sizeof type);
            return type == NVN_COUNTER_TYPE_SAMPLES_PASSED;
        }
        case OP_BARRIER: {
            uint32_t bits; memcpy(&bits, arguments, sizeof bits);
            // These draws have no external writes. Attachment ordering is
            // already guaranteed within the render pass.
            return !(bits & ~(NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT));
        }
        default: return 0;
    }
}
static void vis_flush_native(void) {
    static id<MTLComputePipelineState> pipeline;
    if (!pipeline) {
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Slot { const device ulong *value; };\n"
            "struct Job { const device Slot *slots; device ulong *total; device ulong *output; ulong count, publish, ticks; };\n"
            "kernel void resolve(const device Job *jobs [[buffer(0)]], constant uint &count [[buffer(1)]]) {"
            "for (uint j = 0; j < count; ++j) { const device Job &job = jobs[j]; ulong sum = *job.total;"
            "for (ulong i = 0; i < job.count; ++i) sum += *job.slots[i].value;"
            "*job.total = sum; if (job.publish) { job.output[0] = sum; job.output[1] = job.ticks; } } }\n";
        NSError *error = nil;
        id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
        if (!library) MDIE("visibility resolve library failed: %s", error.localizedDescription.UTF8String);
        pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"resolve"] error:&error];
        if (!pipeline) MDIE("visibility resolve pipeline failed: %s", error.localizedDescription.UTF8String);
    }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    _Static_assert(sizeof(vis_gpu_job) == 48, "visibility job ABI");
    size_t count_offset = (size_t)g_vis_pending_n * sizeof(vis_gpu_job);
    id<MTLBuffer> arguments; size_t offset = arena_alloc(count_offset + 8, &arguments);
    vis_gpu_job *jobs = (vis_gpu_job *)((uint8_t *)arguments.contents + offset);
    for (int i = 0; i < g_vis_pending_n; i++) jobs[i] = g_vis_pending[i].gpu;
    *(uint32_t *)((uint8_t *)jobs + count_offset) = (uint32_t)g_vis_pending_n;
    id<MTLComputeCommandEncoder> encoder = enc_c_begin(nil); enc_c_pso(encoder, pipeline);
    for (int i = 0; i < g_vis_input_n; i++) {
        id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)g_vis_inputs[i];
        shader_use(encoder, 5, 0, buffer, MTLResourceUsageRead);
    }
    for (int i = 0; i < g_vis_pending_n; i++) {
        shader_use(encoder, 5, 0, (__bridge id)g_vis_pending[i].total_buffer, MTLResourceUsageRead | MTLResourceUsageWrite);
        shader_use(encoder, 5, 0, (__bridge id)g_vis_pending[i].output_buffer, MTLResourceUsageRead | MTLResourceUsageWrite);
    }
    enc_c_args(encoder, buffer_gpu_address(arguments) + offset, 0);
    enc_c_args(encoder, buffer_gpu_address(arguments) + offset + count_offset, 1);
    enc_c_dispatch(encoder, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(encoder);
}
static void vis_flush_tier1(void) {
    static id<MTLComputePipelineState> pipeline;
    if (!pipeline) {
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Params { ulong count, publish, ticks; };\n"
            "kernel void resolve(const device ulong *slots [[buffer(0)]], const device ulong *offsets [[buffer(1)]],"
            "device ulong *total [[buffer(2)]], device ulong *output [[buffer(3)]], constant Params &p [[buffer(4)]]) {"
            "ulong sum = *total; for (ulong i = 0; i < p.count; ++i) sum += slots[offsets[i]];"
            "*total = sum; if (p.publish) { output[0] = sum; output[1] = p.ticks; } }\n";
        NSError *error = nil;
        id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
        if (!library) MDIE("Tier 1 visibility resolve library failed: %s", error.localizedDescription.UTF8String);
        pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"resolve"] error:&error];
        if (!pipeline) MDIE("Tier 1 visibility resolve pipeline failed: %s", error.localizedDescription.UTF8String);
    }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLComputeCommandEncoder> encoder = enc_c_begin(nil); enc_c_pso(encoder, pipeline);
    int dispatched = 0;
    for (int i = 0; i < g_vis_pending_n; i++) {
        const vis_gpu_job *job = &g_vis_pending[i].gpu;
        NSUInteger source_offset;
        id<MTLBuffer> source = gpu_buffer_required(job->slots, MAX(job->count * 8, 1ull), &source_offset);
        const uint64_t *slots = (const uint64_t *)((const uint8_t *)source.contents + source_offset);
        id<MTLBuffer> arguments; size_t offset = arena_alloc(MAX((size_t)job->count * 8, (size_t)8), &arguments);
        uint64_t *indices = (uint64_t *)((uint8_t *)arguments.contents + offset);
        uint64_t start = 0;
        // Consecutive slots normally share a visibility buffer. Bind that buffer
        // once per group; only CPU-produced offsets are read here, never results.
        do {
            NSUInteger slot_offset = 0;
            id<MTLBuffer> buffer = job->count ? gpu_buffer_required(slots[start], 8, &slot_offset) :
                (__bridge id<MTLBuffer>)g_vis_pending[i].total_buffer;
            uint64_t end = start;
            while (end < job->count) {
                id<MTLBuffer> candidate = gpu_buffer_required(slots[end], 8, &slot_offset);
                if (candidate != buffer) break;
                if (slot_offset % 8) MDIE("unaligned visibility slot");
                indices[end++] = slot_offset / 8;
            }
            uint64_t parameters[3] = {end - start, end == job->count && job->publish, job->ticks};
            if (dispatched++) [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            [encoder setBuffer:buffer offset:0 atIndex:0];
            [encoder setBuffer:arguments offset:offset + start * 8 atIndex:1];
            enc_c_args(encoder, job->total, 2); enc_c_args(encoder, job->output, 3);
            [encoder setBytes:parameters length:sizeof parameters atIndex:4];
            enc_c_dispatch(encoder, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
            start = end;
        } while (start < job->count);
    }
    enc_c_end(encoder);
}
static void vis_flush_pending(void) {
    if (!g_vis_pending_n) return;
    if (g_enc) { rp_end(); return; }
    g_pg_vis_queries = g_vis_pending_n;
    if (g_nvn_tier1) vis_flush_tier1(); else vis_flush_native();
    g_pg_vis_queries = 0;
    for (int i = 0; i < g_vis_input_n; i++) CFRelease(g_vis_inputs[i]);
    for (int i = 0; i < g_vis_pending_n; i++) { CFRelease(g_vis_pending[i].total_buffer); CFRelease(g_vis_pending[i].output_buffer); }
    g_vis_input_n = g_vis_pending_n = 0;
}
static void vis_resolve(uint64_t destination) {
    if (!destination && !g_nvs) { if (!g_enc) vis_flush_pending(); return; }
    if (g_vis_pending_n == 512) { rp_flush(); vis_flush_pending(); }
    int defer = destination && g_enc && g_pass_readonly && vis_batch_on() && !vis_destination_is_attachment(destination);
    PD("v report pass=%llu defer=%d safe=%d slots=%d pending=%d", g_stat_pass, defer, g_pass_readonly, g_nvs, g_vis_pending_n);
    id<MTLBuffer> arguments; size_t offset = arena_alloc(MAX((size_t)g_nvs * 8, (size_t)8), &arguments);
    uint64_t *slots = (uint64_t *)((uint8_t *)arguments.contents + offset);
    vis_take_input((void *)CFBridgingRetain(arguments));
    for (int i = 0; i < g_nvs; i++) {
        id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)g_vs[i].buf;
        slots[i] = buffer_gpu_address(buffer) + (uint64_t)g_vs[i].slot * 8;
        vis_take_input(g_vs[i].buf);   // Transfer the slot's retained buffer to the pending batch.
    }
    id<MTLBuffer> total = vis_total(), output_buffer = total;
    uint64_t output = vis_total_gpu();
    if (destination) {
        void *object; uint64_t output_offset;
        if (!mtl_gpu_range(destination, 16, &object, &output_offset, &output)) MDIE("visibility report is outside registered memory");
        output_buffer = (__bridge id<MTLBuffer>)object;
        pend_add(destination, 16, 4); sbw_note(destination, 16, SBW_REPORT);
    }
    if (g_vis_pending_n == g_vis_pending_cap) {
        int capacity = g_vis_pending_cap ? 2 * g_vis_pending_cap : 32;
        vis_pending_job *pending = realloc(g_vis_pending, (size_t)capacity * sizeof *pending);
        if (!pending) MDIE("visibility report list allocation failed");
        g_vis_pending = pending; g_vis_pending_cap = capacity;
    }
    g_vis_pending[g_vis_pending_n++] = (vis_pending_job){
        {buffer_gpu_address(arguments) + offset, vis_total_gpu(), output, (uint64_t)g_nvs, destination != 0, gpu_ticks()}, destination,
        (void *)CFBridgingRetain(total), (void *)CFBridgingRetain(output_buffer)
    };
    g_nvs = 0; g_vis_cur = -1;
    if (!defer) { rp_flush(); vis_flush_pending(); }
}
static void vis_buffer_barrier(void) {
    if (g_vis_pending_n) { rp_flush(); vis_flush_pending(); }
}
