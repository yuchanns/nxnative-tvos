#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t pixel_format;
    uint8_t pixel_bytes, block_bytes;
} nvn_bc_format_info;

int nvn_bc_format(uint32_t format, nvn_bc_format_info *info);
// Decode one plane, with explicit byte bounds and row pitches. Returns 0 on success.
// Source and destination must not overlap. Pixels outside partial edge blocks are not written.
int nvn_bc_decode(uint32_t format, const void *source, size_t source_size, size_t source_pitch,
                  uint32_t width, uint32_t height, void *destination, size_t destination_size, size_t destination_pitch);
