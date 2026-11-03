"""Exercise offline export specialization and emit fixtures for GPU readback."""

import ast
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import dmsh_fetch
import dmsh_outputs


def source(types=("float", "float2", "uint3", "int4"), colors=(0, 2, 3, 7)):
    members = "\n".join(f"    {ty} m_{i} [[color({c})]];" for i, (ty, c) in enumerate(zip(types, colors)))
    assignments = []
    for i, ty in enumerate(types):
        width = int(ty[-1]) if ty[-1].isdigit() else 1
        values = ", ".join(f"input[{4 * i + lane}]" for lane in range(width))
        bits = values if width == 1 else f"uint{width}({values})"
        assignments.append(f"    out.m_{i} = as_type<{ty}>({bits});")
    return (
        "#include <metal_stdlib>\nusing namespace metal;\n"
        f"struct main0_out\n{{\n{members}\n}};\n"
        "fragment main0_out main0(constant uint *input [[buffer(16)]])\n{\n"
        "    main0_out out = {};\n" + "\n".join(assignments) + "\n"
        "    if (input[16]) return out;\n"
        "    out.m_0 = out.m_0;\n"
        "    return out;\n}\n"
    )


class FragmentOutputTests(unittest.TestCase):
    def test_all_component_types_and_widths(self):
        for scalar in dmsh_outputs.TYPES:
            for width in ("", "2", "3", "4"):
                ty = scalar + width
                with self.subTest(type=ty):
                    result, metadata = dmsh_outputs.specialize(source((ty,), (3,)))
                    self.assertEqual(metadata["fields"], [{"color": 3, "kind": dmsh_outputs.TYPES.index(scalar)}])
                    for other in dmsh_outputs.TYPES:
                        self.assertRegex(result, rf"\b{other}{width} dm_out_color_0_\d")
                    self.assertIn(f"{ty} dm_out_value_0 = {ty}(0)", result)
                    self.assertNotIn("out.m_0", result)

    def test_early_return_keeps_control_flow_and_export_bits(self):
        result, metadata = dmsh_outputs.specialize(source())
        self.assertEqual([field["color"] for field in metadata["fields"]], [0, 2, 3, 7])
        self.assertRegex(result, r"if \(input\[16\]\) \{\s*if \(dm_out_0_0\)")
        self.assertEqual(result.count("as_type<uint2>(dm_out_value_1)"), 2)
        self.assertEqual(result.count("as_type<int3>(dm_out_value_2)"), 2)

    def test_depth_and_sample_mask_are_preserved(self):
        original = source(("float4",), (0,)).replace(
            "float4 m_0 [[color(0)]];",
            "float4 m_0 [[color(0)]]; float depth [[depth(any)]]; uint mask [[sample_mask]];",
        ).replace("out.m_0 = out.m_0;", "out.depth = 0.5; out.mask = 3u;")
        result, _ = dmsh_outputs.specialize(original)
        self.assertIn("float depth [[depth(any)]]", result)
        self.assertIn("out.depth = 0.5; out.mask = 3u;", result)

    def test_output_mapping_variant_uses_physical_field_order(self):
        original = source(("float4", "uint4"), (0, 3))
        mapped, fetch = dmsh_fetch.variant(original, [], om_only_ok=True)
        self.assertIsNotNone(mapped)
        result, metadata = dmsh_outputs.specialize(mapped, conditional=True)
        self.assertEqual(fetch["outputs"], [{"color": 0, "type": "float4"}, {"color": 3, "type": "uint4"}])
        self.assertEqual(len(metadata["fields"]), 16)
        for index, field in enumerate(metadata["fields"]):
            self.assertEqual(field["color"], index % 8)
            self.assertEqual(field["condition"], f"OM_{index // 8}_{index % 8}")
        self.assertIn("function_constant(175)", result)
        self.assertIn("if (OM_1_7) dm_out_value_15 = spv_o_m_1", result)

    def test_no_color_export_is_unchanged(self):
        for original in ("fragment void main0() {}", "struct main0_out { float depth [[depth(any)]]; };"):
            self.assertEqual(dmsh_outputs.specialize(original), (original, None))

    def test_unsupported_or_conflicting_input_is_rejected(self):
        original = source(("float4",), (0,))
        bad_inputs = [
            original.replace("float4 m_0", "half4 m_0"),
            original.replace("color(0)", "color(8)"),
            original.replace("using namespace metal;", "using namespace metal;\nconstant bool x [[function_constant(128)]];"),
            original.replace("main0_out out = {};", "main0_out out;"),
            original.replace("return out;", "return main0_out{};"),
            original.replace("m_0", "dm_out_existing"),
            original.replace("color(0)", "color(0), index(1)"),
        ]
        for bad_input in bad_inputs:
            with self.subTest(source=bad_input), self.assertRaises(ValueError):
                dmsh_outputs.specialize(bad_input)

    def test_cas_hit_uses_original_analysis_source_for_fetch(self):
        path = Path(__file__).resolve().parents[1] / "tools" / "dmsh_build.py"
        tree = ast.parse(path.read_text())
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "fin")
        import os
        with tempfile.TemporaryDirectory() as directory:
            key = "fixture"
            original = source(("float4",), (0,))
            result, _ = dmsh_outputs.specialize(original)
            Path(directory, key + "_b.source.metal").write_text(original)
            Path(directory, key + "_b.metal").write_text(result)
            Path(directory, key + "_b.json").write_text(json.dumps({"stage": "ps"}))
            seen = []
            namespace = {"cache_hits": {key}, "out": directory, "os": os, "json": json,
                         "fetch_finish": lambda k, s, r: seen.append(s)}
            exec(compile(ast.Module(body=[function], type_ignores=[]), str(path), "exec"), namespace)
            namespace["fin"](key)
            self.assertEqual(seen, [original])
            self.assertIsNotNone(dmsh_fetch.variant(seen[0], [], om_only_ok=True)[0])


def header(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    original = source(("float", "float2", "uint4", "int4"))
    specialized, metadata = dmsh_outputs.specialize(original)
    mapped, fetch = dmsh_fetch.variant(original, [], om_only_ok=True)
    mapped, mapped_metadata = dmsh_outputs.specialize(mapped, conditional=True)
    contents = "// Generated by test_fragment_outputs.py with the production generators.\n"
    for name, value in (("source", specialized), ("metadata", json.dumps(metadata)),
                        ("mapped_source", mapped), ("fetch", json.dumps(fetch)),
                        ("mapped_metadata", json.dumps(mapped_metadata))):
        contents += f"static const char output_{name}[] = {json.dumps(value)};\n"
    path.write_text(contents)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--header":
        header(Path(sys.argv[2]))
    else:
        unittest.main()
