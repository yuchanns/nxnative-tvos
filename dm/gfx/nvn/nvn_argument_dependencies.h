// Bound slots that can change the arguments resolved by encode_args_.
#pragma once
#include "nvn_refl.h"
#include <string.h>

enum nvn_argument_binding {
    NVN_ARGUMENT_UBO, NVN_ARGUMENT_SSBO, NVN_ARGUMENT_TEXTURE,
    NVN_ARGUMENT_SAMPLER, NVN_ARGUMENT_IMAGE
};
typedef struct {
    uint64_t ubo, ssbo, texture[4], sampler[2], image;
} nvn_argument_dependencies;

static inline void nvn_argument_dependency_add(uint64_t *bits, unsigned limit, unsigned slot) {
    if (slot < limit) bits[slot / 64] |= UINT64_C(1) << (slot % 64);
}

static inline nvn_argument_dependencies nvn_argument_dependencies_make(const nvn_refl *reflection, int stage) {
    nvn_argument_dependencies dependencies = {0};
    if (!reflection || reflection->nb < 0 || reflection->nb > NVN_REFL_MAXB || stage < 0 || stage >= 6) {
        memset(&dependencies, 0xff, sizeof dependencies);
        return dependencies;
    }
    for (int i = 0; i < reflection->nb; i++) {
        const nvn_rbind *binding = &reflection->b[i];
        unsigned slot = binding->reg;
        switch (binding->kind) {
        case NVN_RB_CBUF: case NVN_RB_UBOP: case NVN_RB_TEXU: case NVN_RB_SAMPU: case NVN_RB_SBUFU:
            nvn_argument_dependency_add(&dependencies.ubo, 16, slot); break;
        case NVN_RB_SBUFP:
            nvn_argument_dependency_add(&dependencies.ssbo, 16, slot); break;
        case NVN_RB_TEX: case NVN_RB_TBUF: case NVN_RB_SAMPT:
            nvn_argument_dependency_add(dependencies.texture, 256, slot); break;
        case NVN_RB_SAMP:
            nvn_argument_dependency_add(dependencies.sampler, 128, slot); break;
        case NVN_RB_IMG: case NVN_RB_IMGA:
            nvn_argument_dependency_add(&dependencies.image, 32, slot); break;
        case NVN_RB_TEXD:
            for (unsigned k = 0; k < binding->n && k < 128; k++) {
                nvn_argument_dependency_add(dependencies.texture, 256, k);
                nvn_argument_dependency_add(dependencies.texture, 256, 128 + k);
            }
            break;
        case NVN_RB_SAMPD:
            for (unsigned k = 0; k < binding->n; k++) {
                nvn_argument_dependency_add(dependencies.sampler, 128, k);
                nvn_argument_dependency_add(dependencies.texture, 256, k);
            }
            break;
        case NVN_RB_DRV0:
            if (reflection->drv_read[0]) {
                dependencies.ssbo = UINT16_MAX;
                if (stage == 5) dependencies.ubo = UINT16_MAX;
            }
            break;
        case NVN_RB_DRV2:
            if (reflection->drv_read[2]) {
                // Driver tables include overlapping combined/image slots and
                // dynamic fallback handles; keep all handle tables dependent.
                memset(dependencies.texture, 0xff, sizeof dependencies.texture);
                memset(dependencies.sampler, 0xff, sizeof dependencies.sampler);
                dependencies.image = UINT32_MAX;
            }
            break;
        case NVN_RB_C1: case NVN_RB_SUPPORT: case NVN_RB_SAMPI:
            // Constant sampler IDs still follow the global registration epoch.
            break;
        default:
            memset(&dependencies, 0xff, sizeof dependencies);
            return dependencies;
        }
    }
    return dependencies;
}

static inline int nvn_argument_binding_used(const nvn_argument_dependencies *dependencies,
        enum nvn_argument_binding kind, int first, int count) {
    const uint64_t *bits;
    int limit;
    if (!dependencies) return 1;
    switch (kind) {
    case NVN_ARGUMENT_UBO: bits = &dependencies->ubo; limit = 16; break;
    case NVN_ARGUMENT_SSBO: bits = &dependencies->ssbo; limit = 16; break;
    case NVN_ARGUMENT_TEXTURE: bits = dependencies->texture; limit = 256; break;
    case NVN_ARGUMENT_SAMPLER: bits = dependencies->sampler; limit = 128; break;
    case NVN_ARGUMENT_IMAGE: bits = &dependencies->image; limit = 32; break;
    default: return 1;
    }
    if (first < 0 || count < 0 || first > limit || count > limit - first) return 1;
    while (count) {
        unsigned bit = (unsigned)first % 64;
        unsigned width = (unsigned)count < 64 - bit ? (unsigned)count : 64 - bit;
        uint64_t mask = (UINT64_MAX >> (64 - width)) << bit;
        if (bits[first / 64] & mask) return 1;
        first += (int)width; count -= (int)width;
    }
    return 0;
}
