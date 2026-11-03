#include "nvn_astc.h"
#include "../../third_party/astc-encoder/astcenc.h"
#include <mutex>

namespace {
struct Codec {
    std::mutex mutex;
    astcenc_context *context;
    Codec() : context(NULL) {}
    ~Codec() { if (context) astcenc_context_free(context); }
};
Codec codecs[2][2];
}

int nvn_astc_encode_rgba8(const uint8_t *pixels, uint32_t width, uint32_t height,
                         int srgb, unsigned block, uint8_t *output, size_t output_size) {
    if (!pixels || !output || !width || !height || width > 16384 || height > 16384 ||
        (block != 4 && block != 6)) return -1;
    uint64_t required = ((uint64_t)width + block - 1) / block * ((height + block - 1) / block) * 16;
    if (required != output_size) return -1;
    Codec &codec = codecs[block == 6][srgb != 0];
    std::lock_guard<std::mutex> lock(codec.mutex);
    astcenc_error result;
    if (!codec.context) {
        astcenc_config config;
        result = astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR,
            block, block, 1, ASTCENC_PRE_FASTEST, 0, &config);
        if (result != ASTCENC_SUCCESS) return result;
        result = astcenc_context_alloc(&config, 1, &codec.context, nullptr);
        if (result != ASTCENC_SUCCESS) return result;
    }
    void *plane = const_cast<uint8_t *>(pixels);
    astcenc_image image = {width, height, 1, ASTCENC_TYPE_U8, &plane};
    const astcenc_swizzle swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    result = astcenc_compress_image(codec.context, &image, &swizzle, output, output_size, 0);
    astcenc_compress_reset(codec.context);
    return result;
}
