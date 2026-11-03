#pragma once
#include "nvn_geometry_topology.h"

#define DM_GEOMETRY_INPUT_STRINGIFY(...) #__VA_ARGS__
#define DM_GEOMETRY_INPUT_TEXT(...) DM_GEOMETRY_INPUT_STRINGIFY(__VA_ARGS__)
#define NVN_GEOMETRY_THREAD thread
static const char k_geometry_indirect_msl[] =
    "#include <metal_stdlib>\nusing namespace metal; typedef uint uint32_t;\n"
    DM_GEOMETRY_INPUT_TEXT(NVN_GEOMETRY_TOPOLOGY)
    DM_GEOMETRY_INPUT_TEXT(
struct InputParams { uint bound, primitive, width, output_width; };
kernel void dm_gs_input(const device uint *source [[buffer(0)]],
                       device uint *info [[buffer(1)]],
                       device uint *capture [[buffer(2)]],
                       device uint *remap [[buffer(3)]],
                       device uint *arguments [[buffer(4)]],
                       device uint *indices [[buffer(5)]],
                       constant InputParams &p [[buffer(6)]],
                       device uint *status [[buffer(7)]],
                       uint index [[thread_position_in_grid]]) {
    uint invalid = source[0] > p.bound || source[1] != 1u || source[2] || source[3];
    uint count = invalid ? 0u : source[0];
    uint primitives = nvn_geometry_input_count(p.primitive, count);
    if (!index) {
        status[0] = invalid ? 1u : 0u;
        info[0] = count; info[1] = 1u; info[2] = info[3] = 0u;
        info[4] = info[5] = primitives; info[6] = 0u;
        capture[0] = count; capture[1] = 1u; capture[2] = capture[3] = 0u;
        arguments[0] = primitives * p.output_width; arguments[1] = 1u;
        arguments[2] = arguments[3] = arguments[4] = 0u;
    }
    if (index >= primitives) return;
    uint positions[6];
    nvn_geometry_input_indices(p.primitive, count, index, positions);
    for (uint slot = 0; slot < p.width; ++slot) remap[ulong(index) * p.width + slot] = positions[slot];
    for (uint slot = 0; slot < p.output_width; ++slot) {
        uint selected = p.width == 4u ? slot + 1u : p.width == 6u ? slot * 2u : slot;
        indices[ulong(index) * p.output_width + slot] = positions[selected];
    }
}
);
#undef NVN_GEOMETRY_THREAD
#undef DM_GEOMETRY_INPUT_TEXT
#undef DM_GEOMETRY_INPUT_STRINGIFY
