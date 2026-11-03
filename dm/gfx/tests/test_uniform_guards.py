"""Uniform guards inspect compiled shader sources and fail closed on bounds."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class UniformGuardTests(unittest.TestCase):
    def guard(self, sources, reflections):
        script = Path(__file__).resolve().parents[1] / "tools/c1_guard.py"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "b-fixture"
            root.mkdir()
            for name, text in sources.items():
                (root / name).write_text(text)
            for key, byte_count in reflections.items():
                (root / (key + "_b.json")).write_text(json.dumps({"c1": "00" * byte_count}))
            output = Path(temporary) / "c1_full.tsv"
            subprocess.run([sys.executable, str(script), str(root), str(output)],
                           check=True, capture_output=True, text=True)
            lines = output.read_text().splitlines()
            self.assertTrue(lines[0].startswith("# chain b-fixture "))
            return lines[1:]

    def test_analysis_and_variant_sources_do_not_create_guard_keys(self):
        sources = {
            "1111111111111111_b.metal": "float x = (*spvDescriptorSet0.fp_c1)._m0[0];",
            "1111111111111111_b.source.metal": "float x = (*spvDescriptorSet0.fp_c1)._m0[dynamic_index];",
            "1111111111111111_bf.metal": "float x = (*spvDescriptorSet0.fp_c1)._m0[dynamic_index];",
            "diagnostic.metal": "float x = (*spvDescriptorSet0.fp_c1)._m0[dynamic_index];",
        }
        self.assertEqual(self.guard(sources, {"1111111111111111": 256}), [])

    def test_full_vector_must_fit_data_and_zero_gap(self):
        for stage in ("vp", "fp", "cp", "gp"):
            with self.subTest(stage=stage):
                sources = {
                    "1111111111111111_b.metal": f"float x = (*spvDescriptorSet0.{stage}_c1)._m0[31];",
                    "2222222222222222_b.metal": f"float x = (*spvDescriptorSet0.{stage}_c1)._m0[32];",
                }
                self.assertEqual(self.guard(sources, {"1111111111111111": 256, "2222222222222222": 256}),
                                 ["2222222222222222"])

    def test_missing_reflection_cannot_prove_a_shared_bank_safe(self):
        self.assertEqual(self.guard({"1111111111111111_b.metal": "float x = (*spvDescriptorSet0.fp_c1)._m0[0];"}, {}),
                         ["1111111111111111"])


if __name__ == "__main__":
    unittest.main()
