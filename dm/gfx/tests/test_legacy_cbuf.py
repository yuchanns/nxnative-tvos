"""Check bounded constant-buffer lowering and rejection of unproven accesses."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from dmsh_legacy import LoweringError, constant_buffer_indices


BINDINGS = [{"kind": "cbuf", "msl_name": "vp_c3", "msl_type": "constant _16*", "arg_id": 0}]
PREFIX = """#include <metal_stdlib>
using namespace metal;
struct _16 { float4 _m0[4096]; };
struct spvDescriptorSetBuffer0 { constant _16* vp_c3 [[id(0)]]; };
"""
READ = "(*spvDescriptorSet0.vp_c3)._m0"


class ConstantBufferIndexTests(unittest.TestCase):
    def lower(self, expression):
        return constant_buffer_indices(PREFIX + expression, BINDINGS)

    def test_legal_literals_unchanged(self):
        source = PREFIX + f"{READ}[0] + {READ}[4095u] + {READ}[0xff];"
        self.assertEqual(constant_buffer_indices(source, BINDINGS), (source, None))

    def test_outside_extent_and_signed_indices(self):
        for index in ("4096u", "0xffffffffu", "-1", "int(bits)"):
            with self.subTest(index=index):
                source, metadata = self.lower(f"{READ}[{index}].x;")
                self.assertIn(f"{READ}[dm_legacy_cbuf_index(uint({index}), 4096u)].x;", source)
                self.assertEqual(metadata, {"version": 1, "reads": 1})

    def test_nested_reads_and_single_evaluation(self):
        source, metadata = self.lower(f"{READ}[int({READ}[cursor++].x)][component];")
        self.assertEqual(source.count("cursor++"), 1)
        self.assertIn(f"{READ}[dm_legacy_cbuf_index(uint(int({READ}[dm_legacy_cbuf_index(uint(cursor++), 4096u)].x)), 4096u)][component]", source)
        self.assertEqual(metadata["reads"], 2)

    def test_extent_from_declaration(self):
        source = (PREFIX + f"{READ}[index];").replace("[4096]", "[35]")
        lowered, _ = constant_buffer_indices(source, BINDINGS)
        self.assertIn("uint(index), 35u)", lowered)

    def test_comments_do_not_create_accesses(self):
        source = PREFIX + f"// unknown.vp_c3\n{READ}[/* ] */ int(index)].x;"
        lowered, _ = constant_buffer_indices(source, BINDINGS)
        self.assertIn("[dm_legacy_cbuf_index(uint(/* ] */ int(index)), 4096u)]", lowered)

    def test_pointer_escape_rejected(self):
        with self.assertRaisesRegex(LoweringError, "use"):
            self.lower("consume(spvDescriptorSet0.vp_c3);")

    def test_unrecognized_layout_rejected(self):
        for declaration in ("float4 _m0[];", "float4 _m0[0];", "float4 _m0[4096]; uint extra;", "uint _m0[4096];"):
            with self.subTest(declaration=declaration), self.assertRaisesRegex(LoweringError, "array"):
                constant_buffer_indices(PREFIX.replace("float4 _m0[4096];", declaration), BINDINGS)

    def test_reflection_mismatch_rejected(self):
        with self.assertRaisesRegex(LoweringError, "declaration"):
            constant_buffer_indices(PREFIX, [dict(BINDINGS[0], arg_id=1)])

    def test_non_constant_resources_unchanged(self):
        source = PREFIX + "consume(spvDescriptorSet0.vp_c3);"
        self.assertEqual(constant_buffer_indices(source, [dict(BINDINGS[0], kind="sbuf")]), (source, None))

    def test_repeated_lowering_rejected(self):
        source, _ = self.lower(f"{READ}[index];")
        with self.assertRaisesRegex(LoweringError, "already lowered"):
            constant_buffer_indices(source, BINDINGS)


if __name__ == "__main__":
    unittest.main()
