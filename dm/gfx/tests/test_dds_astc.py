"""CPU-only DDS bounds and cache identity tests."""

import importlib.util
from pathlib import Path
import struct
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("dds_astc", TOOLS / "dds_astc.py")
dds = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = dds
spec.loader.exec_module(dds)


def header(width, height, fourcc=b"DXT1", mips=1, caps2=0, depth=0):
    fields = [0] * 31
    fields[0:7] = [124, 0x1007, height, width, 0, depth, mips]
    fields[18:21] = [32, 4, int.from_bytes(fourcc, "little")]
    fields[26:28] = [0x1000, caps2]
    return b"DDS " + struct.pack("<31I", *fields)


class LayoutTests(unittest.TestCase):
    def test_odd_dimensions_and_mip_tail(self):
        source = header(7, 5, mips=3) + bytes(32 + 8 + 8)
        planes, end = dds.dds_planes(source)
        self.assertEqual([(p.width, p.height, p.size) for p in planes],
                         [(7, 5, 32), (3, 2, 8), (1, 1, 8)])
        self.assertEqual(end, len(source))
        self.assertEqual(planes[0].formats, (65, 66, 69, 70))

    def test_embedded_payload_and_trailing_container(self):
        source = b"container" + header(4, 4, b"DXT5") + bytes(16) + b"next"
        planes, end = dds.dds_planes(source, 9)
        self.assertEqual(planes[0].offset, 137)
        self.assertEqual(source[end:], b"next")
        self.assertEqual(planes[0].formats, (68, 72))

    def test_legacy_cube_faces(self):
        source = header(4, 4, caps2=0x200 | 0x400 | 0x1000) + bytes(16)
        planes, end = dds.dds_planes(source)
        self.assertEqual([p.layer for p in planes], [0, 1])
        self.assertEqual(end, len(source))

    def test_dx10_cube_array_bc7_srgb(self):
        source = header(4, 4, b"DX10") + struct.pack("<5I", 99, 3, 4, 2, 0) + bytes(12 * 16)
        planes, end = dds.dds_planes(source)
        self.assertEqual(len(planes), 12)
        self.assertEqual(planes[-1].layer, 11)
        self.assertEqual(planes[0].formats, (78,))
        self.assertEqual(end, len(source))

    def test_every_payload_truncation_is_rejected(self):
        source = header(8, 8, b"DXT3", mips=4) + bytes(64 + 16 + 16 + 16)
        for length in range(len(source)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                dds.dds_planes(source[:length])

    def test_invalid_and_unsupported_layouts(self):
        for source in [header(0, 4), header(16385, 4), header(4, 4, mips=4),
                       header(4, 4, caps2=0x200), header(4, 4, caps2=0x400),
                       header(4, 4, depth=2), header(4, 4, b"DXT2"),
                       header(4, 4, b"DX10") + struct.pack("<5I", 98, 4, 0, 1, 0),
                       header(4, 4, b"DX10") + struct.pack("<5I", 98, 3, 0, 0, 0)]:
            with self.subTest(source=source[:28]), self.assertRaises(ValueError):
                dds.dds_planes(source + bytes(1024))

    def test_cache_identity_includes_format_size_and_bytes(self):
        original = dds.cache_key(66, 4, 4, bytes(8))
        self.assertNotEqual(original, dds.cache_key(65, 4, 4, bytes(8)))
        self.assertNotEqual(original, dds.cache_key(66, 2, 4, bytes(8)))
        self.assertNotEqual(original, dds.cache_key(66, 4, 4, bytes([1]) * 8))


if __name__ == "__main__":
    unittest.main()
