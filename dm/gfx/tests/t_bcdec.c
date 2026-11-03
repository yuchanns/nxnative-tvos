#include "../tex/nvn_bcdec.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; } } while (0)

static void all_formats(void) {
    for (uint32_t format = 0x41; format <= 0x50; format++) {
        nvn_bc_format_info info;
        CHECK(nvn_bc_format(format, &info));
        uint8_t source[129] = {0}, output[1024];
        size_t source_pitch = 2 * info.block_bytes + 5, pitch = 7 * info.pixel_bytes + 3;
        memset(output, 0xa5, sizeof output);
        CHECK(!nvn_bc_decode(format, source + 1, sizeof source - 1, source_pitch, 7, 5, output + 1, sizeof output - 1, pitch));
        CHECK(output[0] == 0xa5);
        for (size_t y = 0; y < 5; y++) {
            for (size_t x = 0; x < 7; x++) {
                const uint8_t *pixel = output + 1 + y * pitch + x * info.pixel_bytes;
                if (format >= 0x4f) {
                    uint16_t rgba[4]; memcpy(rgba, pixel, sizeof rgba);
                    CHECK(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 0x3c00);
                } else {
                    CHECK(pixel[0] == 0);
                    if (info.pixel_bytes >= 2) CHECK(pixel[1] == 0);
                    if (info.pixel_bytes == 4) CHECK(pixel[2] == 0);
                }
            }
            for (size_t x = 7 * info.pixel_bytes; x < pitch; x++) CHECK(output[1 + y * pitch + x] == 0xa5);
        }
        for (size_t i = 1 + 5 * pitch; i < sizeof output; i++) CHECK(output[i] == 0xa5);
    }
}

static void known_values(void) {
    uint8_t source[32] = {0}, output[7 * 5 * 4];
    for (int i = 0; i < 4; i++) { source[i * 8] = 0; source[i * 8 + 1] = 0xf8; }
    CHECK(!nvn_bc_decode(0x42, source, sizeof source, 16, 7, 5, output, sizeof output, 28));
    for (size_t i = 0; i < 7 * 5; i++) CHECK(output[i * 4] == 255 && !output[i * 4 + 1] && !output[i * 4 + 2] && output[i * 4 + 3] == 255);
    source[0] = 0x81; source[1] = 0x7f;
    CHECK(!nvn_bc_decode(0x4a, source, 8, 8, 4, 4, output, sizeof output, 4));
    for (size_t i = 0; i < 16; i++) CHECK(output[i] == 0x81);
}

static void rejected_inputs(void) {
    uint8_t source[64] = {0}, output[128], before[128];
    memset(output, 0xa5, sizeof output); memcpy(before, output, sizeof before);
    CHECK(nvn_bc_decode(0, source, 8, 8, 4, 4, output, 64, 16));
    CHECK(nvn_bc_decode(0x42, source, 7, 8, 4, 4, output, 64, 16));
    CHECK(nvn_bc_decode(0x42, source, 64, 7, 7, 4, output, 128, 28));
    CHECK(nvn_bc_decode(0x42, source, 8, 8, 4, 4, output, 63, 16));
    CHECK(nvn_bc_decode(0x42, source, 8, 8, 4, 4, output, 64, 15));
    CHECK(nvn_bc_decode(0x42, source, 8, 8, 0, 4, output, 64, 16));
    CHECK(nvn_bc_decode(0x42, NULL, 8, 8, 4, 4, output, 64, 16));
    CHECK(nvn_bc_decode(0x42, source, 64, SIZE_MAX, 4, 8, output, 128, 16));
    CHECK(nvn_bc_decode(0x42, source, 16, 8, 4, 8, output, 128, SIZE_MAX));
    CHECK(nvn_bc_decode(0x42, source, 64, 8, UINT_MAX, UINT_MAX, output, 128, 16));
    CHECK(nvn_bc_decode(0x42, source, 8, 8, 4, 4, source, 64, 16));
    CHECK(!memcmp(output, before, sizeof output));
}

int main(void) {
    all_formats(); known_values(); rejected_inputs();
    printf("BC decode checks=%d failed=%d\n", checks, failures);
    return failures != 0;
}
