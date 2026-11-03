#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// RGBA8 input is tightly packed. Output contains tightly packed 16-byte ASTC
// blocks. Only 4x4 and 6x6 LDR blocks are supported; zero means success.
int nvn_astc_encode_rgba8(const uint8_t *pixels, uint32_t width, uint32_t height,
                         int srgb, unsigned block, uint8_t *output, size_t output_size);
#ifdef __cplusplus
}
#endif
