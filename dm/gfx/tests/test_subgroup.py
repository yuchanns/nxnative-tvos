"""Check one-lane subgroup masks without requiring game shader data."""

import ast
from pathlib import Path
import struct
import tempfile
import unittest


def rewrite(path):
    source = Path(__file__).resolve().parents[1] / "tools" / "dmsh_build.py"
    names = {"SG_OPS", "SG_BUILTINS", "spv_single_lane", "_ep_drop"}
    nodes = []
    for node in ast.parse(source.read_text()).body:
        if isinstance(node, ast.FunctionDef) and node.name in names:
            nodes.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in names for target in node.targets
        ):
            nodes.append(node)
    namespace = {"struct": struct}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), "exec"), namespace)
    return namespace["spv_single_lane"](str(path))


def instruction(opcode, *operands):
    return [((len(operands) + 1) << 16) | opcode, *operands]


def fixture(builtin=4420, index=0, access_opcode=65, escape=False, nested=False):
    ops = [
        instruction(17, 1), instruction(17, 61), instruction(17, 64),
        instruction(14, 0, 1),
        instruction(15, 0, 20, *struct.unpack("<2I", b"main0\0\0\0"), 10, 11, 14),
        instruction(71, 10, 11, builtin), instruction(71, 11, 11, 0),
        instruction(71, 14, 11, 42),
        instruction(19, 1), instruction(21, 2, 32, 0), instruction(20, 3),
        instruction(23, 4, 2, 4), instruction(22, 5, 32),
        instruction(23, 6, 5, 4), instruction(32, 7, 1, 4),
        instruction(32, 8, 1, 2), instruction(32, 9, 3, 6),
        instruction(33, 12, 1), instruction(43, 2, 13, index),
        instruction(59, 7, 10, 1), instruction(59, 9, 11, 3),
        instruction(59, 8, 14, 1), instruction(54, 1, 20, 0, 12),
        instruction(248, 21), instruction(61, 2, 15, 14),
        instruction(access_opcode, 8, 22, 10, 15, *([13] if nested else [])),
        instruction(61, 2, 23, 22), instruction(112, 5, 24, 23),
        instruction(80, 6, 25, 24, 24, 24, 24), instruction(62, 11, 25),
    ]
    if escape:
        ops.append(instruction(83, 8, 26, 22))
    ops += [instruction(253), instruction(56)]
    words = [0x07230203, 0x00010300, 0, 80, 0] + [word for op in ops for word in op]
    return struct.pack(f"<{len(words)}I", *words)


def instructions(data):
    words = struct.unpack(f"<{len(data) // 4}I", data)
    cursor = 5
    while cursor < len(words):
        count = words[cursor] >> 16
        yield words[cursor] & 0xffff, list(words[cursor + 1:cursor + count])
        cursor += count


class SubgroupMaskTests(unittest.TestCase):
    def test_component_values_and_dynamic_index(self):
        for builtin, expected in ((4416, [1, 0, 0, 0]), (4420, [0, 0, 0, 0])):
            for access_opcode in (65, 66):
                with self.subTest(builtin=builtin, access_opcode=access_opcode):
                    with tempfile.TemporaryDirectory() as directory:
                        path = Path(directory) / "mask.spv"
                        path.write_bytes(fixture(builtin=builtin, access_opcode=access_opcode))
                        self.assertTrue(rewrite(path))
                        ops = list(instructions(path.read_bytes()))
                    constants = {args[1]: args[2] for op, args in ops if op == 43}
                    vectors = {
                        args[1]: [constants[value] for value in args[2:]]
                        for op, args in ops if op == 44
                    }
                    extract = next(args for op, args in ops if op == 77)
                    self.assertEqual(vectors[extract[2]], expected)
                    self.assertEqual(extract[3], 15)
                    self.assertFalse(any(op in (65, 66) for op, _ in ops))
                    self.assertTrue(any(op == 71 and args == [14, 11, 42] for op, args in ops))

    def test_escaping_pointer_leaves_input_unchanged(self):
        self.check_rejected(fixture(escape=True))

    def test_nested_access_leaves_input_unchanged(self):
        self.check_rejected(fixture(nested=True))

    def check_rejected(self, original):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "unsupported.spv"
            path.write_bytes(original)
            self.assertFalse(rewrite(path))
            self.assertEqual(path.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
