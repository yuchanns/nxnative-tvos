"""Quad exchange is valid only when the original target stays in the same quad."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from dmsh_legacy import LoweringError, fragment_quad_shuffles
FIXTURE = Path(__file__).with_name("legacy_quad.metal").read_text()

class LegacyQuadTests(unittest.TestCase):
    def test_quad_local(self):
        source, metadata = fragment_quad_shuffles(FIXTURE)
        self.assertEqual(metadata["calls"], 1)
        self.assertIn("quad_shuffle", source)
        self.assertNotIn("simd_shuffle", source)

    def test_cross_quad(self):
        with self.assertRaises(LoweringError):
            fragment_quad_shuffles(FIXTURE.replace("int mask = 1;", "int mask = 4;"))

    def test_dynamic_target(self):
        with self.assertRaises(LoweringError):
            fragment_quad_shuffles(FIXTURE.replace("int mask = 1;", "int mask = int(pos.x);"))

    def test_other_invocation(self):
        source = FIXTURE.replace("int exchanged =", "int wide = 8;\n    int other = exchange(value, wide, gl_SubgroupInvocationID);\n    int exchanged =")
        with self.assertRaises(LoweringError):
            fragment_quad_shuffles(source)

if __name__ == "__main__":
    unittest.main()
