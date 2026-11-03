static int discard_inline_on(void) {
    static int initialized;
    if (!initialized) {
        const char *value = getenv("DM_NVN_DISCARD_INLINE");
        if (value) g_nvn_discard_inline = strcmp(value, "0") != 0;
        initialized = 1;
    }
    return g_nvn_discard_inline;
}
static int discard_inline_shape(void) {
    if (!discard_inline_on() || !g_enc || g_enc_mode != 0 || !R.has_rt || g_pass.samples != 1 ||
        g_vis_pending_n || g_fz.active || g_defer_nact || g_defer_pend || g_hoist_n || g_rt_range_active ||
        g_pass.layered || g_nlc || g_nubd || g_nbcrt || !rt_batch_storage_known(&R.rt) || R.clear_d || R.clear_s) return 0;
    for (int i = 0; i < 8; i++) if (R.clear_c[i]) return 0;
    return g_inline_pool_generation == atomic_load(&g_pool_gen) &&
        g_inline_registration == g_reg_gen && g_inline_serial == g_ser_gen;
}
static int inline_read_range(uint64_t address, uint64_t length) {
    if (!address || !length || length > (16u << 20)) return 0;
    for (unsigned chunks = 0; length; chunks++) {
        nvn_backing_range span; uint64_t consumed;
        if (chunks == 64 || !nvn_backing_prefix(&g_inline_backing, g_inline_pool_generation,
                address, length, &span, &consumed)) return 0;
        unsigned i = 0;
        while (i < g_inline_reads_n && !nvn_backing_key_equal(g_inline_reads[i].key, span.key)) i++;
        if (i == g_inline_reads_n) {
            if (i == 64) return 0;
            g_inline_reads[g_inline_reads_n++] = span;
        } else {
            // Including the gaps between reads is conservative and bounds the
            // proof by backing identities rather than by the number of draws.
            g_inline_reads[i].lo = MIN(g_inline_reads[i].lo, span.lo);
            g_inline_reads[i].hi = MAX(g_inline_reads[i].hi, span.hi);
        }
        address += consumed; length -= consumed;
    }
    return 1;
}
static int inline_texture_private(unsigned slot) {
    if (slot >= NTEX) return 0;
    unsigned tid = (unsigned)(S.tex[1][slot] & 0xfffff); if (!tid) return 1;
    pthread_mutex_lock(&g_rmu);
    int safe = 0;
    if (tid < (unsigned)g_ntreg && g_treg[tid].ok) {
        const tex_t *texture = &g_treg[tid].t;
        // Nonlinear, nonvirtual textures use independent Metal allocations,
        // including packaged uploads. Buffer stores cannot change their bytes.
        safe = texture->pool && !(texture->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) &&
            texture->d.target != NVN_TEXTURE_TARGET_BUFFER &&
            !(texture->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
    }
    pthread_mutex_unlock(&g_rmu); return safe;
}
static int fragment_read_ranges(NvShader *fragment) {
    if (!discard_inline_shape()) return 0;
    for (int i = 0; fragment && i < fragment->rf.nb; i++) {
        const nvn_rbind *binding = &fragment->rf.b[i]; unsigned reg = binding->reg;
        switch (binding->kind) {
            case NVN_RB_CBUF: case NVN_RB_UBOP: case NVN_RB_SBUFP: {
                if (reg >= 16 || storage_usage(fragment, binding) & MTLResourceUsageWrite) return 0;
                const p_bind_buf *buffer = binding->kind == NVN_RB_SBUFP ? &S.ssbo[1][reg] : &S.ubo[1][reg];
                uint64_t length = binding->kind == NVN_RB_CBUF ? cb_len(fragment, reg, buffer->size) : buffer->size;
                if (!inline_read_range(buffer->addr, length)) {
                    PD("inline fragment range unproved key=%s reg=%u addr=%#llx bytes=%llu", fragment->key, reg,
                       (unsigned long long)buffer->addr, (unsigned long long)length);
                    return 0;
                }
                break;
            }
            case NVN_RB_TEX: if (!inline_texture_private(reg)) return 0; break;
            case NVN_RB_DRV0: case NVN_RB_DRV2:
                if (fragment->rf.drv_read[binding->kind == NVN_RB_DRV2 ? 2 : 0]) return 0;
                break;
            case NVN_RB_C1: case NVN_RB_SUPPORT:
            case NVN_RB_SAMP: case NVN_RB_SAMPT: case NVN_RB_SAMPI: case NVN_RB_SAMPU: case NVN_RB_SAMPD:
                break;
            default: return 0;
        }
    }
    return discard_inline_shape();
}
static int inline_write_disjoint(uint64_t address, uint64_t length) {
    if (!address || !length || length > (16u << 20)) return 0;
    for (unsigned chunks = 0; length; chunks++) {
        nvn_backing_range span; uint64_t consumed;
        if (chunks == 64 || !nvn_backing_prefix(&g_inline_backing, g_inline_pool_generation,
                address, length, &span, &consumed)) return 0;
        for (unsigned i = 0; i < g_inline_reads_n; i++)
            if (!nvn_backing_disjoint(g_inline_reads[i], span)) return 0;
        address += consumed; length -= consumed;
    }
    return 1;
}
static int discard_inline_can(NvShader *vertex) {
    if (!g_inline_pass_safe || !vertex || vertex->layer_out || !discard_inline_shape()) {
        if (discard_inline_on()) PD("inline prefix unproved safe=%d encoder=%d mode=%d reads=%u pending=%d",
            g_inline_pass_safe, g_enc != nil, g_enc_mode, g_inline_reads_n, g_vis_pending_n);
        return 0;
    }
    for (int i = 0; i < vertex->rf.nb; i++) {
        const nvn_rbind *binding = &vertex->rf.b[i];
        if ((binding->kind == NVN_RB_DRV0 || binding->kind == NVN_RB_DRV2) &&
            vertex->rf.drv_read[binding->kind == NVN_RB_DRV2 ? 2 : 0]) return 0;
        if (!(storage_usage(vertex, binding) & MTLResourceUsageWrite)) continue;
        if (binding->kind != NVN_RB_SBUFP || binding->reg >= 16) return 0;
        const p_bind_buf *buffer = &S.ssbo[0][binding->reg];
        if (!inline_write_disjoint(buffer->addr, buffer->size)) {
            PD("inline write range unproved key=%s reg=%u addr=%#llx bytes=%llu reads=%u", vertex->key, binding->reg,
               (unsigned long long)buffer->addr, (unsigned long long)buffer->size, g_inline_reads_n);
            return 0;
        }
    }
    return discard_inline_shape();
}
static int discard_inline_keeps(uint32_t operation, const void *arguments) {
    switch (operation) {
        case OP_BEGIN: case OP_END: case OP_JUMP: case OP_CALL:
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_SCISSOR: case OP_STENCIL: case OP_POLY_OFFSET:
        case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
        case OP_RAST_DISCARD: case OP_DRAW: case OP_DRAW_IDX: case OP_SET_RT:
            return 1;
        case OP_BIND_TEX: case OP_BIND_SAMP: return !((const p_bind_handles *)arguments)->defer;
        case OP_BARRIER: {
            uint32_t bits; memcpy(&bits, arguments, sizeof bits);
            return !(bits & ~(NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT |
                NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT));
        }
        default: return 0;
    }
}
