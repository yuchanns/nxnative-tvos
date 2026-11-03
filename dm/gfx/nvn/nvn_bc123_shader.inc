// The integer interpolation matches the repository's bcdec reference decoder.
static id<MTLComputePipelineState> bc123_pipeline(void) {
    static id<MTLComputePipelineState> pipeline;
    if (pipeline) return pipeline;
    const char *source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Params { uint width, height, block_row, pixel_row, kind, unused; };\n"
        "uint u16(device const uchar *b) { return uint(b[0]) | (uint(b[1]) << 8); }\n"
        "kernel void decode_bc123(device const uchar *blocks [[buffer(0)]], device uchar *pixels [[buffer(1)]],"
        "constant Params &p [[buffer(2)]], uint2 xy [[thread_position_in_grid]]) {\n"
        " if (xy.x >= p.width || xy.y >= p.height) return;\n"
        " uint pos = (xy.y & 3) * 4 + (xy.x & 3);\n"
        " device const uchar *b = blocks + (xy.y / 4) * p.block_row + (xy.x / 4) * (p.kind == 1 ? 8 : 16);\n"
        " device const uchar *c = b + (p.kind == 1 ? 0 : 8);\n"
        " uint a = u16(c), d = u16(c + 2), index = (uint(c[4 + pos / 4]) >> ((pos & 3) * 2)) & 3;\n"
        " uint3 x = uint3(a >> 11, (a >> 5) & 63, a & 31), y = uint3(d >> 11, (d >> 5) & 63, d & 31);\n"
        " uint3 rgb; uint alpha = 255;\n"
        " if (index < 2) { uint3 v = index == 0 ? x : y; rgb = (v * uint3(527,259,527) + uint3(23,33,23)) >> 6; }\n"
        " else if (a > d || p.kind != 1) {\n"
        "  uint3 v = index == 2 ? 2 * x + y : x + 2 * y;\n"
        "  rgb = uint3((v.x * 351 + 61) >> 7, (v.y * 2763 + 1039) >> 11, (v.z * 351 + 61) >> 7);\n"
        " } else if (index == 2) { uint3 v = x + y; rgb = uint3((v.x*1053+125)>>8,(v.y*4145+1019)>>11,(v.z*1053+125)>>8); }\n"
        " else { rgb = uint3(0); alpha = 0; }\n"
        " if (p.kind == 2) alpha = ((uint(b[pos / 2]) >> ((pos & 1) * 4)) & 15) * 17;\n"
        " if (p.kind == 3) {\n"
        "  uint bit = pos * 3, i = uint(b[2 + bit / 8]) >> (bit & 7);\n"
        "  if ((bit & 7) > 5) i |= uint(b[3 + bit / 8]) << (8 - (bit & 7));\n"
        "  i &= 7; uint a0 = b[0], a1 = b[1];\n"
        "  if (i < 2) alpha = i == 0 ? a0 : a1;\n"
        "  else if (a0 > a1) alpha = ((8 - i) * a0 + (i - 1) * a1) / 7;\n"
        "  else if (i < 6) alpha = ((6 - i) * a0 + (i - 1) * a1) / 5;\n"
        "  else alpha = i == 6 ? 0 : 255;\n"
        " }\n"
        " uint o = xy.y * p.pixel_row + xy.x * 4;\n"
        " pixels[o] = uchar(rgb.x); pixels[o+1] = uchar(rgb.y); pixels[o+2] = uchar(rgb.z); pixels[o+3] = uchar(alpha);\n"
        "}\n";
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@(source) options:nil error:&error];
    if (!library) MDIE("BC1/2/3 GPU decoder compilation failed: %s", error.localizedDescription.UTF8String);
    pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"decode_bc123"] error:&error];
    if (!pipeline) MDIE("BC1/2/3 GPU decoder pipeline failed: %s", error.localizedDescription.UTF8String);
    MLOG("BC1/BC2/BC3 GPU decoder ready");
    return pipeline;
}
