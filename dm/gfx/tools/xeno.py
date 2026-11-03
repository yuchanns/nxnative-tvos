"""Read Xenoblade ARH1 archives, XBC1 streams and HCPS shader containers.

The layouts are cross-checked with ardain's arh1 and xc3_lib's xbc1 / spch
readers (https://github.com/RoccoDev/ard-tools and
https://github.com/ScanMountGoat/xc3_lib). No archive is written or mounted.
"""

import os
from pathlib import Path
import re
import struct
import subprocess
import zlib


def span(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f"range {offset:#x}+{size:#x} exceeds {len(data):#x} bytes")
    return data[offset:offset + size]


def ard_entries(index):
    data = Path(index).read_bytes()
    if data[:4] != b"arh1":
        raise ValueError(f"{index}: unsupported ARH magic {data[:4]!r}")
    span(data, 0, 0x28)
    offset, count = struct.unpack_from("<II", data, 0x1C)
    table = span(data, offset, count * 24)
    return [(fid, off, size, expanded, flags)
            for off, size, expanded, flags, fid in struct.iter_unpack("<Q4I", table)
            if size and not flags & 1]


def ard_jobs(files, parts=64):
    jobs, archives = [], set()
    for path, rel in files:
        if not path.lower().endswith(".ard"):
            continue
        index = str(Path(path).with_suffix(".arh"))
        if not os.path.isfile(index):
            continue
        entries = ard_entries(index)
        size = max(1, (len(entries) + parts - 1) // parts)
        jobs.extend((path, rel, entries[i:i + size], "ard")
                    for i in range(0, len(entries), size))
        archives.update((path, index))
    return jobs, archives


def xbc1(data, offset=0):
    header = span(data, offset, 48)
    if header[:4] != b"xbc1":
        raise ValueError("missing XBC1 header")
    method, expanded, size = struct.unpack_from("<III", header, 4)
    payload = span(data, offset + 48, size)
    if method == 0:
        raw = payload
    elif method == 1:
        raw = zlib.decompress(payload)
    elif method == 3:
        try:
            from compression import zstd
        except ImportError:
            raw = subprocess.run(["zstd", "-q", "-d", "-c"], input=payload,
                                 capture_output=True, check=True).stdout
        else:
            raw = zstd.decompress(payload)
    else:
        raise ValueError(f"unsupported XBC1 compression {method}")
    if len(raw) != expanded:
        raise ValueError(f"XBC1 expands to {len(raw)} bytes, expected {expanded}")
    return raw, offset + 48 + size


def wrapped_blobs(data):
    """MSRD resources keep independently compressed XBC1 streams inside them."""
    end = 0
    for match in re.finditer(b"xbc1", data):
        offset = match.start()
        if offset < end or offset + 48 > len(data):
            continue
        method, expanded, size = struct.unpack_from("<III", data, offset + 4)
        if method not in (0, 1, 3) or not expanded or offset + 48 + size > len(data):
            continue
        raw, end = xbc1(data, offset)
        yield offset, raw


def cmp1(data, offset=0):
    """Firehawk ARC members store a length-delimited CMP zlib stream."""
    header = span(data, offset, 16)
    if header[:8] != b"cmp\0zlib":
        raise ValueError("missing CMP zlib header")
    size, expanded = struct.unpack_from("<II", header, 8)
    payload = span(data, offset + 16, size)
    decoder = zlib.decompressobj()
    raw = decoder.decompress(payload, expanded + 1)
    if (len(raw) != expanded or not decoder.eof or decoder.unused_data
            or decoder.unconsumed_tail):
        raise ValueError("CMP decoded length or zlib extent mismatch")
    return raw, offset + 16 + size


def cmp_blobs(data):
    end = 0
    for match in re.finditer(b"cmp\0zlib", data):
        offset = match.start()
        if offset < end or offset + 16 > len(data):
            continue
        size, _ = struct.unpack_from("<II", data, offset + 8)
        if offset + 16 + size > len(data):
            raise ValueError("truncated CMP payload")
        raw, end = cmp1(data, offset)
        yield offset, raw


def ard_blobs(path, entries):
    with open(path, "rb") as archive:
        length = os.fstat(archive.fileno()).st_size
        for fid, offset, size, expanded, flags in entries:
            if offset + size > length:
                raise ValueError(f"{path} entry {fid}: outside the archive")
            archive.seek(offset)
            if expanded or flags & 2:
                header = archive.read(48)
                if len(header) != 48 or header[:4] != b"xbc1":
                    raise ValueError(f"{path} entry {fid}: missing XBC1 header")
                payload_size = struct.unpack_from("<I", header, 12)[0]
                if offset + 48 + payload_size > length:
                    raise ValueError(f"{path} entry {fid}: truncated XBC1 payload")
                # ARH1's compressed size excludes the XBC1 header in XC3.
                raw, _ = xbc1(header + archive.read(payload_size))
                if expanded and len(raw) != expanded:
                    raise ValueError(f"{path} entry {fid}: ARH/XBC1 size mismatch")
            else:
                raw = archive.read(size)
            yield fid, raw


def program_stages(controls, codes):
    """An NVSD vertex group can contain both vertex and geometry stages."""
    ci = ki = 0
    while ci < len(controls):
        header = span(controls, ci, 0x708)
        if header[:4] != b"\x34\x12\x76\x98":
            raise ValueError("missing shader control header")
        control_size = struct.unpack_from("<I", header, 0x20)[0]
        code_size = struct.unpack_from("<I", header, 0x704)[0]
        if control_size < 0x760 or not code_size:
            raise ValueError("invalid shader section size")
        if ki + code_size == len(codes):
            # Standalone SLCT 0x1003 pads the final control to its NVSD size.
            remaining = len(controls) - ci
            if not control_size <= remaining <= ((control_size + 7) & ~7):
                raise ValueError("invalid final shader control size")
            if any(controls[ci + control_size:]):
                raise ValueError("nonzero shader control padding")
            control_size = remaining
        elif control_size & 3:
            raise ValueError("unaligned intermediate shader control")
        yield span(controls, ci, control_size), span(codes, ki, code_size)
        ci += control_size
        ki += code_size
    if ki != len(codes):
        raise ValueError("control sections do not cover the shader code")


def spch_programs(data, base=0):
    """Yield (SLCT index, permutation, stage, control, code) from one HCPS."""
    header = span(data, base, 0x48)
    if header[:4] != b"HCPS":
        raise ValueError("missing HCPS header")
    table, count = struct.unpack_from("<II", header, 8)
    slct_at, slct_size, code_at, code_size, ctl_at, ctl_size = struct.unpack_from(
        "<6I", header, 24)
    # DLC model streams also carry source-only NVSP containers: compressed GLSL
    # in their permutations, with no NVN code/control sections to translate.
    if not code_size and not ctl_size:
        return
    if not code_size or not ctl_size:
        raise ValueError("HCPS has only one of its code/control sections")
    offsets = span(data, base + table, count * 8)
    metadata = span(data, base + slct_at, slct_size)
    codes = span(data, base + code_at, code_size)
    controls = span(data, base + ctl_at, ctl_size)
    for si, (offset, _) in enumerate(struct.iter_unpack("<II", offsets)):
        slct = span(metadata, offset, 0x48)
        if slct[:4] != b"SLCT":
            raise ValueError(f"HCPS SLCT {si}: missing SLCT header")
        cp, cn, kp, kn = struct.unpack_from("<4I", slct, 40)
        ctl = span(controls, cp, cn)
        code = span(codes, kp, kn)
        for vi, stage, control, shader in _slct_programs(metadata, offset, ctl, code):
            yield si, vi, stage, control, shader


def _slct_programs(metadata, offset, controls, codes, control_alignment=1):
    slct = span(metadata, offset, 0x48)
    variants, programs = struct.unpack_from("<II", slct, 16)
    table = span(metadata, offset + programs, variants * 8)
    ci = ki = 0
    for vi, (nv_at, nv_size) in enumerate(struct.iter_unpack("<II", table)):
        nv = span(metadata, offset + nv_at, nv_size)
        span(nv, 0, 32)
        shader_at, shader_count = struct.unpack_from("<II", nv, 24)
        shaders = span(nv, shader_at, shader_count * 20)
        if not shader_count:
            raise ValueError(f"SLCT permutation {vi}: no shader descriptor")
        # The last NVSD descriptor holds the linked vertex/fragment sizes.
        _, vs, ps, vc, pc = struct.unpack_from("<5I", shaders, len(shaders) - 20)
        for stage, ns, nc in (("vs", vs, vc), ("ps", ps, pc)):
            shader, control = span(codes, ki, ns), span(controls, ci, nc)
            ki += ns
            ci += nc
            if bool(ns) != bool(nc):
                raise ValueError("SLCT has a code/control section mismatch")
            if ns:
                if shader[:4] == b"\x79\x56\x34\x12":
                    stage = "cs"
                yield vi, stage, control, shader
        aligned = (ci + control_alignment - 1) & ~(control_alignment - 1)
        if any(span(controls, ci, aligned - ci)):
            raise ValueError("SLCT has nonzero control padding")
        ci = aligned
    if ci != len(controls) or ki != len(codes):
        raise ValueError("SLCT descriptor sizes do not cover its sections")


def slct_programs(data, base=0):
    """Standalone SLCT 0x1003 stores its code/control offsets relative to itself."""
    header = span(data, base, 0x48)
    if header[:4] != b"SLCT" or struct.unpack_from("<I", header, 4)[0] != 0x1003:
        raise ValueError("unsupported standalone SLCT header")
    cp, cn, kp, kn = struct.unpack_from("<4I", header, 40)
    metadata = span(data, base, cp)
    controls, codes = span(data, base + cp, cn), span(data, base + kp, kn)
    yield from _slct_programs(metadata, 0, controls, codes, control_alignment=8)
