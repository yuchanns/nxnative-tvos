#pragma once
#include <stdint.h>
#include <stdlib.h>

// Whole pixel extents shared by the guest framebuffer and the Metal drawable.
static inline int dm_render_size_parse(const char *text, uint32_t size[2]) {
    if (!text || *text < '0' || *text > '9') return 0;
    char *end;
    unsigned long width = strtoul(text, &end, 10);
    if (*end != 'x' || end[1] < '0' || end[1] > '9') return 0;
    unsigned long height = strtoul(end + 1, &end, 10);
    if (*end || width < 64 || width > 8192 || height < 64 || height > 8192 || width % 8 || height % 8) return 0;
    size[0] = (uint32_t)width; size[1] = (uint32_t)height;
    return 1;
}
