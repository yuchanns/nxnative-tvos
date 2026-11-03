"""Reject helper and subgroup rewrites without a counter equivalence proof."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from dmsh_legacy import LoweringError, fragment_counters

FIXTURE = Path(__file__).with_name("legacy_fragment.metal").read_text()

class LegacyFragmentTests(unittest.TestCase):
    def test_counter(self):
        source, metadata = fragment_counters(FIXTURE)
        self.assertEqual(metadata["counters"], 1)
        self.assertNotIn("simd_is_helper_thread", source)
        self.assertNotIn("spvSubgroupBallot", source)
        self.assertIn("1u, memory_order_relaxed", source)
        self.assertIn("uint unused = counter + 1;", source)

    def test_varying_address(self):
        with self.assertRaisesRegex(LoweringError, "draw-uniform"):
            fragment_counters(FIXTURE.replace("._m0[counter]", "._m0[uint(pos.x)]"))

    def test_observed_result(self):
        with self.assertRaises(LoweringError):
            fragment_counters(FIXTURE.replace("uint result = old;", "return float4(old);"))

    def test_other_helper_use(self):
        with self.assertRaises(LoweringError):
            fragment_counters(FIXTURE.replace("float(predicate)", "float(gl_HelperInvocation)"))

    def test_other_guard_effect(self):
        with self.assertRaises(LoweringError):
            fragment_counters(FIXTURE.replace("uint mask =", "(*spvDescriptorSet0.fp_s0)._m0[3] = 5;\n            uint mask ="))

    def test_not_a_population_count(self):
        with self.assertRaises(LoweringError):
            fragment_counters(FIXTURE.replace("spvSubgroupBallot(true).x", "spvSubgroupBallot(predicate).x"))

if __name__ == "__main__":
    unittest.main()
