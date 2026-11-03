"""Container regressions; synthetic inputs contain no game data."""

from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import xeno


def wrap(raw, method=1):
    payload = zlib.compress(raw) if method == 1 else raw
    header = bytearray(48)
    header[:4] = b"xbc1"
    struct.pack_into("<III", header, 4, method, len(raw), len(payload))
    return bytes(header) + payload


def shader_container():
    # Two permutations with distinct sections. The intermediate descriptor is
    # deliberately different from the final linked vertex/fragment descriptor.
    slct = bytearray(0x48 + 16 + 2 * 72)
    slct[:4] = b"SLCT"
    struct.pack_into("<II", slct, 16, 2, 0x48)
    struct.pack_into("<4I", slct, 40, 0, 10, 0, 12)
    for i in range(2):
        at = 0x48 + 16 + 72 * i
        struct.pack_into("<II", slct, 0x48 + 8 * i, at, 72)
        struct.pack_into("<II", slct, at + 24, 32, 2)
        struct.pack_into("<5I", slct, at + 32, 0, 0, 1, 0, 2)
        struct.pack_into("<5I", slct, at + 52, 1, 3, 3, 2, 3)
    header = bytearray(80)
    header[:4] = b"HCPS"
    struct.pack_into("<III", header, 4, 1010, 72, 1)
    ctl_at = len(header) + len(slct)
    struct.pack_into("<6I", header, 24, 80, len(slct), ctl_at + 10, 12, ctl_at, 10)
    return bytes(header + slct) + b"VVpppWWqqq" + b"vsApsBvsCpsD"


class Xbc1Tests(unittest.TestCase):
    def test_zlib_and_uncompressed(self):
        for method in (0, 1):
            with self.subTest(method=method):
                data = wrap(b"shader data", method)
                raw, end = xeno.xbc1(b"prefix" + data, 6)
                self.assertEqual(raw, b"shader data")
                self.assertEqual(end, 6 + len(data))

    def test_truncation_is_rejected(self):
        with self.assertRaises(ValueError):
            xeno.xbc1(wrap(b"shader data")[:-1])

    def test_expanded_size_must_match(self):
        data = bytearray(wrap(b"shader data"))
        struct.pack_into("<I", data, 8, 100)
        with self.assertRaisesRegex(ValueError, "expected 100"):
            xeno.xbc1(data)

    def test_msrd_has_multiple_streams(self):
        first, second = wrap(b"vertex data"), wrap(b"fragment data")
        data = b"DRSM" + bytes(252) + first + bytes(16) + second
        blobs = list(xeno.wrapped_blobs(data))
        self.assertEqual(blobs, [(256, b"vertex data"),
                                (256 + len(first) + 16, b"fragment data")])

    def test_zstandard(self):
        try:
            from compression import zstd
        except ImportError:
            self.skipTest("Python's zstd compressor is unavailable")
        payload = zstd.compress(b"shader data")
        header = bytearray(48)
        header[:4] = b"xbc1"
        struct.pack_into("<III", header, 4, 3, 11, len(payload))
        self.assertEqual(xeno.xbc1(bytes(header) + payload)[0], b"shader data")


class CmpTests(unittest.TestCase):
    def test_member_extent_and_adjacent_streams(self):
        def member(raw):
            payload = zlib.compress(raw)
            return b"cmp\0zlib" + struct.pack("<II", len(payload), len(raw)) + payload
        first, second = member(b"first shader"), member(b"second shader")
        data = b"ARC\0" + first + second
        self.assertEqual(list(xeno.cmp_blobs(data)), [
            (4, b"first shader"), (4 + len(first), b"second shader"),
        ])
        with self.assertRaisesRegex(ValueError, "truncated"):
            list(xeno.cmp_blobs(data[:-1]))
        bad = bytearray(first)
        struct.pack_into("<I", bad, 12, 100)
        with self.assertRaisesRegex(ValueError, "length"):
            xeno.cmp1(bad)
        bad = bytearray(first + b"trailing")
        struct.pack_into("<I", bad, 8, len(bad) - 16)
        with self.assertRaisesRegex(ValueError, "extent"):
            xeno.cmp1(bad)


class ArdTests(unittest.TestCase):
    def test_payload_size_excludes_xbc1_header_and_hidden_entries_are_skipped(self):
        archive = wrap(b"shader data")
        header = bytearray(48)
        header[:4] = b"arh1"
        struct.pack_into("<II", header, 0x1C, 48, 2)
        table = struct.pack("<Q4I", 0, len(archive) - 48, 11, 0, 7)
        table += struct.pack("<Q4I", 0, len(archive), 0, 1, 8)
        with tempfile.TemporaryDirectory() as directory:
            index = Path(directory) / "test.arh"
            path = Path(directory) / "test.ard"
            index.write_bytes(bytes(header) + table)
            path.write_bytes(archive)
            jobs, consumed = xeno.ard_jobs([(str(path), "test.ard"),
                                           (str(index), "test.arh")])
            self.assertEqual(len(jobs), 1)
            self.assertEqual(consumed, {str(path), str(index)})
            self.assertEqual(list(xeno.ard_blobs(path, jobs[0][2])), [(7, b"shader data")])
            path.write_bytes(archive[:-1])
            with self.assertRaisesRegex(ValueError, "truncated"):
                list(xeno.ard_blobs(path, jobs[0][2]))

    def test_index_table_must_fit(self):
        with tempfile.TemporaryDirectory() as directory:
            index = Path(directory) / "test.arh"
            header = bytearray(40)
            header[:4] = b"arh1"
            struct.pack_into("<II", header, 0x1C, 48, 2)
            index.write_bytes(header)
            with self.assertRaises(ValueError):
                xeno.ard_entries(index)


class SpchTests(unittest.TestCase):
    def test_standalone_slct_keeps_permutation_padding_and_relative_offsets(self):
        metadata = bytearray(shader_container()[80:-22])
        struct.pack_into("<I", metadata, 4, 0x1003)
        struct.pack_into("<4I", metadata, 40, len(metadata), 16,
                         len(metadata) + 16, 12)
        data = bytes(metadata) + b"VVppp\0\0\0WWqqq\0\0\0vsApsBvsCpsD"
        self.assertEqual(list(xeno.slct_programs(bytes(37) + data, 37)), [
            (0, "vs", b"VV", b"vsA"), (0, "ps", b"ppp", b"psB"),
            (1, "vs", b"WW", b"vsC"), (1, "ps", b"qqq", b"psD"),
        ])
        with self.assertRaises(ValueError):
            list(xeno.slct_programs(data[:-1]))
        bad = bytearray(data)
        bad[len(metadata) + 5] = 1
        with self.assertRaisesRegex(ValueError, "padding"):
            list(xeno.slct_programs(bad))

    def test_legacy_control_keeps_its_padded_descriptor_size(self):
        control = bytearray(0x9e8)
        control[:4] = b"\x34\x12\x76\x98"
        struct.pack_into("<I", control, 0x20, 0x9e1)
        struct.pack_into("<I", control, 0x704, 128)
        code = b"v" * 128
        self.assertEqual(list(xeno.program_stages(control, code)), [(control, code)])
        with self.assertRaises(ValueError):
            list(xeno.program_stages(control + bytes(8), code))
        bad = control.copy()
        bad[-1] = 1
        with self.assertRaisesRegex(ValueError, "padding"):
            list(xeno.program_stages(bad, code))
        short = control[:0x87f]
        struct.pack_into("<I", short, 0x20, len(short))
        self.assertEqual(list(xeno.program_stages(short, code)), [(short, code)])

    def test_source_only_container_has_no_offline_binaries(self):
        data = bytearray(shader_container())
        struct.pack_into("<4I", data, 32, 0, 0, 0, 0)
        self.assertEqual(list(xeno.spch_programs(data)), [])
        struct.pack_into("<I", data, 36, 1)
        with self.assertRaisesRegex(ValueError, "only one"):
            list(xeno.spch_programs(data))

    def test_vertex_group_can_also_hold_geometry(self):
        control = bytearray(0x880)
        control[:4] = b"\x34\x12\x76\x98"
        struct.pack_into("<I", control, 0x20, len(control))
        struct.pack_into("<I", control, 0x704, 128)
        vertex, geometry = b"v" * 128, b"g" * 128
        self.assertEqual(list(xeno.program_stages(bytes(control) * 2, vertex + geometry)),
                         [(control, vertex), (control, geometry)])
        with self.assertRaises(ValueError):
            list(xeno.program_stages(bytes(control), vertex + geometry))

    def test_compute_shader_uses_fragment_descriptor(self):
        data = bytearray(shader_container()[:-3] + b"\x79\x56\x34\x12")
        struct.pack_into("<I", data, 36, 13)
        struct.pack_into("<I", data, 80 + 52, 13)
        struct.pack_into("<I", data, 80 + 160 + 52 + 8, 4)
        self.assertEqual(list(xeno.spch_programs(data))[-1],
                         (0, 1, "cs", b"qqq", b"\x79\x56\x34\x12"))

    def test_embedded_container_uses_last_descriptor_and_keeps_pairs(self):
        data = bytes(37) + shader_container()
        self.assertEqual(list(xeno.spch_programs(data, 37)), [
            (0, 0, "vs", b"VV", b"vsA"),
            (0, 0, "ps", b"ppp", b"psB"),
            (0, 1, "vs", b"WW", b"vsC"),
            (0, 1, "ps", b"qqq", b"psD"),
        ])

    def test_sections_cannot_extend_past_the_file(self):
        with self.assertRaises(ValueError):
            list(xeno.spch_programs(shader_container()[:-1]))

    def test_all_section_bytes_must_be_accounted_for(self):
        data = bytearray(shader_container())
        struct.pack_into("<I", data, 80 + 44, 9)
        with self.assertRaises(ValueError):
            list(xeno.spch_programs(data))


if __name__ == "__main__":
    unittest.main()
