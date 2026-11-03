// Geometry stages use the normal replay resources and submission-owned arenas.
#include "nvn_geometry_indices.h"
#include "nvn_geometry_assemble.h"
#include "nvn_geometry_feedback.h"
#include "nvn_geometry_indirect.h"

typedef struct {
    uint64_t vertices, indices, indirect;
    size_t index_bytes;
    MTLPrimitiveType primitive;
    uint32_t max_indices, output_words, output_vertices, output_width;
} geometry_mesh;
typedef struct { uint32_t groups, maximum, stride, width, capacity, count, reserved[2]; } geometry_params;
static NSMapTable<id<MTLBuffer>, NSMutableDictionary<NSNumber *, NSNumber *> *> *g_tf_bounds;

static int geometry_feedback_bound(uint64_t control, uint32_t *bound) {
    void *object; uint64_t offset;
    if (!mtl_addr(control, 32, &object, &offset)) return 0;
    NSNumber *value = [g_tf_bounds objectForKey:(__bridge id<MTLBuffer>)object][@(offset)];
    if (!value) return 0;
    *bound = value.unsignedIntValue;
    return 1;
}

static uint64_t geometry_alloc(size_t bytes, void **contents) {
    id<MTLBuffer> buffer; size_t offset = arena_alloc(MAX(bytes, (size_t)4), &buffer);
    if (contents) *contents = (uint8_t *)buffer.contents + offset;
    return buffer_gpu_address(buffer) + offset;
}
static id<MTLBuffer> geometry_feedback_counters(void) {
    if (!g_tf_counters) {
        uint32_t initial[8] = {0, 0, 0, 0, 0, 1, 0, 0};
        for (unsigned b = 0; b < 4; b++) {
            if (S.tf_off[b] > UINT32_MAX) MDIE("feedback offset exceeds bounded addressing");
            initial[b] = (uint32_t)S.tf_off[b];
        }
        if (S.tf_vertices > UINT32_MAX) MDIE("feedback count exceeds bounded addressing");
        initial[4] = (uint32_t)S.tf_vertices;
        g_tf_bound = initial[4];
        g_tf_counters = [g_dev newBufferWithBytes:initial length:sizeof initial options:MTLResourceStorageModeShared];
        if (!g_tf_counters) MDIE("feedback counter allocation failed");
        res_add(g_tf_counters);
    }
    return g_tf_counters;
}
static void geometry_feedback_state(const p_tf_state *state) {
    defer_fallback();
    if (state->what == 0) {
        if (g_tf_counters) { res_remove(g_tf_counters); g_tf_counters = nil; }
        g_tf_bound = 0;
        state_op(OP_TF_STATE, state);
        return;
    }
    if (state->ctl) {
        void *object; uint64_t offset, address;
        if (!mtl_gpu_addr(state->ctl, 32, &object, &offset, &address)) MDIE("feedback control is outside registered memory");
        id<MTLBuffer> counters = geometry_feedback_counters();
        rp_flush();
        id<MTLBuffer> control = (__bridge id<MTLBuffer>)object;
        id<MTLBlitCommandEncoder> encoder;
        if (state->what == 3) {
            if (!geometry_feedback_bound(state->ctl, &g_tf_bound)) MDIE("feedback restore has no bounded capture provenance");
            encoder = blit_buf(state->ctl, state->ctl + 32, 0, 0);
            enc_b_copy_buffer(encoder, control, offset, counters, 0, 32);
        } else {
            encoder = blit_buf(0, 0, state->ctl, state->ctl + 32);
            enc_b_copy_buffer(encoder, counters, 0, control, offset, 32);
            pend_add(state->ctl, 32, 4);
            if (!g_tf_bounds) g_tf_bounds = [NSMapTable weakToStrongObjectsMapTable];
            NSMutableDictionary *bounds = [g_tf_bounds objectForKey:control];
            if (!bounds) { bounds = [NSMutableDictionary dictionary]; [g_tf_bounds setObject:bounds forKey:control]; }
            bounds[@(offset)] = @(g_tf_bound);
        }
        enc_b_end(encoder); g_blit = nil;
    }
    state_op(OP_TF_STATE, state);
}
static uint64_t geometry_indirect_input(geometry_mesh *mesh, uint64_t source, uint64_t info, uint64_t remap,
                                        uint32_t bound, unsigned primitive, int has_geometry) {
    static id<MTLComputePipelineState> pipeline;
    if (!pipeline) {
        NSError *error = nil;
        id<MTLLibrary> library = [g_dev newLibraryWithSource:@(k_geometry_indirect_msl) options:nil error:&error];
        if (!library) MDIE("geometry indirect input library failed: %s", error.localizedDescription.UTF8String);
        pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"dm_gs_input"] error:&error];
        if (!pipeline) MDIE("geometry indirect input pipeline failed: %s", error.localizedDescription.UTF8String);
    }
    unsigned width = nvn_geometry_input_width(primitive), output_width = has_geometry ? 0 : width == 4 ? 2 : width == 6 ? 3 : width;
    uint32_t primitives = nvn_geometry_input_count(primitive, bound);
    uint64_t total = (uint64_t)primitives * output_width;
    if (total > INT_MAX) MDIE("feedback indirect indices exceed bounded addressing");
    uint32_t *parameters, *status;
    uint64_t parameter_gpu = geometry_alloc(16, (void **)&parameters), status_gpu = geometry_alloc(4, (void **)&status);
    parameters[0] = bound; parameters[1] = primitive; parameters[2] = width; parameters[3] = output_width; *status = 0;
    mesh->indices = geometry_alloc(MAX(total * 4, UINT64_C(4)), NULL);
    mesh->indirect = geometry_alloc(20, NULL); mesh->index_bytes = MAX(total * 4, UINT64_C(4)); mesh->max_indices = (uint32_t)total;
    uint64_t capture = geometry_alloc(16, NULL);
    rp_flush(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLComputeCommandEncoder> encoder = enc_c_begin(nil); enc_c_pso(encoder, pipeline);
    uint64_t resources[] = {source, info, capture, remap, mesh->indirect, mesh->indices, parameter_gpu, status_gpu};
    for (unsigned i = 0; i < 8; i++) enc_c_args(encoder, resources[i], i);
    enc_c_dispatch(encoder, MTLSizeMake(MAX(((uint64_t)primitives + 63) / 64, UINT64_C(1)), 1, 1), MTLSizeMake(64, 1, 1));
    enc_c_end(encoder);
    NSUInteger offset; id<MTLBuffer> owner = gpu_buffer_required(status_gpu, 4, &offset);
    [g_commit_blocks addObject:^{
        uint32_t flags; memcpy(&flags, (const uint8_t *)owner.contents + offset, 4);
        if (flags) MDIE("geometry indirect input exceeds its recorded capture bounds");
    }];
    return capture;
}
int g_nvn_geometry_small;
static int geometry_small_on(void) {
    static int initialized;
    if (!initialized) {
        const char *value = getenv("DM_NVN_GEOMETRY_SMALL");
        if (value) g_nvn_geometry_small = strcmp(value, "0") != 0;
        initialized = 1;
    }
    return g_nvn_geometry_small;
}
static id<MTLComputePipelineState> geometry_pipeline(unsigned index) {
    static id<MTLComputePipelineState> pipelines[6];
    if (!pipelines[0]) {
        NSError *error = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_geometry_assemble_msl) options:nil error:&error];
        if (!lib) MDIE("geometry assembly library failed: %s", error.localizedDescription.UTF8String);
        const char *names[] = {"dm_gs_count", "dm_gs_scan", "dm_gs_add", "dm_gs_emit", "dm_gs_finalize", "dm_gs_small"};
        for (unsigned i = 0; i < 6; i++) {
            pipelines[i] = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(names[i])] error:&error];
            if (!pipelines[i]) MDIE("geometry assembly pipeline failed: %s", error.localizedDescription.UTF8String);
        }
    }
    return pipelines[index];
}
static void geometry_pass(id<MTLComputeCommandEncoder> e, unsigned pipeline, uint64_t first, uint64_t second, uint64_t third,
                          uint64_t indirect, uint64_t status, uint64_t params, uint32_t count) {
    enc_c_pso(e, geometry_pipeline(pipeline));
    uint64_t resources[] = {first, second, third, indirect, status, params};
    unsigned slots[] = {0, 1, 2, 4, 5, 6};
    for (unsigned i = 0; i < 6; i++) enc_c_args(e, resources[i], slots[i]);
    unsigned threads = pipeline >= 4 ? 1 : NVN_GEOMETRY_SCAN_THREADS;
    enc_c_dispatch(e, MTLSizeMake(((uint64_t)count + threads - 1) / threads, 1, 1), MTLSizeMake(threads, 1, 1));
    [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
}
static void geometry_assemble(geometry_mesh *mesh, uint64_t raw, uint32_t groups,
                              uint32_t maximum, uint32_t stride, unsigned width, id<MTLComputeCommandEncoder> supplied) {
    id<MTLComputeCommandEncoder> encoder = supplied ?: enc_c_begin(nil);
    uint64_t per_group = width == 1 ? maximum : maximum >= width ? width * ((uint64_t)maximum - width + 1) : 0;
    uint64_t capacity = groups * per_group;
    if (capacity > INT_MAX) MDIE("geometry assembled indices exceed 32-bit addressing");
    mesh->max_indices = (uint32_t)capacity;
    mesh->index_bytes = MAX(capacity * 4, UINT64_C(4));
    mesh->indices = geometry_alloc(mesh->index_bytes, NULL);
    void *argument_bytes, *status_bytes;
    mesh->indirect = geometry_alloc(20, &argument_bytes); memset(argument_bytes, 0, 20);
    uint64_t status = geometry_alloc(4, &status_bytes); memset(status_bytes, 0, 4);
    geometry_params p = {groups, maximum, stride, width, (uint32_t)capacity, groups, {0}};
    void *bytes; uint64_t params = geometry_alloc(sizeof p, &bytes); memcpy(bytes, &p, sizeof p);
    if (geometry_small_on() && groups && groups <= 32 && (uint64_t)groups * stride <= 256) {
        // A bounded serial walk keeps the exact group and strip order while
        // avoiding four dispatches and a 256-lane scan for a few records.
        geometry_pass(encoder, 5, raw, raw, mesh->indices, mesh->indirect, status, params, 1);
        PD("G small assembly groups=%u maximum=%u stride=%u width=%u", groups, maximum, stride, width);
        static unsigned shown;
        if (shown++ < 4) MLOG("geometry small assembly: frame=%llu groups=%u maximum=%u width=%u", (unsigned long long)atomic_load(&g_cur_frame), groups, maximum, width);
    } else {
        uint64_t counts = geometry_alloc((uint64_t)groups * 4, NULL);
        typedef struct { uint32_t count; uint64_t input, offsets, sums, params; } scan_level;
        scan_level levels[8]; unsigned nlevels = 0; uint32_t count = groups; uint64_t input = counts;
        do {
            if (nlevels == 8) MDIE("geometry prefix-scan depth exceeds its addressing range");
            scan_level *level = &levels[nlevels++]; level->count = count; level->input = input;
            level->offsets = geometry_alloc((uint64_t)count * 4, NULL);
            uint32_t blocks = (uint32_t)(((uint64_t)count + 255) / 256);
            level->sums = geometry_alloc((uint64_t)blocks * 4, NULL);
            p.count = count; level->params = geometry_alloc(sizeof p, &bytes); memcpy(bytes, &p, sizeof p);
            count = blocks; input = level->sums;
        } while (count > 1);
        geometry_pass(encoder, 0, raw, counts, counts, mesh->indirect, status, params, groups);
        for (unsigned i = 0; i < nlevels; i++) {
            scan_level *l = &levels[i];
            geometry_pass(encoder, 1, l->input, l->offsets, l->sums, mesh->indirect, status, l->params, l->count);
        }
        for (unsigned i = nlevels - 1; i > 0; i--) {
            scan_level *l = &levels[i - 1];
            geometry_pass(encoder, 2, l->offsets, levels[i].offsets, counts, mesh->indirect, status, l->params, l->count);
        }
        geometry_pass(encoder, 3, raw, levels[0].offsets, mesh->indices, mesh->indirect, status, params, groups);
        geometry_pass(encoder, 4, counts, levels[0].offsets, counts, mesh->indirect, status, params, 1);
    }
    enc_c_end(encoder);
    NSUInteger offset; id<MTLBuffer> owner = gpu_buffer_required(status, 4, &offset);
    [g_commit_blocks addObject:^{
        uint32_t flags; memcpy(&flags, (const uint8_t *)owner.contents + offset, 4);
        if (flags) MDIE("geometry GPU assembly rejected output (flags=%#x)", flags);
    }];
}
static NvShader *geometry_child(NvShader *root, NSDictionary *layout, NSString *role) {
    NSString *entry = layout[[role stringByAppendingString:@"_reflection"]][@"entry"];
    NvShader *child = root->gs_children[entry];
    if (child) return child;
    NSMutableDictionary *j = [root->gs_layout mutableCopy];
    [j addEntriesFromDictionary:layout[[role stringByAppendingString:@"_reflection"]]];
    int stage = [role isEqual:@"geometry"] ? 2 : 0;
    child = shader_from_json(@(root->key), stage, root->gs_library, j);
    if (!child || !child->fn) MDIE("cannot load geometry stage entry %s", entry.UTF8String);
    if (!stage) {
        child->generated = 1;
        child->range_ro = 0;
        child->layer_out = 0;
        for (NSDictionary *field in layout[@"offsets"])
            if ([field[@"storage"] isEqual:@"Output"] && [field[@"variable"] isEqual:@"Layer"]) child->layer_out = 1;
    }
    root->gs_children[entry] = child;
    return child;
}
int g_nvn_geometry_inline;
static uint64_t g_geometry_capture_passes, g_geometry_inline_reuses;
static int geometry_inline_on(void) {
    static int initialized;
    if (!initialized) {
        const char *value = getenv("DM_NVN_GEOMETRY_INLINE");
        if (value) g_nvn_geometry_inline = strcmp(value, "0") != 0;
        initialized = 1;
    }
    return g_nvn_geometry_inline;
}
static int geometry_inline_can(NvShader *vertex) {
    // Captured records use fresh, bounded arena allocations. Any stores from
    // the original vertex shader still need the ordinary fragment alias proof.
    if (!geometry_inline_on() || !vertex) return 0;
    for (int i = 0; i < vertex->rf.nb; i++) {
        // A texture read may consume the preceding fragment stage's output.
        // Keep those captures behind the original full encoder boundary.
        switch (vertex->rf.b[i].kind) {
            case NVN_RB_CBUF: case NVN_RB_SBUFP: case NVN_RB_UBOP:
            case NVN_RB_C1: case NVN_RB_SUPPORT: case NVN_RB_DRV0: case NVN_RB_DRV2:
                break;
            default: return 0;
        }
    }
    return discard_inline_can(vertex);
}
static id<MTLRenderPipelineState> geometry_capture_pso(NvShader *vertex, int continued) {
    static const char capture_mark; static const uint32_t formats[8];
    pso_key k; pso_key_fill(&k, vertex, nil, continued ? g_pass.cfmt : formats,
        continued ? g_pass.dfmt : 0, continued ? g_pass.sfmt : 0, continued ? g_pass.samples : 1, 0);
    k.fsfn = &capture_mark; k.a2c = k.blend_en = k.chmask = 0;
    id<MTLRenderPipelineState> pipeline = hm_get_id(&g_pso, &k);
    if (!pipeline) {
        MTLRenderPipelineDescriptor *d = pso_desc(&k, vertex, nil, nil, 0);
        d.vertexFunction = vertex->gc_fn; d.fragmentFunction = nil; d.rasterizationEnabled = NO;
        d.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
        NSError *error = nil; pipeline = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
        if (!pipeline) MDIE("geometry capture pipeline failed (%s): %s", vertex->key, error.localizedDescription.UTF8String);
        hm_put_id(&g_pso, &k, pipeline);
    }
    return pipeline;
}
typedef struct { id<MTLRenderCommandEncoder> encoder; int continued; } geometry_capture_encoder;
static geometry_capture_encoder geometry_capture_begin(NvShader *vertex) {
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    geometry_capture_encoder capture = {.continued = geometry_inline_can(vertex)};
    if (capture.continued) {
        capture.encoder = g_enc;
        [capture.encoder memoryBarrierWithScope:MTLBarrierScopeBuffers afterStages:MTLRenderStageVertex beforeStages:MTLRenderStageVertex];
        g_geometry_inline_reuses++;
        PD("i %llu disjoint geometry capture vs=%s backings=%u", g_stat_pass, vertex->key, g_inline_reads_n);
        static unsigned shown;
        if (shown++ < 4) MLOG("disjoint geometry capture: frame=%llu pass=%llu vertex=%s", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, vertex->key);
    } else {
        rp_flush();
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
        pass.renderTargetWidth = pass.renderTargetHeight = pass.defaultRasterSampleCount = 1;
        capture.encoder = enc_r_begin(pass); g_geometry_capture_passes++;
    }
    enc_r_pso(capture.encoder, geometry_capture_pso(vertex, capture.continued));
    return capture;
}
static void geometry_capture_end(geometry_capture_encoder capture) {
    // Ending the shared encoder publishes both the earlier fragment reads and
    // the captured records before the existing compute / feedback stages.
    if (capture.continued) {
        int target_pending = g_geometry_rt_pending;
        rp_end();
        if (target_pending) sub_cut();
    }
    else enc_r_end(capture.encoder);
}
typedef struct {
    uint32_t words, width, vertices, count;
    uint32_t strides[4], sizes[4], starts[4], map[512];
} geometry_feedback_params;
int g_nvn_tf_layout_cache = 1;
static void geometry_feedback_layout(NSArray *layout, NSArray *offsets, geometry_feedback_params *p) {
    memset(p, 0, sizeof *p); memset(p->map, 0xff, sizeof p->map);
    for (NSUInteger b = 0; b < MIN(layout.count, 4); b++) {
        NSArray *binding = layout[b]; if (binding == (id)NSNull.null || !binding.count) continue;
        uint32_t stride = [binding[1] unsignedIntValue];
        if (!stride || stride % 4 || stride > 512) MDIE("feedback buffer has an unsupported bounded layout");
        p->strides[b] = stride / 4;
        unsigned at = 0;
        for (NSArray *attribute in binding[0]) {
            unsigned location = [attribute[0] unsignedIntValue], components = [attribute[1] unsignedIntValue];
            for (unsigned component = 0; component < components; component++) {
                if (at >= p->strides[b] || location >= 32 || components > 4) MDIE("feedback attribute exceeds its stride");
                uint32_t word = component == 3 ? UINT32_MAX - 1 : UINT32_MAX;
                if (!offsets) word = 14 + 4 * location + component;
                else for (NSDictionary *field in offsets)
                    if ([field[@"storage"] isEqual:@"Output"] && [field[@"variable"] isEqual:@"UserDefined"] &&
                        [field[@"location"] unsignedIntValue] == location && [field[@"component"] unsignedIntValue] == component)
                        word = [field[@"offset"] unsignedIntValue];
                p->map[b * 128 + at++] = word;
            }
        }
    }
}
static void geometry_feedback(geometry_mesh *mesh, NvShader *source, unsigned input_width, NSArray *offsets) {
    NSArray *layout = source->tf_layout;
    if (!layout || layout == (id)NSNull.null) return;
    if (!input_width || input_width >= 7) MDIE("feedback input width is outside its bounded layout");
    static id<MTLComputePipelineState> reserve, copy;
    if (!reserve) {
        NSError *error = nil;
        id<MTLLibrary> library = [g_dev newLibraryWithSource:@(k_geometry_feedback_msl) options:nil error:&error];
        if (!library) MDIE("geometry feedback library failed: %s", error.localizedDescription.UTF8String);
        reserve = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"dm_gs_tf_reserve"] error:&error];
        copy = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"dm_gs_tf_copy"] error:&error];
        if (!reserve || !copy) MDIE("geometry feedback pipeline failed: %s", error.localizedDescription.UTF8String);
    }
    geometry_feedback_counters();
    uint64_t additional = mesh->max_indices;
    geometry_feedback_params *p; uint64_t parameters = geometry_alloc(sizeof *p, (void **)&p);
    if (g_nvn_tf_layout_cache) {
        // Reflection is immutable. Buffer addresses, lengths and capture offsets
        // are still taken from the current replay state below.
        if (!source->tf_maps[input_width]) {
            geometry_feedback_params prepared;
            geometry_feedback_layout(layout, offsets, &prepared);
            source->tf_maps[input_width] = [NSData dataWithBytes:&prepared length:sizeof prepared];
        }
        memcpy(p, source->tf_maps[input_width].bytes, sizeof *p);
    } else geometry_feedback_layout(layout, offsets, p);
    p->words = mesh->output_words; p->vertices = mesh->output_vertices; p->width = mesh->output_width;
    uint64_t outputs[4] = {0};
    for (unsigned b = 0; b < 4; b++) {
        if (!p->strides[b]) continue;
        uint32_t stride = p->strides[b] * 4;
        if (!S.tf[b].addr || S.tf[b].size > UINT32_MAX || (S.tf[b].size & 3))
            MDIE("feedback buffer has an unsupported bounded layout");
        void *object; uint64_t offset;
        if (!mtl_gpu_addr(S.tf[b].addr, MAX(S.tf[b].size, 1), &object, &offset, &outputs[b])) MDIE("feedback output is outside registered memory");
        p->sizes[b] = (uint32_t)S.tf[b].size;
        additional = MIN(additional, S.tf[b].size / stride);
        int previous = pend_last(S.tf[b].addr, S.tf[b].size);
        if (previous < 0 || g_pend_kind[previous] != 4 || g_pend_lo[previous] > S.tf[b].addr || g_pend_hi[previous] < S.tf[b].addr + S.tf[b].size)
            pend_add(S.tf[b].addr, S.tf[b].size, 4);
    }
    g_tf_bound = (uint32_t)MIN((uint64_t)g_tf_bound + additional, UINT32_MAX);
    id<MTLComputeCommandEncoder> e = enc_c_begin(nil);
    enc_c_args(e, mesh->vertices, 0); enc_c_args(e, mesh->indices, 1); enc_c_args(e, mesh->indirect, 2);
    enc_c_args(e, buffer_gpu_address(g_tf_counters), 3); enc_c_args(e, parameters, 4);
    for (unsigned b = 0; b < 4; b++) enc_c_args(e, outputs[b] ?: parameters, 5 + b);
    enc_c_pso(e, reserve); enc_c_dispatch(e, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
    [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
    enc_c_pso(e, copy); enc_c_dispatch(e, MTLSizeMake(((uint64_t)mesh->max_indices + 63) / 64, 1, 1), MTLSizeMake(64, 1, 1)); enc_c_end(e);
}
static NvShader *geometry_vertex_passthrough(NvShader *vertex) {
    if (!vertex->gp_shader) {
        if (!vertex->gp_fn) MDIE("vertex feedback has no canonical passthrough entry");
        NvShader *child = [NvShader new]; child->fn = vertex->gp_fn; child->stage = 0; child->generated = 1; child->layer_out = vertex->layer_out;
        child->rf.pos_attr = -1; memset(child->rf.uv_attr, -1, sizeof child->rf.uv_attr); memset(child->rf.uv_aff_attr, -1, sizeof child->rf.uv_aff_attr);
        strlcpy(child->key, vertex->key, sizeof child->key); vertex->gp_shader = child;
    }
    return vertex->gp_shader;
}
static int geometry_read(uint64_t address, size_t bytes, void *output) {
    if (!bytes || address > UINT64_MAX - bytes || g_sbw_over || sbw_hits(address, address + bytes) || gw_live(address, address + bytes)) return 0;
    void *object; uint64_t offset;
    if (!mtl_addr(address, bytes, &object, &offset)) return 0;
    if (pend_hits(address, bytes)) {
        int kind; if (pend_read(address, bytes, output, &kind)) return 1;
        uint64_t gpu; const uint8_t *shadow;
        if (!ubp_get(address, address + bytes, &gpu, &shadow)) return 0;
        memcpy(output, shadow, bytes); return 1;
    }
    id<MTLBuffer> owner = (__bridge id<MTLBuffer>)object;
    memcpy(output, (const uint8_t *)owner.contents + offset, bytes); return 1;
}
static NvProg *geometry_prepare(NvProg *program, const p_draw *requested_draw, const p_draw_idx *requested_indexed, geometry_mesh *mesh) {
    p_draw resolved_draw = requested_draw ? *requested_draw : (p_draw){0};
    p_draw_idx resolved_indexed = requested_indexed ? *requested_indexed : (p_draw_idx){0};
    const p_draw *draw = requested_draw ? &resolved_draw : NULL;
    const p_draw_idx *indexed = requested_indexed ? &resolved_indexed : NULL;
    NvShader *root = program->sh[2], *vertex = program->sh[0];
    if (!vertex || !vertex->gc_fn || program->sh[3] || program->sh[4]) MDIE("geometry requires a capture vertex stage without tessellation");
    uint64_t indirect = draw ? draw->ind : indexed->ind, dynamic_input = 0;
    if (indirect) {
        uint32_t args[5] = {0};
        if (!geometry_read(indirect, indexed ? 20 : 16, args)) {
            uint32_t bound;
            void *object; uint64_t offset;
            if (indexed || indirect < 16 || !geometry_feedback_bound(indirect - 16, &bound) ||
                !mtl_gpu_addr(indirect, 16, &object, &offset, &dynamic_input))
                MDIE("geometry GPU-generated indirect input needs bounded capture provenance");
            args[0] = bound; args[1] = 1;
        }
        if (!args[0] || !args[1]) return nil;
        if (args[0] > INT_MAX || args[1] > INT_MAX || (draw && args[2] > INT_MAX)) MDIE("geometry indirect input exceeds bounded addressing");
        if (draw) { resolved_draw.count = args[0]; resolved_draw.instances = args[1]; resolved_draw.first = args[2]; resolved_draw.base_instance = args[3]; resolved_draw.ind = 0; }
        else {
            unsigned size = indexed->itype >= 0 && indexed->itype <= 2 ? 1u << indexed->itype : 0;
            if (!size || indexed->ibuf > UINT64_MAX - (uint64_t)args[2] * size) MDIE("geometry indirect index range overflows");
            resolved_indexed.count = args[0]; resolved_indexed.instances = args[1]; resolved_indexed.ibuf += (uint64_t)args[2] * size;
            resolved_indexed.base_vertex = (int32_t)args[3]; resolved_indexed.base_instance = args[4]; resolved_indexed.ind = 0;
        }
    }
    uint64_t counter = draw ? draw->mdi_cnt : indexed->mdi_cnt;
    if (counter) {
        uint32_t count; if (!geometry_read(counter, 4, &count)) MDIE("geometry GPU-generated multi-draw count needs GPU expansion");
        int index = draw ? draw->mdi_i : indexed->mdi_i; if (index < 0 || (uint32_t)index >= count) return nil;
    }
    if ((draw ? draw->base_instance : indexed->base_instance) < 0) MDIE("geometry base instance exceeds bounded addressing");
    int primitive = draw ? draw->prim : indexed->prim;
    unsigned width = nvn_geometry_input_width(primitive);
    if (!width) MDIE("geometry input topology %d is unsupported", primitive);
    NSDictionary *layout = nil;
    for (NSDictionary *candidate in (root ? root->gs_layout[@"geometry"] : nil))
        if ([candidate[@"input_vertices"] unsignedIntValue] == width) layout = candidate;
    uint32_t maximum = [layout[@"max_vertices"] unsignedIntValue], stride = [layout[@"index_stride"] unsignedIntValue];
    uint32_t invocations = [layout[@"invocations"] unsignedIntValue], words = [layout[@"output_words"] unsignedIntValue];
    if (root && (!layout || [layout[@"input_words"] intValue] != 142 || [layout[@"vertex_info_bytes"] intValue] != 1056 ||
        !maximum || maximum > 256 || stride != 2 * maximum || !invocations || invocations > 32 || !words || words > 160))
        MDIE("geometry layout has an invalid bounded ABI");
    int32_t signed_count = draw ? draw->count : indexed->count;
    uint32_t instances = MAX(draw ? draw->instances : indexed->instances, 1);
    if (signed_count <= 0) return nil;
    uint32_t count = (uint32_t)signed_count, vertices = count, primitives;
    uint32_t *remap, *unique = NULL; uint64_t unique_gpu = 0;
    if (indexed) {
        if (indexed->itype < 0 || indexed->itype > 2) MDIE("geometry index format is invalid");
        size_t index_bytes = (size_t)count * (1u << indexed->itype);
        uint8_t *source;
        geometry_alloc(index_bytes, (void **)&source);
        if (!geometry_read(indexed->ibuf, index_bytes, source)) MDIE("geometry GPU-written indices need GPU remapping");
        uint32_t *raw, *dense;
        geometry_alloc((uint64_t)count * 4, (void **)&raw); geometry_alloc((uint64_t)count * 4, (void **)&dense);
        unique_gpu = geometry_alloc((uint64_t)count * 4, (void **)&unique);
        for (uint32_t i = 0; i < count; i++) {
            raw[i] = 0; memcpy(&raw[i], source + ((size_t)i << indexed->itype), 1u << indexed->itype);
        }
        // NVN fixed-index restart is enabled for strip and fan topologies.
        uint64_t restart = primitive == 2 || primitive == 3 || primitive == 5 || primitive == 6 || primitive == 11 || primitive == 13
            ? (indexed->itype == 2 ? UINT32_MAX : (1u << (8u << indexed->itype)) - 1u) : UINT64_MAX;
        if (!nvn_geometry_dense_indices(raw, count, indexed->base_vertex, restart, unique, dense, &vertices))
            MDIE("geometry base vertex exceeds unsigned vertex addressing");
        primitives = nvn_geometry_index_primitives(primitive, dense, count, NULL);
        if (!primitives || !vertices) return nil;
        mesh->indirect = geometry_alloc((uint64_t)primitives * width * 4, (void **)&remap);
        nvn_geometry_index_primitives(primitive, dense, count, remap);
    } else {
        if (draw->first < 0 || (uint64_t)draw->first + count > UINT32_MAX) MDIE("geometry vertex range is invalid");
        primitives = nvn_geometry_input_count(primitive, count);
        if (!primitives) return nil;
        mesh->indirect = geometry_alloc((uint64_t)primitives * width * 4, (void **)&remap);
        for (uint32_t i = 0; i < primitives; i++) nvn_geometry_input_indices(primitive, count, i, remap + (size_t)i * width);
    }
    uint64_t remap_gpu = mesh->indirect;
    PD("G requested vs=%s gs=%s prim=%d width=%u count=%u vertices=%u primitives=%u instances=%u indexed=%d indirect=%d maximum=%u invocations=%u words=%u tf=%d",
       vertex->key, root ? root->key : "-", primitive, width, count, vertices, primitives, instances, indexed != NULL, indirect != 0,
       maximum, invocations, words, S.tf_on);
    uint64_t groups = (uint64_t)primitives * instances * invocations, output_vertices = groups * maximum;
    if (groups > INT_MAX || output_vertices * words > INT_MAX || groups * stride > INT_MAX || (uint64_t)vertices * instances * 142 > INT_MAX)
        MDIE("geometry expansion exceeds bounded 32-bit shader addressing");
    void *raw_bytes, *state_bytes;
    uint64_t input = geometry_alloc((uint64_t)vertices * instances * 142 * 4, NULL);
    mesh->vertices = root ? geometry_alloc(output_vertices * words * 4, NULL) : input;
    uint64_t raw = geometry_alloc(groups * stride * 4, &raw_bytes); memset(raw_bytes, 0xff, groups * stride * 4);
    uint64_t info = geometry_alloc(1056, &state_bytes); memset(state_bytes, 0, 1056);
    uint32_t *state = state_bytes;
    state[0] = vertices; state[1] = instances; state[2] = draw ? draw->first : indexed->base_vertex;
    state[3] = draw ? draw->base_instance : indexed->base_instance; state[4] = primitives; state[5] = primitives; state[6] = indexed != NULL;
    uint64_t capture_indirect = dynamic_input ? geometry_indirect_input(mesh, dynamic_input, info, remap_gpu, count, primitive, root != nil) : 0;
    NvShader *geometry = root ? geometry_child(root, layout, @"geometry") : nil;
    NvProg *raster = program->gs_pr[width];
    if (!raster) { raster = [NvProg new]; raster->sh[0] = root ? geometry_child(root, layout, @"passthrough") : geometry_vertex_passthrough(vertex); raster->sh[1] = program->sh[1]; program->gs_pr[width] = raster; }
    geometry_capture_encoder capture = geometry_capture_begin(vertex);
    id<MTLRenderCommandEncoder> e = capture.encoder;
    g_draw_binst = (uint32_t)(draw ? draw->base_instance : indexed->base_instance);
    args_dirty_all(); bind_vbufs(e); vconv_bind(e, &vertex->rf, draw, indexed); encode_args(e, vertex, 0, 1);
    if (capture.continued && !geometry_inline_can(vertex)) {
        geometry_capture_end(capture);
        return geometry_prepare(program, requested_draw, requested_indexed, mesh);
    }
    enc_r_vbuf(e, 8, input); enc_r_vbuf(e, 9, info); enc_r_vbuf(e, 14, unique_gpu ?: info);
    if (indexed) enc_r_draw_indexed(e, MTLPrimitiveTypePoint, vertices, MTLIndexTypeUInt32, unique_gpu, (size_t)vertices * 4, instances, indexed->base_vertex, indexed->base_instance);
    else if (capture_indirect) enc_r_draw_indirect(e, MTLPrimitiveTypePoint, capture_indirect);
    else enc_r_draw_instanced(e, MTLPrimitiveTypePoint, draw->first, count, instances, draw->base_instance);
    geometry_capture_end(capture); args_dirty_all();
    if (!root) {
        unsigned output_width = width == 4 ? 2 : width == 6 ? 3 : width;
        uint64_t total = (uint64_t)primitives * output_width * instances;
        if (total > INT_MAX) MDIE("feedback primitive range exceeds bounded addressing");
        uint32_t *indices, *arguments;
        if (!dynamic_input) {
            mesh->indices = geometry_alloc(MAX(total * 4, UINT64_C(4)), (void **)&indices);
            mesh->indirect = geometry_alloc(20, (void **)&arguments);
            uint32_t values[5] = {(uint32_t)total, 1, 0, 0, 0}; memcpy(arguments, values, sizeof values);
            for (uint32_t instance = 0; instance < instances; instance++) for (uint32_t primitive_index = 0; primitive_index < primitives; primitive_index++)
                for (unsigned component = 0; component < output_width; component++) {
                    unsigned slot = width == 4 ? component + 1 : width == 6 ? 2 * component : component;
                    indices[((uint64_t)instance * primitives + primitive_index) * output_width + component] = instance * vertices + remap[(uint64_t)primitive_index * width + slot];
                }
        }
        mesh->index_bytes = MAX(total * 4, UINT64_C(4)); mesh->max_indices = (uint32_t)total;
        mesh->output_words = 142; mesh->output_vertices = vertices * instances; mesh->output_width = output_width;
        mesh->primitive = output_width == 1 ? MTLPrimitiveTypePoint : output_width == 2 ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle;
        geometry_feedback(mesh, vertex, width, nil);
        g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0; g_pso_dirty = g_dss_dirty = 1;
        return raster;
    }
    static nvn_hmap pipelines = {.klen = sizeof(void *)};
    void *key = (__bridge void *)geometry->fn;
    id<MTLComputePipelineState> pipeline = hm_get_id(&pipelines, &key);
    if (!pipeline) {
        NSError *error = nil; pipeline = [g_dev newComputePipelineStateWithFunction:geometry->fn error:&error];
        if (!pipeline) MDIE("geometry compute pipeline failed: %s", error.localizedDescription.UTF8String);
        hm_put_id(&pipelines, &key, pipeline);
    }
    id<MTLComputeCommandEncoder> c = enc_c_begin(nil); enc_c_pso(c, pipeline); encode_args(c, geometry, 2, -1);
    enc_c_args(c, input, 8); enc_c_args(c, info, 9); enc_c_args(c, mesh->vertices, 10); enc_c_args(c, raw, 11); enc_c_args(c, remap_gpu, 12);
    enc_c_dispatch(c, MTLSizeMake(((uint64_t)primitives + 31) / 32, instances, invocations), MTLSizeMake(32, 1, 1)); [c memoryBarrierWithScope:MTLBarrierScopeBuffers];
    NSString *topology = layout[@"output_topology"];
    unsigned output_width = [topology isEqual:@"Points"] ? 1 : [topology isEqual:@"LineStrip"] ? 2 : [topology isEqual:@"TriangleStrip"] ? 3 : 0;
    if (!output_width) MDIE("geometry output topology is invalid");
    mesh->primitive = output_width == 1 ? MTLPrimitiveTypePoint : output_width == 2 ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle;
    geometry_assemble(mesh, raw, (uint32_t)groups, maximum, stride, output_width, c);
    mesh->output_words = words; mesh->output_vertices = (uint32_t)output_vertices; mesh->output_width = output_width;
    if (S.tf_on) geometry_feedback(mesh, root, width, layout[@"offsets"]);
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0; g_pso_dirty = g_dss_dirty = 1;
    { static int once; if (!once++) MLOG("geometry: capture, compute expansion and GPU indirect assembly active"); }
    return raster;
}
