#pragma once

// Slot numbers come from translation metadata; byte offsets come exclusively
// from the device's encoder. Tier 1 resource descriptors are not eight bytes.
static int argument_is_sampler(unsigned kind) {
    return kind == NVN_RB_SAMP || kind == NVN_RB_SAMPT || kind == NVN_RB_SAMPU ||
           kind == NVN_RB_SAMPI || kind == NVN_RB_SAMPD;
}

// Return zero on overflow without producing a partially valid binding table.
static unsigned argument_sampler_map(const uint64_t *slots, const uint16_t *ids, unsigned count,
                                     uint64_t physical[16], uint32_t *mapping) {
    unsigned used = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned index = 0;
        while (index < used && physical[index] != slots[ids[i]]) index++;
        if (index == used) {
            if (used == 16) return 0;
            physical[used++] = slots[ids[i]];
        }
        mapping[i] = index;
    }
    return used;
}

enum { ARGUMENT_BUFFER = 1, ARGUMENT_TEXTURE, ARGUMENT_SAMPLER, ARGUMENT_BATCH_MAX = 32 };
typedef struct {
    __unsafe_unretained id resources[ARGUMENT_BATCH_MAX];
    NSUInteger offsets[ARGUMENT_BATCH_MAX], first;
    unsigned count, kind;
} argument_batch;

static unsigned argument_resource_kind(unsigned kind) {
    switch (kind) {
    case NVN_RB_CBUF: case NVN_RB_SBUFP: case NVN_RB_SBUFU: case NVN_RB_UBOP:
    case NVN_RB_SUPPORT: case NVN_RB_C1: case NVN_RB_DRV0: case NVN_RB_DRV2: case NVN_RB_IMGA:
        return ARGUMENT_BUFFER;
    case NVN_RB_TEX: case NVN_RB_TEXU: case NVN_RB_IMG: case NVN_RB_TBUF: case NVN_RB_TEXD:
        return ARGUMENT_TEXTURE;
    case NVN_RB_SAMP: case NVN_RB_SAMPT: case NVN_RB_SAMPU: case NVN_RB_SAMPI: case NVN_RB_SAMPD:
        return ARGUMENT_SAMPLER;
    default: MDIE("argument encoder does not support binding kind %u", kind); return 0;
    }
}

static void argument_batch_flush(id<MTLArgumentEncoder> encoder, argument_batch *batch) {
    if (!batch->count) return;
    NSRange range = NSMakeRange(batch->first, batch->count);
    switch (batch->kind) {
    case ARGUMENT_BUFFER:
        if (batch->count == 1) [encoder setBuffer:batch->resources[0] offset:batch->offsets[0] atIndex:batch->first];
        else [encoder setBuffers:(id<MTLBuffer> const __unsafe_unretained *)batch->resources offsets:batch->offsets withRange:range];
        break;
    case ARGUMENT_TEXTURE:
        if (batch->count == 1) [encoder setTexture:batch->resources[0] atIndex:batch->first];
        else [encoder setTextures:(id<MTLTexture> const __unsafe_unretained *)batch->resources withRange:range];
        break;
    case ARGUMENT_SAMPLER:
        if (batch->count == 1) [encoder setSamplerState:batch->resources[0] atIndex:batch->first];
        else [encoder setSamplerStates:(id<MTLSamplerState> const __unsafe_unretained *)batch->resources withRange:range];
        break;
    }
    batch->count = 0;
}

static void argument_sampler_table(id<MTLArgumentEncoder> encoder, NSUInteger first,
                                   const uint64_t physical[16], unsigned used) {
    if (!used || used > 16) MDIE("invalid compact sampler count %u", used);
    __unsafe_unretained id<MTLSamplerState> samplers[16];
    for (unsigned i = 0; i < 16; i++)
        samplers[i] = (__bridge id<MTLSamplerState>)(void *)(uintptr_t)physical[i < used ? i : 0];
    [encoder setSamplerStates:samplers withRange:NSMakeRange(first, 16)];
}

static void argument_encode(id<MTLArgumentEncoder> encoder, id<MTLBuffer> destination,
                            NSUInteger offset, const nvn_refl *reflection, const uint64_t *slots,
                            int compact_samplers) {
    if (!encoder || offset > destination.length || encoder.encodedLength > destination.length - offset ||
        (encoder.alignment && offset % encoder.alignment)) MDIE("invalid argument-buffer allocation or alignment");
    memset((uint8_t *)destination.contents + offset, 0, encoder.encodedLength);
    [encoder setArgumentBuffer:destination offset:offset];
    argument_batch batch = {0};
    for (int i = 0; i < reflection->nb; i++) {
        const nvn_rbind *binding = &reflection->b[i];
        if (compact_samplers && argument_is_sampler(binding->kind)) continue;
        unsigned count = binding->n ? binding->n : 1;
        if (binding->aid >= NVN_REFL_MAXARG || count > NVN_REFL_MAXARG - binding->aid)
            MDIE("argument slot %u count %u exceeds the reflection limit", binding->aid, count);
        unsigned kind = argument_resource_kind(binding->kind);
        for (unsigned element = 0; element < count; element++) {
            NSUInteger index = binding->aid + element;
            uint64_t token = slots[index];
            // Keep reflection order and holes intact; only combine adjacent
            // writes that use the same encoder API. The device owns the layout.
            if (batch.count && (batch.kind != kind || index != batch.first + batch.count || batch.count == ARGUMENT_BATCH_MAX))
                argument_batch_flush(encoder, &batch);
            if (!batch.count) { batch.kind = kind; batch.first = index; }
            if (kind == ARGUMENT_BUFFER) {
                NSUInteger buffer_offset = 0;
                id<MTLBuffer> buffer = token ? gpu_buffer_required(token, 1, &buffer_offset) : nil;
                batch.resources[batch.count] = buffer;
                batch.offsets[batch.count] = buffer_offset;
            } else batch.resources[batch.count] = (__bridge id)(void *)(uintptr_t)token;
            batch.count++;
        }
    }
    argument_batch_flush(encoder, &batch);
}
