"""Border-lowering rejection tests and native-versus-emulated GPU fixture."""

import argparse
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import dmsh_border
from dmsh_legacy import LoweringError, compact_samplers


BINDINGS = [
    {"msl_name": "color", "msl_type": "texture2d<float>", "arg_id": 0, "kind": "tex", "reg": 0},
    {"msl_name": "color_sampler", "msl_type": "sampler", "arg_id": 1, "kind": "samp", "reg": 0},
    {"msl_name": "depth", "msl_type": "texture2d_array<float>", "arg_id": 2, "kind": "tex", "reg": 1},
    {"msl_name": "depth_sampler", "msl_type": "sampler", "arg_id": 3, "kind": "samp", "reg": 1},
    {"msl_name": "volume", "msl_type": "texture3d<float>", "arg_id": 4, "kind": "tex", "reg": 2},
    {"msl_name": "output", "msl_type": "device float4*", "arg_id": 5, "kind": "sbuf", "reg": 0, "rw": True},
]

SOURCE = """#include <metal_stdlib>
using namespace metal;
template<typename T> inline depth2d_array<T> spvDepthCast(texture2d_array<T> t) {
    return reinterpret_cast<thread const depth2d_array<T> &>(t);
}
struct spvDescriptorSetBuffer0 {
    texture2d<float> color [[id(0)]];
    sampler color_sampler [[id(1)]];
    texture2d_array<float> depth [[id(2)]];
    sampler depth_sampler [[id(3)]];
    texture3d<float> volume [[id(4)]];
    device float4 *output [[id(5)]];
};
kernel void main0(constant spvDescriptorSetBuffer0 &spvDescriptorSet0 [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    float2 uv = float2(int(i % 13) - 2, int((i / 13) % 13) - 2) / 8.0;
    float lod = float((i / 169) % 7) * 0.5;
    float reference = float(i / (169 * 7)) * 0.25;
    spvDescriptorSet0.output[5*i] = spvDescriptorSet0.color.sample(spvDescriptorSet0.color_sampler, uv, level(lod));
    spvDescriptorSet0.output[5*i+1] = spvDescriptorSet0.color.gather(spvDescriptorSet0.color_sampler, uv, int2(1, -1), component::w);
    spvDescriptorSet0.output[5*i+2] = float4(spvDepthCast(spvDescriptorSet0.depth).sample_compare(spvDescriptorSet0.depth_sampler, uv, i % 2, reference, level(lod)));
    spvDescriptorSet0.output[5*i+3] = spvDepthCast(spvDescriptorSet0.depth).gather_compare(spvDescriptorSet0.depth_sampler, uv, i % 2, reference, int2(0));
    spvDescriptorSet0.output[5*i+4] = spvDescriptorSet0.volume.sample(spvDescriptorSet0.color_sampler, float3(uv, uv.x), level(lod));
}
"""


class BorderTests(unittest.TestCase):
    def test_layout_and_depth_arguments(self):
        source, metadata = dmsh_border.lower(SOURCE, BINDINGS)
        self.assertEqual(metadata["params_arg"], 6)
        self.assertEqual([pair["dimension"] for pair in metadata["pairs"]], [2, 2, 3])
        self.assertIn("dm_border_mask_1.sample(spvDescriptorSet0.depth_sampler, uv, level(lod))", source)
        self.assertIn("dm_border_mask_1.gather(spvDescriptorSet0.depth_sampler, uv, int2(0))", source)
        self.assertIn("dm_border_params[0].delta.w", source)

    def test_variant_keeps_unused_layout(self):
        _, metadata = dmsh_border.lower(SOURCE, BINDINGS)
        variant = SOURCE.replace("spvDescriptorSet0.color.sample(spvDescriptorSet0.color_sampler, uv, level(lod))", "float4(0.0)")
        _, variant_metadata = dmsh_border.lower(variant, BINDINGS, metadata)
        self.assertEqual(metadata, variant_metadata)

    def test_unknown_sampler_rejected(self):
        with self.assertRaises(LoweringError):
            dmsh_border.lower(SOURCE.replace("spvDescriptorSet0.color_sampler, uv", "other.sampler, uv"), BINDINGS)

    def test_duplicate_lowering_rejected(self):
        lowered, _ = dmsh_border.lower(SOURCE, BINDINGS)
        with self.assertRaises(LoweringError):
            dmsh_border.lower(lowered, BINDINGS)

    def test_cube_address_mode_is_ignored(self):
        source = SOURCE.replace("texture2d<float> color", "texturecube<float> color")
        bindings = [dict(binding, msl_type="texturecube<float>") if binding["msl_name"] == "color" else binding for binding in BINDINGS]
        _, metadata = dmsh_border.lower(source, bindings)
        self.assertNotIn(0, [pair["texture_arg"] for pair in metadata["pairs"]])

    def test_compact_table_follows_border_resources(self):
        from test_legacy_samplers import BINDINGS as many_bindings, SOURCE as many_source
        bordered, border_metadata = dmsh_border.lower(many_source, many_bindings)
        _, sampler_metadata = compact_samplers(bordered, many_bindings)
        self.assertGreater(sampler_metadata["table_arg"], max(pair["mask_arg"] for pair in border_metadata["pairs"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", type=Path)
    args, rest = parser.parse_known_args()
    if args.header:
        source, metadata = dmsh_border.lower(SOURCE, BINDINGS)
        rows = ["{" + ", ".join(str(p[key]) for key in ("texture_arg", "sampler_arg", "mask_arg", "dimension")) + "}"
                for p in metadata["pairs"]]
        args.header.parent.mkdir(parents=True, exist_ok=True)
        args.header.write_text("// Generated by test_legacy_borders.py.\n" +
                               "static const char *native_border_source = " + json.dumps(SOURCE) + ";\n" +
                               "static const char *legacy_border_source = " + json.dumps(source) + ";\n" +
                               "static const nvn_border_pair legacy_border_pairs[] = {" + ", ".join(rows) + "};\n" +
                               f"static const unsigned legacy_border_param_arg = {metadata['params_arg']};\n")
    else:
        unittest.main(argv=[sys.argv[0], *rest])
