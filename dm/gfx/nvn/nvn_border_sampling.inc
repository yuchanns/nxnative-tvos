typedef struct { float delta[4]; uint32_t compare, enabled, padding[2]; } nvn_border_info;
_Static_assert(sizeof(nvn_border_info) == 32, "border parameter ABI");

static unsigned border_channels(MTLPixelFormat format) {
    switch (format) {
        case MTLPixelFormatA8Unorm: return 8;
        case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB: case MTLPixelFormatR8Snorm:
        case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint:
        case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Float:
        case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
        case MTLPixelFormatR32Float: case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
        case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float: case MTLPixelFormatDepth32Float_Stencil8:
        case MTLPixelFormatStencil8: case MTLPixelFormatX32_Stencil8:
        case MTLPixelFormatBC4_RUnorm: case MTLPixelFormatBC4_RSnorm: return 1;
        case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm:
        case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
        case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Float:
        case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint:
        case MTLPixelFormatRG32Float: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
        case MTLPixelFormatBC5_RGUnorm: case MTLPixelFormatBC5_RGSnorm: return 3;
        case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float:
        case MTLPixelFormatBC6H_RGBFloat: case MTLPixelFormatBC6H_RGBUfloat: return 7;
        default: return 15;
    }
}

static id<MTLTexture> border_mask(id<MTLTexture> texture, unsigned dimension) {
    static NSMutableDictionary<NSData *, id<MTLTexture>> *cache;
    static size_t bytes;
    if (!cache) cache = [NSMutableDictionary dictionary];
    NSUInteger w = texture.width, h = dimension >= 2 ? texture.height : 1;
    NSUInteger depth = dimension == 3 ? texture.depth : 1, levels = texture.mipmapLevelCount;
    if (!w || !h || !depth || !levels) MDIE("invalid texture dimensions for sampler border coverage");
    uint64_t fields[] = {dimension, w, h, depth, levels};
    NSData *key = [NSData dataWithBytes:fields length:sizeof fields];
    id<MTLTexture> result = cache[key]; if (result) return result;
    size_t cost = 0;
    for (NSUInteger level = 0; level < levels; level++) cost += MAX(w >> level, 1) * MAX(h >> level, 1) * MAX(depth >> level, 1);
    if (cache.count && (cache.count >= 128 || cost > (32u << 20) || bytes > (32u << 20) - cost)) {
        for (id<MTLTexture> old in cache.allValues) defer_obj_release(old);
        [cache removeAllObjects]; bytes = 0;
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = dimension == 3 ? MTLTextureType3D : dimension == 2 ? MTLTextureType2D : MTLTextureType1D;
    d.pixelFormat = MTLPixelFormatR8Unorm; d.width = w; d.height = h; d.depth = depth; d.mipmapLevelCount = levels;
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    result = [g_dev newTextureWithDescriptor:d]; if (!result) MDIE("cannot allocate sampler border coverage texture");
    uint8_t *ones = malloc(w * h); if (!ones) MDIE("cannot allocate sampler border coverage staging");
    memset(ones, 255, w * h);
    for (NSUInteger level = 0; level < levels; level++) {
        NSUInteger lw = MAX(w >> level, 1), lh = MAX(h >> level, 1), ld = MAX(depth >> level, 1);
        for (NSUInteger z = 0; z < ld; z++)
            [result replaceRegion:MTLRegionMake3D(0, 0, z, lw, lh, 1) mipmapLevel:level slice:0
                        withBytes:ones bytesPerRow:dimension > 1 ? lw : 0 bytesPerImage:dimension == 3 ? lw * lh : 0];
    }
    free(ones); res_add(result); cache[key] = result; bytes += cost;
    return result;
}

static void border_encode(NvShader *shader, id<MTLCommandEncoder> encoder, int stage, int metal_vertex,
                          const uint64_t *slots, const int *sampler_ids, id<MTLBuffer> arguments, size_t offset) {
    id<MTLBuffer> parameters; size_t parameter_offset = arena_alloc(shader->border_count * sizeof(nvn_border_info), &parameters);
    nvn_border_info *info = (nvn_border_info *)((uint8_t *)parameters.contents + parameter_offset);
    memset(info, 0, shader->border_count * sizeof(*info));
    [shader->ae setArgumentBuffer:arguments offset:offset];
    pthread_mutex_lock(&g_rmu);
    for (unsigned i = 0; i < shader->border_count; i++) {
        const nvn_border_pair *pair = &shader->border_pairs[i];
        id<MTLTexture> texture = (__bridge id<MTLTexture>)(void *)(uintptr_t)slots[pair->texture];
        int sid = sampler_ids[pair->sampler]; samp_par p = {0};
        if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) p = g_sreg[sid];
        int wraps[] = {p.ws, p.wt, p.wr}, active = 0;
        for (unsigned axis = 0; axis < pair->dimension; axis++) active |= wrapm(wraps[axis]) == MTLSamplerAddressModeClampToBorderColor;
        active &= g_nvn_legacy_border && p.border[3] > 0.5f;
        id<MTLTexture> mask = nil;
        if (active) {
            if (p.reduction) MDIE("sampler border coverage does not support minimum/maximum reduction");
            mask = border_mask(texture, pair->dimension);
            float color[] = {p.border[0] > 0.5f, p.border[0] > 0.5f, p.border[0] > 0.5f, 1};
            // Missing format channels are constants even outside the texture.
            // Apply that expansion before the texture view's channel swizzle.
            unsigned present_channels = border_channels(texture.pixelFormat);
            for (unsigned channel = 0; channel < 4; channel++) if (!(present_channels >> channel & 1)) color[channel] = 0;
            MTLTextureSwizzleChannels swizzle = texture.swizzle;
            MTLTextureSwizzle channels[] = {swizzle.red, swizzle.green, swizzle.blue, swizzle.alpha};
            for (unsigned channel = 0; channel < 4; channel++) {
                MTLTextureSwizzle from = channels[channel];
                info[i].delta[channel] = from >= MTLTextureSwizzleRed && from <= MTLTextureSwizzleAlpha ? color[from - MTLTextureSwizzleRed] : 0;
            }
            info[i].compare = p.cmp_mode ? p.cmp_func - 1 : MTLCompareFunctionNever;
            info[i].enabled = 1;
        } else mask = dummy_tex(pair->dimension == 3 ? MTLTextureType3D : pair->dimension == 2 ? MTLTextureType2D : MTLTextureType1D, 0);
        [shader->ae setTexture:mask atIndex:pair->mask];
        shader_use(encoder, stage, metal_vertex, mask, MTLResourceUsageRead);
    }
    pthread_mutex_unlock(&g_rmu);
    [shader->ae setBuffer:parameters offset:parameter_offset atIndex:shader->border_param_arg];
    shader_use(encoder, stage, metal_vertex, parameters, MTLResourceUsageRead);
}
