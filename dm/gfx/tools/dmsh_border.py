"""Preserve opaque sampler borders on GPUs that only support zero borders.

An immutable all-one texture measures the fraction of the filter footprint that
remains inside the texture. Sampling it with the same sampler and coordinates
preserves mip selection, anisotropy, offsets, and per-axis address modes. Native
fixed-point filtering rounds intermediate values, so GPU comparisons allow its
sub-UNORM8 rounding error instead of requiring bitwise distributivity.
"""

import re

from dmsh_legacy import LoweringError, _arguments, _matching


INFO = """
struct dm_border_info {
    float4 delta;
    uint compare;
    uint enabled;
    uint2 padding;
};
inline float dm_border_compare(float reference, float depth, uint function) {
    switch (function) {
        case 1: return float(reference < depth);
        case 2: return float(reference == depth);
        case 3: return float(reference <= depth);
        case 4: return float(reference > depth);
        case 5: return float(reference != depth);
        case 6: return float(reference >= depth);
        case 7: return 1.0;
        default: return 0.0;
    }
}
"""


def lower(source, bindings, layout=None):
    if "dm_border_info" in source:
        raise LoweringError("Sampler borders already lowered")
    resources = {binding["msl_name"]: binding for binding in bindings}
    calls = []
    pattern = (r"(?P<receiver>(?P<depth>spvDepthCast\()?spvDescriptorSet0\."
               r"(?P<texture>\w+)(?(depth)\)))\."
               r"(?P<operation>sample_compare|gather_compare|sample|gather)\s*\(")
    for match in re.finditer(pattern, source):
        end = _matching(source, match.end() - 1)
        args = _arguments(source[match.end():end])
        calls.append((match.start(), end + 1, match["texture"], match["operation"], args, bool(match["depth"])))
    # SPIRV-Cross gathers four constant offsets through a typed helper.
    for match in re.finditer(r"\bspvGatherConstOffsets\(spvDescriptorSet0\.(\w+)\s*,", source):
        opening = source.index("(", match.start())
        end = _matching(source, opening)
        args = _arguments(source[opening + 1:end])
        calls.append((match.start(), end + 1, match[1], "offsets", args[1:], False))

    selected = []
    pairs = {}
    for start, end, name, operation, args, depth in calls:
        texture = resources.get(name)
        if not texture:
            raise LoweringError(f"Unknown sampled texture {name}")
        kind = re.fullmatch(r"texture(1d|2d|2d_array|3d|cube|cube_array)<(float|half|uint|int)>", texture["msl_type"])
        if not kind:
            raise LoweringError(f"Unsupported sampled texture type {texture['msl_type']}")
        shape, scalar = kind.groups()
        if shape in ("cube", "cube_array"):
            continue  # Cube sampling ignores sampler address modes.
        sampler_match = re.fullmatch(r"spvDescriptorSet0\.(\w+)", args[0])
        sampler = resources.get(sampler_match[1]) if sampler_match else None
        if not sampler or sampler["msl_type"] != "sampler":
            raise LoweringError("Sampler borders require a scalar sampler binding")
        if scalar not in ("float", "half") and operation not in ("gather", "offsets"):
            raise LoweringError("Integer border sampling needs an integer gather")
        if depth and shape not in ("2d", "2d_array"):
            raise LoweringError("Unsupported depth texture border")
        pair = (texture["arg_id"], sampler["arg_id"])
        pairs[pair] = 3 if shape == "3d" else 1 if shape == "1d" else 2
        selected.append((start, end, name, operation, args, depth, shape, scalar, pair))

    if layout is None:
        if not pairs:
            return source, None
        ids = [int(value) for value in re.findall(r"\[\[id\((\d+)\)\]\]", source)]
        if not ids:
            raise LoweringError("Missing border sampler argument buffer")
        base = max(ids) + 1
        if base + len(pairs) >= 192:
            raise LoweringError("Sampler border resources exceed the reflection limit")
        layout = {"version": 1, "params_arg": base, "pairs": [
            {"texture_arg": texture, "sampler_arg": sampler, "mask_arg": base + 1 + index, "dimension": pairs[(texture, sampler)]}
            for index, (texture, sampler) in enumerate(sorted(pairs))]}
    elif layout["version"] != 1:
        raise LoweringError("Unknown sampler border layout")
    mapping = {(pair["texture_arg"], pair["sampler_arg"]): (index, pair)
               for index, pair in enumerate(layout["pairs"])}
    for pair, dimension in pairs.items():
        if pair not in mapping or mapping[pair][1]["dimension"] != dimension:
            raise LoweringError("Framebuffer-fetch variant changes sampler border bindings")

    edits = []
    ordered = sorted(selected)
    if any(left[1] > right[0] for left, right in zip(ordered, ordered[1:])):
        raise LoweringError("Nested texture samples need explicit temporaries")
    for start, end, name, operation, args, depth, shape, scalar, pair in selected:
        index, entry = mapping[pair]
        info = f"spvDescriptorSet0.dm_border_params[{index}]"
        mask = f"spvDescriptorSet0.dm_border_mask_{index}"
        delta = info + ".delta"
        original = source[start:end]
        coordinate = 3 if operation == "offsets" else 1
        if len(args) <= coordinate:
            raise LoweringError("Missing sampler border coordinates")
        mask_args = list(args)
        if shape == "2d_array":
            if len(mask_args) <= coordinate + 1:
                raise LoweringError("Missing sampler border array layer")
            del mask_args[coordinate + 1]
        if operation.endswith("compare"):
            reference_index = coordinate + 1
            if len(mask_args) <= reference_index:
                raise LoweringError("Missing sampler border depth reference")
            reference = mask_args.pop(reference_index)
            delta = (f"(dm_border_compare({reference}, {delta}.x, {info}.compare) - "
                     f"dm_border_compare({reference}, 0.0, {info}.compare))")
        gather = operation in ("gather", "gather_compare", "offsets")
        if operation == "offsets":
            component = args[2]
            if not re.fullmatch(r"component::[xyzw]", component):
                raise LoweringError("Sampler border gather needs a constant component")
            delta += "." + component[-1]
            mask_args[2] = "component::x"
            coverage = f"spvGatherConstOffsets({mask}, {', '.join(mask_args)})"
        else:
            if gather and not operation.endswith("compare"):
                components = [i for i, arg in enumerate(mask_args) if arg.startswith("component::")]
                if len(components) > 1:
                    raise LoweringError("Multiple gather components")
                component = mask_args[components[0]] if components else "component::x"
                if not re.fullmatch(r"component::[xyzw]", component):
                    raise LoweringError("Sampler border gather needs a constant component")
                delta += "." + component[-1]
                if components:
                    mask_args[components[0]] = "component::x"
            elif depth and not operation.endswith("compare"):
                delta += ".x"
            coverage = f"{mask}.{'gather' if gather else 'sample'}({', '.join(mask_args)})"
            if not gather:
                coverage += ".x"
        one = "float4(1.0)" if gather else "1.0"
        weight = f"({one} - ({info}.enabled ? {coverage} : {one}))"
        correction = f"({delta} * {weight})"
        if scalar in ("uint", "int"):
            correction = f"{scalar}4({correction})"
        value = f"({original} + {correction})"
        if operation.endswith("compare"):
            value = f"clamp({value}, 0.0, 1.0)"
        elif scalar == "half":
            value = f"half4({value})"
        edits.append((start, end, value))
    for start, end, value in sorted(edits, reverse=True):
        source = source[:start] + value + source[end:]

    blocks = list(re.finditer(r"\bstruct spvDescriptorSetBuffer0\s*\{", source))
    if len(blocks) != 1:
        raise LoweringError("Unrecognized sampler border resource block")
    block = blocks[0]
    end = _matching(source, block.end() - 1, "{", "}")
    declarations = f"    constant dm_border_info *dm_border_params [[id({layout['params_arg']})]];\n"
    for index, pair in enumerate(layout["pairs"]):
        declarations += f"    texture{pair['dimension']}d<float> dm_border_mask_{index} [[id({pair['mask_arg']})]];\n"
    source = source[:end] + declarations + source[end:]
    source = source[:block.start()] + INFO + "\n" + source[block.start():]
    return source, layout
