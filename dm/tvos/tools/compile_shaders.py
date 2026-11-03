#!/usr/bin/env python3
"""Compile verified NVN MSL for tvOS with checked Apple5 compatibility lowering."""

import argparse
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys
import time

TOOLS = Path(__file__).resolve().parents[2] / "gfx/tools"
sys.path.insert(0, str(TOOLS))
import dmsh_fetch
import dmsh_mtlcache
import dmsh_outputs
import dmsh_legacy
import dmsh_border

METAL = ["xcrun", "-sdk", "appletvos", "metal", "-c", "-mtvos-version-min=18.0",
         "-fmetal-math-mode=relaxed", "-fpreserve-invariance"]
METALLIB = ["xcrun", "-sdk", "appletvos", "metallib"]
RECORD = " ".join(METAL) + "; " + " ".join(METALLIB)


def lower(source, reflection, border_layout=None):
    metadata = {"border_sampling_version": 1, "constant_buffer_indices_version": 1}
    source, _ = dmsh_legacy.constant_buffer_indices(source, reflection["bindings"])
    if reflection["stage"] == "cs":
        source, subgroups = dmsh_legacy.compute_subgroups(source, reflection["threadgroup_size"])
        if subgroups:
            metadata["legacy_subgroups"] = subgroups
    if reflection["stage"] == "ps":
        source, counters = dmsh_legacy.fragment_counters(source)
        if counters:
            metadata["legacy_fragment_counters"] = counters
        source, quads = dmsh_legacy.fragment_quad_shuffles(source)
        if quads:
            metadata["legacy_fragment_quads"] = quads
    source, borders = dmsh_border.lower(source, reflection["bindings"], border_layout)
    if borders:
        metadata["border_sampling"] = borders
    source, samplers = dmsh_legacy.compact_samplers(source, reflection["bindings"])
    if samplers:
        metadata["sampler_table"] = samplers
    return source, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Complete Mac b-<chain> directory")
    parser.add_argument("destination", type=Path, help="tvOS b-<chain> directory")
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=12)
    parser.add_argument("--limit", type=int, default=0, help="Compile a bounded smoke sample")
    parser.add_argument("--sdk", choices=("appletvos", "macosx"), default="appletvos",
                        help="Use macOS to validate the same compatibility shaders on a Mac")
    args = parser.parse_args()
    if args.jobs < 1 or args.limit < 0:
        parser.error("jobs must be positive and limit nonnegative")
    args.destination.mkdir(parents=True, exist_ok=True)
    temporary = args.destination / "work"
    temporary.mkdir(exist_ok=True)
    metal = METAL if args.sdk == "appletvos" else ["xcrun", "-sdk", "macosx", "metal", "-c",
                                                   "-fmetal-math-mode=relaxed", "-fpreserve-invariance"]
    metallib = ["xcrun", "-sdk", args.sdk, "metallib"]
    record = " ".join(metal) + "; " + " ".join(metallib)

    def compile_msl(source, air, library):
        for command in ([*metal, source, "-o", air], [*metallib, air, "-o", library]):
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr or result.stdout)

    target = "tvos" if args.sdk == "appletvos" else "mac"
    cache = dmsh_mtlcache.Cache(str(args.cache), {target: (args.sdk, record, compile_msl)}, str(temporary))
    sources = sorted(args.source.glob("*_b.metal"))
    if args.limit:
        sources = sources[:args.limit]
    if not sources:
        parser.error("no translated shaders")

    def build(source):
        reflection = json.loads(source.with_suffix(".json").read_text())
        if args.source.name != "b-" + reflection["chain"] or args.destination.name != args.source.name:
            raise ValueError(f"Mismatched translation chain: {source}")
        original_msl = source.read_text()
        lowered, compatibility = lower(original_msl, reflection)
        compiled_source = source
        if lowered != original_msl:
            compiled_source = temporary / source.name
            compiled_source.write_text(lowered)
        try:
            cache.compile(target, str(compiled_source), str(args.destination / source.with_suffix(".metallib").name))
        finally:
            if compiled_source != source:
                compiled_source.unlink(missing_ok=True)
        if "fetch" in reflection:
            original = source.with_suffix(".source.metal").read_text()
            variant, metadata = dmsh_fetch.variant(original, reflection["bindings"], om_only_ok=True)
            if not variant:
                raise ValueError(f"Missing recorded framebuffer-fetch variant: {source}")
            variant, outputs = dmsh_outputs.specialize(variant, conditional=True)
            if outputs:
                metadata["fragment_outputs"] = outputs
            if metadata != reflection["fetch"]:
                raise ValueError(f"Variant reflection changed: {source}")
            variant, variant_compatibility = lower(variant, reflection, compatibility.get("border_sampling"))
            if variant_compatibility != compatibility:
                raise ValueError(f"Variant compatibility metadata differs: {source}")
            variant_source = temporary / source.name.replace("_b.metal", "_bf.metal")
            variant_source.write_text(variant)
            try:
                cache.compile(target, str(variant_source), str(args.destination / variant_source.with_suffix(".metallib").name))
            finally:
                variant_source.unlink(missing_ok=True)
        reflection.update(compatibility)
        reflection["chain_metal"] = record
        output = args.destination / source.with_suffix(".json").name
        stage = output.with_suffix(".json.tmp")
        stage.write_text(json.dumps(reflection, sort_keys=True) + "\n")
        stage.replace(output)
        return source.name, int("fetch" in reflection), list(compatibility)

    start = time.monotonic()
    completed = variants = 0
    failures = []
    lowered_counts = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as executor:
        pending = {executor.submit(build, source): source for source in sources}
        for future in concurrent.futures.as_completed(pending):
            try:
                _, count, features = future.result()
                variants += count
                for feature in features:
                    lowered_counts[feature] = lowered_counts.get(feature, 0) + 1
            except Exception as error:
                failures.append({"source": pending[future].name, "error": str(error)})
                print(json.dumps(failures[-1]), flush=True)
            completed += 1
            if completed % 100 == 0 or completed == len(sources):
                elapsed = time.monotonic() - start
                print(f"{args.sdk} {completed}/{len(sources)}, variants {variants}, failures {len(failures)}, {elapsed:.1f}s", flush=True)
    report = {"source": str(args.source), "destination": str(args.destination), "compile_record": record, "sdk": args.sdk,
              "shaders": len(sources), "variants": variants, "failures": failures,
              "lowered": lowered_counts,
              "seconds": round(time.monotonic() - start, 2),
              "cache": {"/".join(key): value for key, value in cache.stats.items()}}
    (args.destination / "tvos-build.json").write_text(json.dumps(report, indent=2) + "\n")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
