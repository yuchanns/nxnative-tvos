"""Fragment input coverage follows the final vertex-producing stage."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
with tempfile.TemporaryDirectory() as directory:
    Path(directory, 'game.tsv').write_text('shader_groups\t\nshader_note\tsynthetic test\n')
    with patch.dict(os.environ, {'DM_GAME': directory}):
        import shader_split


def shader(stage, locations=()):
    control = bytearray(0x880)
    struct.pack_into('<I', control, 0x20, len(control))
    code = bytearray(0x80)
    code[0] = shader_split.STAGES.index(stage)
    for location in locations:
        code[0x30 + 24 + location] = 1
    return bytes(control), bytes(code)


class PairTests(unittest.TestCase):
    def test_separable_geometry_gets_fragment_input_cover(self):
        keys = {
            'vertex': ('main#GLSLC@0', 'vs', *shader('vs')),
            'geometry': ('main#GLSLC@1', 'gs', *shader('gs')),
            'fragment-a': ('main#GLSLC@2', 'ps', *shader('ps', (1, 2))),
            'fragment-b': ('main#GLSLC@3', 'ps', *shader('ps', (14,))),
            'compute': ('main#GLSLC@4', 'cs', *shader('cs')),
        }
        pairs, cover = shader_split.separable_pairs(keys)
        self.assertEqual(set(cover), {'fragment-a', 'fragment-b'})
        self.assertEqual(pairs, {(producer, fragment) for producer in ('vertex', 'geometry')
                                 for fragment in ('fragment-a', 'fragment-b')})

    def test_linked_geometry_replaces_vertex_as_fragment_producer(self):
        records = [('test#BNSH@0#var0', stage, *shader(stage)) for stage in ('vs', 'gs', 'ps')]
        keys = {stage: shader_split.skey(control, code) for _, stage, control, code in records}
        self.assertEqual(shader_split.var_pairs(records), {(keys['gs'], keys['ps'])})
        records = [record for record in records if record[1] != 'gs']
        self.assertEqual(shader_split.var_pairs(records), {(keys['vs'], keys['ps'])})


if __name__ == '__main__':
    unittest.main()
