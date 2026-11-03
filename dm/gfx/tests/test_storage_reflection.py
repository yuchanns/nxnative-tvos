"""Storage write usage follows the translator's BufferUsageFlags contract."""
import ast
from pathlib import Path
import re
import unittest


def reflect_storage(stage, cb_slot, byte_offset, flags):
    path = Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py'
    tree = ast.parse(path.read_text())
    function = next(node for node in tree.body
                    if isinstance(node, ast.FunctionDef) and node.name == 'reflect')
    namespace = {'re': re, 'SSBO': {'vs': 0x110, 'cs': 0x310, 'ps': 0x510}}
    exec(compile(ast.Module(body=[function], type_ignores=[]), str(path), 'exec'), namespace)
    source = '''struct spvDescriptorSetBuffer0 {
        device Storage* vp_s0 [[id(0)]];
    };'''
    resources = {'sbufs': [{'binding': 0, 'cb_slot': cb_slot,
                            'cb_offset': byte_offset // 4, 'flags': flags}]}
    return namespace['reflect']('storage-fixture', stage, source, resources)[0]


class StorageReflectionTests(unittest.TestCase):
    def test_written_storage_uses_low_flag_bit(self):
        for stage, offset in (('vs', 0x110), ('ps', 0x510), ('cs', 0x310)):
            with self.subTest(stage=stage):
                binding = reflect_storage(stage, 0, offset, 1)
                self.assertEqual((binding['kind'], binding['reg']), ('sbuf', 0))
                self.assertTrue(binding['rw'])

    def test_unwritten_storage_stays_read_only(self):
        self.assertFalse(reflect_storage('vs', 0, 0x110, 0)['rw'])
        self.assertFalse(reflect_storage('vs', 0, 0x110, 2)['rw'])

    def test_bindless_storage_retains_write_usage(self):
        binding = reflect_storage('cs', 3, 0x38, 1)
        self.assertEqual((binding['kind'], binding['reg'], binding['off']), ('sbuf_ubo', 0, 0x38))
        self.assertTrue(binding['rw'])

    def test_compute_uniform_pointer_is_read_only(self):
        binding = reflect_storage('cs', 0, 0x210, 0)
        self.assertEqual((binding['kind'], binding['reg']), ('ubo_ptr', 0))
        self.assertFalse(binding['rw'])


if __name__ == '__main__':
    unittest.main()
