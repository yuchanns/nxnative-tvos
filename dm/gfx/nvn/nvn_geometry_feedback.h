#pragma once
// Feedback records complete output primitives, including strip vertex duplication.
// https://registry.khronos.org/OpenGL/specs/gl/glspec43.core.pdf (section 11.1.2)
#define DM_GEOMETRY_TF_MSL(...) #__VA_ARGS__
static const char k_geometry_feedback_msl[] = "#include <metal_stdlib>\n" DM_GEOMETRY_TF_MSL(
using namespace metal;
struct Feedback {
    uint words, width, vertices, count;
    uint strides[4], sizes[4], starts[4], map[512];
};
kernel void dm_gs_tf_reserve(const device uint *arguments [[buffer(2)]],
                            device uint *offsets [[buffer(3)]],
                            device Feedback &p [[buffer(4)]]) {
    uint count = min(arguments[0], 0xffffffffu - offsets[4]);
    for (uint b = 0; b < 4; ++b) if (p.strides[b]) {
        uint remaining = offsets[b] <= p.sizes[b] ? p.sizes[b] - offsets[b] : 0;
        count = min(count, remaining / (p.strides[b] * 4u));
    }
    count -= count % p.width;
    p.count = count;
    for (uint b = 0; b < 4; ++b) {
        p.starts[b] = offsets[b] / 4u;
        offsets[b] += count * p.strides[b] * 4u;
    }
    offsets[4] += count;
    offsets[5] = 1u;
    offsets[6] = offsets[7] = 0u;
}
kernel void dm_gs_tf_copy(const device uint *vertices [[buffer(0)]],
                         const device uint *indices [[buffer(1)]],
                         const device Feedback &p [[buffer(4)]],
                         device uint *out0 [[buffer(5)]], device uint *out1 [[buffer(6)]],
                         device uint *out2 [[buffer(7)]], device uint *out3 [[buffer(8)]],
                         uint i [[thread_position_in_grid]]) {
    if (i >= p.count) return;
    uint index = indices[i];
    if (index >= p.vertices) return;
    for (uint b = 0; b < 4; ++b) {
        device uint *output = b == 0 ? out0 : b == 1 ? out1 : b == 2 ? out2 : out3;
        for (uint c = 0; c < p.strides[b]; ++c) {
            uint offset = p.map[b * 128u + c];
            uint value = offset == 0xfffffffeu ? 0x3f800000u : offset < p.words ? vertices[ulong(index) * p.words + offset] : 0u;
            output[p.starts[b] + ulong(i) * p.strides[b] + c] = value;
        }
    }
}
);
#undef DM_GEOMETRY_TF_MSL
