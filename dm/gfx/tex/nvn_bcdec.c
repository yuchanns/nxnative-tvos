#include "nvn_bcdec.h"
#include <string.h>
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../../third_party/bcdec/bcdec.h"

int nvn_bc_format(uint32_t format, nvn_bc_format_info *info) {
    static const nvn_bc_format_info formats[] = {
        {70, 4, 8}, {70, 4, 8}, {70, 4, 16}, {70, 4, 16},
        {71, 4, 8}, {71, 4, 8}, {71, 4, 16}, {71, 4, 16},
        {10, 1, 8}, {12, 1, 8}, {30, 2, 16}, {32, 2, 16},
        {70, 4, 16}, {71, 4, 16}, {115, 8, 16}, {115, 8, 16},
    };
    if (format < 0x41 || format > 0x50) return 0;
    if (info) *info = formats[format - 0x41];
    return 1;
}

static int rows_fit(size_t rows, size_t pitch, size_t bytes, size_t size) {
    return pitch >= bytes && bytes <= size && rows - 1 <= (size - bytes) / pitch;
}

static void decode_block(uint32_t format, const void *source, void *destination, int pitch) {
    switch (format) {
    case 0x41: case 0x42: case 0x45: case 0x46: bcdec_bc1(source, destination, pitch); break;
    case 0x43: case 0x47: bcdec_bc2(source, destination, pitch); break;
    case 0x44: case 0x48: bcdec_bc3(source, destination, pitch); break;
    case 0x49: case 0x4a: bcdec_bc4(source, destination, pitch, format == 0x4a); break;
    case 0x4b: case 0x4c: bcdec_bc5(source, destination, pitch, format == 0x4c); break;
    case 0x4d: case 0x4e: bcdec_bc7(source, destination, pitch); break;
    case 0x4f: case 0x50: {
        uint16_t rgb[16 * 3];
        bcdec_bc6h_half(source, rgb, 12, format == 0x4f);
        for (size_t y = 0; y < 4; y++) {
            uint16_t *row = (uint16_t *)((uint8_t *)destination + y * pitch);
            for (size_t x = 0; x < 4; x++) {
                memcpy(row + x * 4, rgb + (y * 4 + x) * 3, 6);
                row[x * 4 + 3] = 0x3c00;
            }
        }
        break;
    }
    }
}

int nvn_bc_decode(uint32_t format, const void *source, size_t source_size, size_t source_pitch,
                  uint32_t width, uint32_t height, void *destination, size_t destination_size, size_t destination_pitch) {
    nvn_bc_format_info info;
    if (!source || !destination || !width || !height || !nvn_bc_format(format, &info)) return -1;
    size_t columns = width / 4 + (width % 4 != 0), rows = height / 4 + (height % 4 != 0);
    if (columns > SIZE_MAX / info.block_bytes || (size_t)width > SIZE_MAX / info.pixel_bytes) return -1;
    size_t source_row = columns * info.block_bytes, destination_row = (size_t)width * info.pixel_bytes;
    if (!rows_fit(rows, source_pitch, source_row, source_size) || !rows_fit(height, destination_pitch, destination_row, destination_size)) return -1;
    size_t source_span = (rows - 1) * source_pitch + source_row;
    size_t destination_span = ((size_t)height - 1) * destination_pitch + destination_row;
    uintptr_t src_address = (uintptr_t)source, dst_address = (uintptr_t)destination;
    if (dst_address >= src_address ? dst_address - src_address < source_span : src_address - dst_address < destination_span) return -1;
    const uint8_t *src = source;
    uint8_t *dst = destination;
    for (size_t by = 0; by < rows; by++) for (size_t bx = 0; bx < columns; bx++) {
        uint64_t block[2] = {0};
        union { uint64_t align[16]; uint8_t bytes[128]; } pixels;
        memcpy(block, src + by * source_pitch + bx * info.block_bytes, info.block_bytes);
        decode_block(format, block, pixels.bytes, 4 * info.pixel_bytes);
        size_t count_x = width - bx * 4 < 4 ? width - bx * 4 : 4;
        size_t count_y = height - by * 4 < 4 ? height - by * 4 : 4;
        for (size_t y = 0; y < count_y; y++)
            memcpy(dst + (by * 4 + y) * destination_pitch + bx * 4 * info.pixel_bytes,
                   pixels.bytes + y * 4 * info.pixel_bytes, count_x * info.pixel_bytes);
    }
    return 0;
}
