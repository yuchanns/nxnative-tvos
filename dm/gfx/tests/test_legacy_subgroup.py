"""Check convergence rejection and the production compute collective lowering."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from dmsh_legacy import LoweringError, compute_subgroups


FIXTURE = Path(__file__).with_name("legacy_subgroup.metal").read_text()


class LegacySubgroupTests(unittest.TestCase):
    def test_converged_fixture(self):
        source, metadata = compute_subgroups(FIXTURE, [64, 1, 1])
        self.assertEqual(metadata["width"], 32)
        self.assertEqual(metadata["scratch_bytes"], 2304)
        self.assertNotIn("simd_shuffle(", source)
        self.assertNotIn("simd_ballot(", source)

    def test_divergent_conditional(self):
        source = FIXTURE.replace("uint total = index;", "uint total = index;\n    if (lane != 0)\n    {")
        source = source.replace("output[index * 6 + 1] = total;", "}\n    output[index * 6 + 1] = total;")
        with self.assertRaisesRegex(LoweringError, "Divergent collective"):
            compute_subgroups(source, [64, 1, 1])

    def test_divergent_loop_count(self):
        source = FIXTURE.replace("uint step = 16;", "uint step = lane;")
        with self.assertRaisesRegex(LoweringError, "Divergent collective"):
            compute_subgroups(source, [64, 1, 1])

    def test_divergent_break(self):
        source = FIXTURE.replace("step >>= 1;", "step >>= 1;\n        if (lane == 0)\n        {\n            break;\n        }")
        with self.assertRaisesRegex(LoweringError, "Divergent control transfer"):
            compute_subgroups(source, [64, 1, 1])

    def test_control_dependence(self):
        source = FIXTURE.replace("uint step = 16;", "uint step = 16;\n    if (lane == 0)\n    {\n        step = 8;\n    }")
        with self.assertRaisesRegex(LoweringError, "Divergent collective"):
            compute_subgroups(source, [64, 1, 1])

    def test_incomplete_subgroup(self):
        with self.assertRaisesRegex(LoweringError, "complete subgroups"):
            compute_subgroups(FIXTURE, [17, 1, 1])

    def test_varying_compound_assignment(self):
        for operation in ("&= lane", ">>= lane", "^= lane", "%= lane"):
            source = FIXTURE.replace("uint step = 16;", "uint step = 16;\n    step " + operation + ";")
            with self.subTest(operation=operation), self.assertRaisesRegex(LoweringError, "Divergent collective"):
                compute_subgroups(source, [64, 1, 1])

    def test_conditional_increment(self):
        source = FIXTURE.replace("uint step = 16;", "uint step = 16;\n    if (lane == 0)\n    {\n        --step;\n    }")
        with self.assertRaisesRegex(LoweringError, "Divergent collective"):
            compute_subgroups(source, [64, 1, 1])

    def test_conditional_expression(self):
        source = FIXTURE.replace("uint active = spvSubgroupBallot(true).x;", "uint active = lane ? spvSubgroupBallot(true).x : 0;")
        with self.assertRaisesRegex(LoweringError, "Conditional collective"):
            compute_subgroups(source, [64, 1, 1])

    def test_unknown_collective(self):
        source = FIXTURE.replace("uint total = index;", "uint total = simd_sum(index);")
        with self.assertRaisesRegex(LoweringError, "Unsupported legacy collective"):
            compute_subgroups(source, [64, 1, 1])

    def test_component_assignment(self):
        source = FIXTURE.replace("uint step = 16;", "uint2 counts = uint2(16);\n    counts.x = lane;\n    uint step = counts.x;")
        with self.assertRaisesRegex(LoweringError, "Divergent collective"):
            compute_subgroups(source, [64, 1, 1])

    def test_mutable_helper_output(self):
        source = FIXTURE.replace("kernel void main0", "void vary(thread uint &value) { value = 1; }\n\nkernel void main0")
        source = source.replace("uint step = 16;", "uint step = 16;\n    vary(step);")
        with self.assertRaisesRegex(LoweringError, "Divergent collective"):
            compute_subgroups(source, [64, 1, 1])

    def test_untouched_source(self):
        source = "kernel void main0() {}"
        self.assertEqual(compute_subgroups(source, [1, 1, 1]), (source, None))


if __name__ == "__main__":
    unittest.main()
