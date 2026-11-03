#!/usr/bin/env python3
"""Prepare runtime-compatible ASTC cache entries from DDS textures on the CPU.

DDS layout: https://learn.microsoft.com/windows/win32/direct3ddds/dds-header
This tool never creates a Metal device or changes the original assets.
"""

import argparse
from concurrent.futures import ProcessPoolExecutor
import ctypes
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import time

import cpk


NAMESPACE = b"nxnative-astc-arm-5.7.0-fastest-stripe4m-v1\0"
LEGACY_FORMATS = {b"DXT1": (65, 66, 69, 70), b"DXT3": (67, 71), b"DXT5": (68, 72)}
DXGI_FORMATS = {71: (65, 66), 72: (69, 70), 74: (67,), 75: (71,),
                77: (68,), 78: (72,), 98: (77,), 99: (78,)}
LIBRARY = None


@dataclass(frozen=True)
class Plane:
    offset: int
    size: int
    width: int
    height: int
    layer: int
    mip: int
    formats: tuple[int, ...]


def dds_planes(data, offset=0):
    """Return validated, tightly packed BC planes; reject unsupported layouts."""
    if offset < 0 or len(data) - offset < 128 or data[offset:offset + 4] != b"DDS ":
        raise ValueError("Missing DDS header")
    fields = struct.unpack_from("<31I", data, offset + 4)
    if fields[0] != 124 or fields[18] != 32 or not fields[19] & 4:
        raise ValueError("Invalid DDS pixel format header")
    height, width, depth, mips = fields[2], fields[3], fields[5], fields[6] or 1
    if not 1 <= width <= 16384 or not 1 <= height <= 16384:
        raise ValueError("Unsupported DDS dimensions")
    if not 1 <= mips <= max(width, height).bit_length():
        raise ValueError("Invalid DDS mip count")
    if depth > 1 or fields[27] & 0x200000:
        raise ValueError("Volume DDS is not supported")
    fourcc = data[offset + 84:offset + 88]
    cursor, layers = offset + 128, 1
    if fourcc == b"DX10":
        if len(data) - cursor < 20:
            raise ValueError("Truncated DX10 header")
        dxgi, dimension, misc, array_size, _ = struct.unpack_from("<5I", data, cursor)
        cursor += 20
        if dimension != 3 or not 1 <= array_size <= 2048:
            raise ValueError("Unsupported DX10 resource layout")
        formats = DXGI_FORMATS.get(dxgi)
        layers = array_size * (6 if misc & 4 else 1)
    else:
        formats = LEGACY_FORMATS.get(fourcc)
        caps2 = fields[27]
        if caps2 & 0x200:
            faces = caps2 & 0xFC00
            if not faces:
                raise ValueError("Cube DDS does not identify any faces")
            layers = faces.bit_count()
        elif caps2 & 0xFC00:
            raise ValueError("DDS faces without the cube flag")
    if not formats:
        raise ValueError("DDS format has no supported ASTC conversion")
    block_bytes = 8 if formats[0] in (65, 66, 69, 70) else 16
    planes = []
    for layer in range(layers):
        for mip in range(mips):
            w, h = max(width >> mip, 1), max(height >> mip, 1)
            size = ((w + 3) // 4) * ((h + 3) // 4) * block_bytes
            if size > len(data) - cursor:
                raise ValueError("Truncated DDS payload")
            planes.append(Plane(cursor, size, w, h, layer, mip, formats))
            cursor += size
    return planes, cursor


def cache_key(fmt, width, height, data):
    return hashlib.sha256(NAMESPACE + struct.pack("<4I", fmt, width, height, 4) + data).hexdigest()


def open_encoder(path):
    global LIBRARY
    LIBRARY = ctypes.CDLL(path)
    fn = LIBRARY.nvn_bc_astc_encode_key
    fn.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t,
                   ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint,
                   ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p,
                   ctypes.c_char_p, ctypes.POINTER(ctypes.c_int), ctypes.c_void_p]
    fn.restype = ctypes.c_int


def encode(job):
    meta, source, cache = job
    started = time.monotonic()
    size = ((meta["width"] + 3) // 4) * ((meta["height"] + 3) // 4) * 16
    src = ctypes.create_string_buffer(source, len(source))
    dst = ctypes.create_string_buffer(size)
    key = ctypes.create_string_buffer(65)
    hit = ctypes.c_int()
    rc = LIBRARY.nvn_bc_astc_encode_key(meta["format"], src, len(source),
        meta["width"], meta["height"], 4, dst, size, cache.encode(), None,
        ctypes.byref(hit), key)
    if rc or key.value.decode() != meta["key"]:
        raise RuntimeError("Production encoder failed or returned a different cache key")
    path = Path(cache) / (meta["key"] + ".astc")
    payload = path.read_bytes()
    digest = hashlib.sha256(dst.raw).digest()
    if payload != b"NXASTC01" + digest + dst.raw:
        raise RuntimeError("Written cache entry did not validate")
    return {**meta, "cache_hit": bool(hit.value), "bytes": len(payload),
            "payload_sha256": digest.hex(), "seconds": time.monotonic() - started}


def sources(paths, suffixes, limit, include):
    count = 0
    for path in paths:
        if path.suffix.lower() == ".cpk":
            for entry in cpk.files(path):
                if Path(entry[0]).suffix.lower() not in suffixes or not include.search(entry[0]):
                    continue
                if entry[3] > 128 << 20:
                    raise ValueError("Source entry exceeds the bounded decoder limit")
                yield str(path) + "#" + entry[0], cpk.read(path, entry)
                count += 1
                if limit and count >= limit:
                    return
        else:
            if path.stat().st_size > 128 << 20:
                raise ValueError("Source file exceeds the bounded decoder limit")
            yield str(path), path.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", nargs="+", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--scan-only", action="store_true")
    parser.add_argument("--match-index", type=Path)
    parser.add_argument("--suffixes", default=".dds,.gmd,.gfs,.gft,.epl,.bin,.bed,.pac,.pak,.spd,.gap")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--include", default=".", help="Regex selecting archive entry names")
    parser.add_argument("--formats", help="Optional comma-separated NVN formats observed on the target")
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.workers <= 8 or args.limit < 0:
        parser.error("Use 1 to 8 workers and a nonnegative limit")
    if not args.scan_only and (not args.cache or not args.library):
        parser.error("Encoding requires --cache and --library")
    if args.cache:
        args.cache.mkdir(parents=True, exist_ok=True)
    known = set(json.loads(args.match_index.read_text())["entries"]) if args.match_index else set()
    suffixes = set(args.suffixes.lower().split(","))
    selected_formats = {int(value, 0) for value in args.formats.split(",")} if args.formats else None
    if selected_formats and not selected_formats <= {65, 66, 67, 68, 69, 70, 71, 72, 77, 78}:
        parser.error("Requested format has no supported ASTC conversion")
    counts = {"sources": 0, "textures": 0, "rejected_headers": 0,
              "unique_planes": 0, "existing_device_keys": 0}
    seen, pending = set(), []
    executor = None if args.scan_only else ProcessPoolExecutor(args.workers,
        initializer=open_encoder, initargs=(str(args.library),))
    started = time.monotonic()
    with args.report.open("x") as report:
        try:
            for name, data in sources(args.sources, suffixes, args.limit, re.compile(args.include)):
                counts["sources"] += 1
                end = 0
                for match in re.finditer(b"DDS ", data):
                    if match.start() < end:
                        continue
                    try:
                        planes, end = dds_planes(data, match.start())
                    except ValueError:
                        counts["rejected_headers"] += 1
                        continue
                    counts["textures"] += 1
                    for plane in planes:
                        payload = data[plane.offset:plane.offset + plane.size]
                        for fmt in plane.formats:
                            if selected_formats is not None and fmt not in selected_formats:
                                continue
                            key = cache_key(fmt, plane.width, plane.height, payload)
                            if key in seen:
                                continue
                            seen.add(key)
                            counts["unique_planes"] += 1
                            exists = key + ".astc" in known
                            counts["existing_device_keys"] += exists
                            meta = {"source": name, "dds_offset": match.start(),
                                "layer": plane.layer, "mip": plane.mip,
                                "format": fmt, "width": plane.width, "height": plane.height,
                                "key": key, "source_bytes": len(payload), "device_key_exists": exists}
                            if executor:
                                pending.append(executor.submit(encode, (meta, payload, str(args.cache))))
                                if len(pending) >= args.workers * 2:
                                    report.write(json.dumps(pending.pop(0).result()) + "\n")
                            else:
                                report.write(json.dumps(meta) + "\n")
                if counts["sources"] % 100 == 0:
                    report.flush()
                    print(json.dumps({**counts, "seconds": time.monotonic() - started}), flush=True)
            for future in pending:
                report.write(json.dumps(future.result()) + "\n")
        finally:
            if executor:
                executor.shutdown(wait=True, cancel_futures=True)
    summary = {**counts, "seconds": time.monotonic() - started,
               "scan_only": args.scan_only, "metal_device_created": False}
    args.report.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
