#include "gen/legacy_borders.h"

static void legacy_border_sampling_format(MTLPixelFormat format, unsigned channels) {
    if (![g_dev supportsFamily:MTLGPUFamilyApple7] && ![g_dev supportsFamily:MTLGPUFamilyMac2]) return;
    const unsigned count = 169 * 7 * 5, output_bytes = count * 5 * 16;
    NvShader *shaders[2]; id<MTLComputePipelineState> pipelines[2]; id<MTLBuffer> outputs[2], arguments[2];
    for (unsigned mode = 0; mode < 2; mode++) {
        NSError *error = nil;
        id<MTLLibrary> library = [g_dev newLibraryWithSource:@(mode ? legacy_border_source : native_border_source) options:nil error:&error];
        if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
        NvShader *shader = [NvShader new]; shaders[mode] = shader; shader->stage = 5;
        shader->fn = [library newFunctionWithName:@"main0"];
        shader->ae = [shader->fn newArgumentEncoderWithBufferIndex:0]; shader->arg_len = shader->ae.encodedLength;
        shader->rf.nb = 6;
        shader->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .aid = 0, .dtt = MTLTextureType2D};
        shader->rf.b[1] = (nvn_rbind){.kind = NVN_RB_SAMP, .aid = 1};
        shader->rf.b[2] = (nvn_rbind){.kind = NVN_RB_TEX, .aid = 2, .dtt = MTLTextureType2DArray};
        shader->rf.b[3] = (nvn_rbind){.kind = NVN_RB_SAMP, .aid = 3, .reg = 1};
        shader->rf.b[4] = (nvn_rbind){.kind = NVN_RB_TEX, .aid = 4, .dtt = MTLTextureType3D};
        shader->rf.b[5] = (nvn_rbind){.kind = NVN_RB_SBUFP, .aid = 5, .rw = 1};
        if (mode) {
            shader->border_count = sizeof legacy_border_pairs / sizeof legacy_border_pairs[0];
            shader->border_param_arg = legacy_border_param_arg;
            memcpy(shader->border_pairs, legacy_border_pairs, sizeof legacy_border_pairs);
        }
        pipelines[mode] = [g_dev newComputePipelineStateWithFunction:shader->fn error:&error];
        if (!pipelines[mode]) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
        outputs[mode] = [g_dev newBufferWithLength:output_bytes options:MTLResourceStorageModeShared];
        arguments[mode] = [g_dev newBufferWithLength:shader->arg_len options:MTLResourceStorageModeShared];
        res_add(outputs[mode]); res_add(arguments[mode]);
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:8 height:8 mipmapped:YES];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> color = [g_dev newTextureWithDescriptor:d]; uint8_t pixels[256];
    for (unsigned level = 0; level < 4; level++) {
        unsigned width = 8 >> level; for (unsigned i = 0; i < channels * width * width; i++) pixels[i] = (i * 37 + 17 * level) % 256;
        [color replaceRegion:MTLRegionMake2D(0, 0, width, width) mipmapLevel:level withBytes:pixels bytesPerRow:width * channels];
    }
    id<MTLTexture> views[3] = {color, nil, nil};
    for (unsigned view = 1; view < 3; view++) {
        MTLTextureSwizzleChannels swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed,
            view == 1 ? MTLTextureSwizzleZero : MTLTextureSwizzleRed, view == 1 ? MTLTextureSwizzleZero : MTLTextureSwizzleRed, MTLTextureSwizzleOne);
        views[view] = [color newTextureViewWithPixelFormat:color.pixelFormat textureType:color.textureType levels:NSMakeRange(0, 4) slices:NSMakeRange(0, 1) swizzle:swizzle];
    }
    d.textureType = MTLTextureType3D; d.width = d.height = d.depth = 4; d.mipmapLevelCount = 3;
    id<MTLTexture> volume = [g_dev newTextureWithDescriptor:d];
    for (unsigned level = 0; level < 3; level++) {
        unsigned width = 4 >> level;
        [volume replaceRegion:MTLRegionMake3D(0, 0, 0, width, width, width) mipmapLevel:level slice:0
                    withBytes:pixels bytesPerRow:width * channels bytesPerImage:width * width * channels];
    }
    d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:8 height:8 mipmapped:YES];
    d.textureType = MTLTextureType2DArray; d.arrayLength = 2; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> depth = [g_dev newTextureWithDescriptor:d];
    id<MTLCommandBuffer> upload = [g_q commandBuffer]; id<MTLBlitCommandEncoder> blit = [upload blitCommandEncoder];
    for (unsigned layer = 0; layer < 2; layer++) for (unsigned level = 0; level < 4; level++) {
        unsigned width = 8 >> level; id<MTLBuffer> data = [g_dev newBufferWithLength:256 * width options:MTLResourceStorageModeShared];
        for (unsigned y = 0; y < width; y++) for (unsigned x = 0; x < width; x++)
            ((float *)((uint8_t *)data.contents + y * 256))[x] = (float)((x + y * 3 + level + layer) % 7) / 6;
        [blit copyFromBuffer:data sourceOffset:0 sourceBytesPerRow:256 sourceBytesPerImage:256 * width sourceSize:MTLSizeMake(width, width, 1)
                  toTexture:depth destinationSlice:layer destinationLevel:level destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    [blit endEncoding]; [upload commit]; [upload waitUntilCompleted];
    EXPECT("border fixture upload succeeds", MTLCommandBufferStatusCompleted, upload.status);
    for (id<MTLTexture> texture in @[views[0], views[1], views[2], depth, volume]) res_add(texture);
    int original_legacy = g_nvn_legacy_border; unsigned sample = 0, printed = 0; float maximum_error = 0;
    for (unsigned compare = 1; compare <= 8; compare++)
    for (unsigned white = 0; white < 2; white++) for (unsigned filter = 0; filter < 4; filter++)
    for (unsigned view = 0; view < 3; view++) for (unsigned mixed = 0; mixed < 2; mixed++, sample++) {
        for (unsigned mode = 0; mode < 2; mode++) {
            g_nvn_legacy_border = mode;
            int sid = 900 + 4 * sample + 2 * mode;
            samp_par p = {.magf = filter & 1, .minf = (filter & 2) ? ((filter & 1) ? 5 : 4) : ((filter & 1) ? 3 : 2),
                .ws = 5, .wt = mixed ? 1 : 5, .wr = mixed ? 7 : 5, .lod_max = 3, .aniso = 1, .border = {white, white, white, 1}};
            mtl_reg_samp(sid, &p); p.cmp_mode = 1; p.cmp_func = compare; mtl_reg_samp(sid + 1, &p);
            pthread_mutex_lock(&g_rmu); id<MTLSamplerState> color_sampler = samp_by_id(sid), depth_sampler = samp_by_id(sid + 1); pthread_mutex_unlock(&g_rmu);
            uint64_t slots[NVN_REFL_MAXARG] = {0}; int ids[NVN_REFL_MAXARG] = {0};
            slots[0] = (uint64_t)(uintptr_t)(__bridge void *)views[view]; slots[1] = (uint64_t)(uintptr_t)(__bridge void *)color_sampler;
            slots[2] = (uint64_t)(uintptr_t)(__bridge void *)depth; slots[3] = (uint64_t)(uintptr_t)(__bridge void *)depth_sampler;
            slots[4] = (uint64_t)(uintptr_t)(__bridge void *)volume; slots[5] = buffer_gpu_address(outputs[mode]); ids[1] = sid; ids[3] = sid + 1;
            mtl_sub_begin(); id<MTLComputeCommandEncoder> encoder = enc_c_begin(nil); enc_c_pso(encoder, pipelines[mode]);
            argument_encode(shaders[mode]->ae, arguments[mode], 0, &shaders[mode]->rf, slots, 0);
            if (mode) border_encode(shaders[mode], encoder, 5, -1, slots, ids, arguments[mode], 0);
            for (id object in @[views[view], depth, volume]) shader_use(encoder, 5, -1, object, MTLResourceUsageRead);
            shader_use(encoder, 5, -1, outputs[mode], MTLResourceUsageWrite);
            [encoder setBuffer:arguments[mode] offset:0 atIndex:0];
            [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; enc_c_end(encoder);
            EXPECT("border fixture completes", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
            EXPECT("border fixture GPU succeeds", 0, mtl_last_commit_error());
        }
        const float *native = outputs[0].contents, *emulated = outputs[1].contents;
        unsigned mismatches = 0;
        for (unsigned i = 0; i < output_bytes / sizeof(float); i++) {
            float error = fabsf(native[i] - emulated[i]); maximum_error = MAX(maximum_error, error);
            // Native fixed-point filters round before the border contribution.
            // The tolerance is less than one quarter of a UNORM8 step.
            if (!isfinite(error) || error > 1.0f / 1024) {
                mismatches++;
                if (printed++ < 8) fprintf(stderr, "border mismatch format=%lu compare=%u white=%u filter=%u view=%u mixed=%u index=%u native=%g emulated=%g\n",
                    (unsigned long)format, compare, white, filter, view, mixed, i, native[i], emulated[i]);
            }
        }
        EXPECT("emulated border matches native filter precision", 0, mismatches);
    }
    fprintf(stderr, "border sampling format %lu maximum error: %.9g\n", (unsigned long)format, maximum_error);
    g_nvn_legacy_border = original_legacy; mtl_drain(); state_reset();
    for (id object in @[views[0], views[1], views[2], depth, volume, outputs[0], outputs[1], arguments[0], arguments[1]]) res_remove(object);
}

static void legacy_border_sampling(void) {
    legacy_border_sampling_format(MTLPixelFormatRGBA8Unorm, 4);
    legacy_border_sampling_format(MTLPixelFormatR8Unorm, 1);
    legacy_border_sampling_format(MTLPixelFormatRG8Unorm, 2);
}
