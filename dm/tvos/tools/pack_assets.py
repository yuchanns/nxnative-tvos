#!/usr/bin/env python3
"""Pack immutable RomFS files into bounded, independently verifiable downloads."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct

MAGIC = b"NXROMFS2"
# A record holds one contiguous file fragment, with its own integrity check.
HEADER = struct.Struct("<IQQQ32s")


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while data := stream.read(1 << 20):
            value.update(data)
    return value.digest()


def pack(root, output, limit):
    if output.resolve().is_relative_to(root.resolve()):
        raise ValueError("The output must be outside the resource directory")
    output.mkdir(parents=True, exist_ok=True)
    groups, group, used = [], [], len(MAGIC)
    file_count = unpacked = 0
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"RomFS symlinks are not supported: {path}")
        if not path.is_file():
            continue
        name = path.relative_to(root).as_posix().encode("utf-8")
        if not 0 < len(name) <= 4096 or any(c < 32 or c == 127 for c in name):
            raise ValueError(f"Invalid asset path: {path}")
        stat = path.stat()
        size, offset = stat.st_size, 0
        overhead = HEADER.size + len(name)
        if limit <= len(MAGIC) + overhead:
            raise ValueError(f"Chunk limit is too small for: {path}")
        file_count += 1
        unpacked += size
        while offset < size or not size:
            available = limit - used - overhead
            if available <= 0:
                groups.append(group)
                group, used = [], len(MAGIC)
                available = limit - used - overhead
            length = min(size - offset, available)
            group.append((path, name, stat, offset, length))
            used += overhead + length
            offset += length
            if not size:
                break
    if group:
        groups.append(group)
    manifest = {"version": 2, "files": file_count, "unpacked_bytes": unpacked, "chunks": []}
    for number, files in enumerate(groups):
        part = output / f"chunk-{number:04d}.partial"
        with part.open("wb") as stream:
            stream.write(MAGIC)
            for path, name, stat, offset, length in files:
                header_offset = stream.tell()
                stream.write(HEADER.pack(len(name), stat.st_size, offset, length, bytes(32)))
                stream.write(name)
                copied = 0
                checksum = hashlib.sha256()
                with path.open("rb") as source:
                    before = os.fstat(source.fileno())
                    source.seek(offset)
                    while copied < length:
                        data = source.read(min(1 << 20, length - copied))
                        if not data:
                            raise RuntimeError(f"Asset truncated during packing: {path}")
                        stream.write(data)
                        checksum.update(data)
                        copied += len(data)
                    after = os.fstat(source.fileno())
                fingerprint = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
                if fingerprint(before) != fingerprint(stat) or fingerprint(after) != fingerprint(stat):
                    raise RuntimeError(f"Asset changed during packing: {path}")
                end = stream.tell()
                stream.seek(header_offset)
                stream.write(HEADER.pack(len(name), stat.st_size, offset, length, checksum.digest()))
                stream.seek(end)
            if stream.tell() > limit:
                raise RuntimeError("Chunk exceeds its planned limit")
        checksum = digest(part).hex()
        name = f"{number:04d}-{checksum}.nxassets"
        target = output / name
        os.replace(part, target)
        manifest["chunks"].append({"name": name, "sha256": checksum,
                                   "bytes": target.stat().st_size, "files": len(files)})
        print(f"Packed {number + 1}/{len(groups)}: {len(files)} fragments", flush=True)
    encoded = json.dumps(manifest, separators=(",", ":")).encode() + b"\n"
    (output / "manifest.json").write_bytes(encoded)
    print(json.dumps({"files": manifest["files"], "bytes": manifest["unpacked_bytes"],
                      "chunks": len(groups), "manifest_sha256": hashlib.sha256(encoded).hexdigest()}), flush=True)
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--romfs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--chunk-mib", type=int, default=64)
    args = parser.parse_args()
    if args.chunk_mib < 1:
        parser.error("--chunk-mib must be positive")
    pack(args.romfs.resolve(), args.output.resolve(), args.chunk_mib << 20)
