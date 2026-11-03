"""Exercise the production resource reader, corruption handling, and cache recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import pack_assets


def record(name, data, digest=None):
    if isinstance(name, str):
        name = name.encode()
    return struct.pack("<IQ32s", len(name), len(data), digest or hashlib.sha256(data).digest()) + name + data


def fragment(name, total, offset, data, digest=None):
    if isinstance(name, str):
        name = name.encode()
    return struct.pack("<IQQQ32s", len(name), total, offset, len(data),
                       digest or hashlib.sha256(data).digest()) + name + data


def main(binary):
    checks = 0
    with tempfile.TemporaryDirectory(prefix="nxnative-assets-") as temporary:
        base = Path(temporary)
        root = base / "romfs"
        root.mkdir()
        archive = base / "input.nxassets"

        def unpack(content, count, success):
            nonlocal checks
            archive.write_bytes(content)
            result = subprocess.run([binary, "unpack", str(archive), str(root), str(count)], capture_output=True)
            assert (result.returncode == 0) == success, result.stderr.decode()
            checks += 1
            assert not list(root.glob(".nxasset-*"))
            return json.loads(result.stdout) if success else None

        data = bytes(range(256)) * 700
        valid = b"NXROMFS1" + record("sub/data.bin", data) + record("empty", b"")
        index = unpack(valid, 2, True)
        assert (root / "sub/data.bin").read_bytes() == data
        assert (root / "empty").read_bytes() == b""
        index_path = base / "index.json"
        index_path.write_text(json.dumps(index))
        command = [binary, "check", str(index_path), str(root), "2"]
        assert subprocess.run(command).returncode == 0
        checks += 1
        (root / "empty").unlink()
        assert subprocess.run(command).returncode == 1
        checks += 1
        unpack(valid, 2, True)
        for cut in (0, 1, 7, 8, 9, 43, 51, 60, len(valid) - 1):
            unpack(valid[:cut], 2, False)
        unpack(valid + b"garbage", 2, False)
        unpack(valid, 1, False)
        unpack(valid, 3, False)
        corrupt = bytearray(valid)
        corrupt[70] ^= 1
        unpack(corrupt, 2, False)
        assert (root / "sub/data.bin").read_bytes() == data
        for name in ("../escape", "/escape", "sub/../../escape", "a//b", "a/./b", "a/", b"a\0b", b"\xff"):
            unpack(b"NXROMFS1" + record(name, b"data"), 1, False)
        unpack(b"NXROMFS1" + record("same", b"a") + record("same", b"b"), 2, False)
        outside = base / "outside"
        outside.mkdir()
        (root / "link").symlink_to(outside, target_is_directory=True)
        unpack(b"NXROMFS1" + record("link/escape", b"bad"), 1, False)
        assert not (outside / "escape").exists()
        unpack(b"NXROMFS1" + struct.pack("<IQ32s", 1, 2**64 - 1, bytes(32)) + b"a", 1, False)
        unpack(b"NXROMFS1" + struct.pack("<IQ32s", 4097, 0, bytes(32)), 1, False)

        def check(rows, success):
            nonlocal checks
            index_path.write_text(json.dumps(rows))
            result = subprocess.run([binary, "check", str(index_path), str(root), str(len(rows))])
            assert (result.returncode == 0) == success
            checks += 1

        check([["missing/child/file", 0]], False)
        assert not (root / "missing").exists()
        (outside / "file").write_bytes(b"data")
        check([["link/file", 4]], False)
        (root / "linked-file").symlink_to(outside / "file")
        check([["linked-file", 4]], False)
        check([["../outside/file", 4]], False)
        check([["sub/data.bin", len(data) - 1]], False)
        check([["sub/data.bin", len(data)], ["empty", 0]], True)

        # Independent processes unpack out of order and resume without keeping
        # memory state. The total file size is not proof that all pieces exist.
        cut = len(data) // 2
        head = b"NXROMFS2" + fragment("large.cpk", len(data), 0, data[:cut])
        tail = b"NXROMFS2" + fragment("large.cpk", len(data), cut, data[cut:])
        tail_index = unpack(tail, 1, True)
        head_index = unpack(head, 1, True)
        assert (root / "large.cpk").read_bytes() == data
        check(tail_index + head_index, True)
        unpack(b"NXROMFS2" + fragment("large.cpk", len(data), 0, b"bad", bytes(32)), 1, False)
        assert (root / "large.cpk").read_bytes() == data
        check(tail_index + head_index, True)
        (root / "large.cpk").unlink()
        new_head = unpack(head, 1, True)
        assert (root / "large.cpk").stat().st_size == len(data)
        check(new_head, True)
        check(tail_index, False)
        check(head_index, False)
        new_tail = unpack(tail, 1, True)
        check(new_head + new_tail, True)
        assert (root / "large.cpk").read_bytes() == data
        empty_index = unpack(b"NXROMFS2" + fragment("empty2", 0, 0, b""), 1, True)
        check(empty_index, True)
        for total, offset, payload in ((1, 2, b"a"), (2, 1, b"ab"), (2**64 - 1, 0, b"a"), (2, 0, b"")):
            unpack(b"NXROMFS2" + fragment("invalid", total, offset, payload), 1, False)
        for name in ("../escape", "link/escape", "linked-file", b"a\0b", b"\xff"):
            unpack(b"NXROMFS2" + fragment(name, 4, 0, b"data"), 1, False)
        unpack(head[:-1], 1, False)
        unpack(head + head[8:], 2, False)
        altered = [list(row) for row in new_head]
        altered[0][7] += 1
        check(altered, False)
        # A length-changing repair must create a new identity, so old receipts
        # cannot revive if a later repair restores the original file length.
        (root / "large.cpk").write_bytes(b"short")
        replaced = unpack(head, 1, True)
        check(new_tail, False)
        check(replaced, True)

        source = base / "source"
        source.mkdir()
        inputs = {"nested/large.cpk": data * 2, "small": b"tail", "zero": b""}
        for name, payload in inputs.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
        packed = base / "packed"
        limit = 100_000
        manifest = pack_assets.pack(source, packed, limit)
        assert manifest["files"] == len(inputs)
        assert manifest["unpacked_bytes"] == sum(map(len, inputs.values()))
        assert len(manifest["chunks"]) > 1
        receipts = []
        for chunk in reversed(manifest["chunks"]):
            content = (packed / chunk["name"]).read_bytes()
            assert len(content) == chunk["bytes"] <= limit
            assert hashlib.sha256(content).hexdigest() == chunk["sha256"]
            receipts += unpack(content, chunk["files"], True)
        check(receipts, True)
        for name, payload in inputs.items():
            assert (root / name).read_bytes() == payload
    print(json.dumps({"checks": checks, "failed": 0}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    main(parser.parse_args().binary)
