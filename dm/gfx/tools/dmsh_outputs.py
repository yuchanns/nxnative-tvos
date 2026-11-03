"""Specialize fragment export types from attachment formats, without source JIT.

Maxwell exports register bits. Metal requires each color's component type in
the function signature. Optional output fields keep all three types in one
offline library; function constants select exactly one field per attachment.
"""

import re


CONSTANT_BASE = 128
TYPES = ("float", "uint", "int")
FIELD = re.compile(
    r"\b(?P<type>\w+)\s+(?P<name>\w+)\s*"
    r"\[\[\s*color\((?P<color>\d+)\)"
    r"(?:\s*,\s*function_constant\((?P<condition>\w+)\))?\s*\]\]\s*;"
)


def _body_span(source):
    entry = re.search(r"\bfragment\s+main0_out\s+main0\(.*?\)\s*\{", source, re.S)
    if not entry:
        raise ValueError("fragment entry signature not recognized")
    start = entry.end()
    depth = 1
    for position in range(start, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return start, position
    raise ValueError("fragment entry body is incomplete")


def specialize(source, conditional=False):
    """Return transformed MSL and the export ABI, or unchanged MSL with no ABI."""
    output = re.search(r"struct main0_out\s*\{(.*?)\};", source, re.S)
    if not output:
        if "[[color(" in source:
            raise ValueError("fragment color exports have no recognized output struct")
        return source, None
    fields = list(FIELD.finditer(output.group(1)))
    if len(fields) != len(re.findall(r"\bcolor\s*\(", output.group(1))):
        raise ValueError("fragment color declaration not recognized")
    if not fields:
        return source, None
    if len(fields) > 32 or "dm_out_" in source:
        raise ValueError("fragment output specialization ABI conflict")
    constants = {int(index) for index in re.findall(r"\bfunction_constant\((\d+)\)", source)}
    if constants.intersection(range(CONSTANT_BASE, CONSTANT_BASE + 3 * len(fields))):
        raise ValueError("fragment output function constant range is already used")

    metadata = []
    declarations = []
    values = []
    stores = []
    replacements = {}
    for index, field in enumerate(fields):
        scalar = re.fullmatch(r"(float|uint|int)([234]?)", field["type"])
        color = int(field["color"])
        condition = field["condition"]
        if not scalar or color > 7 or (condition and not conditional):
            raise ValueError("fragment export is outside the specialization ABI")
        record = {"color": color, "kind": TYPES.index(scalar[1])}
        if condition:
            record["condition"] = condition
        metadata.append(record)
        value = f"dm_out_value_{index}"
        replacements[field["name"]] = value
        values.append(f"    {field['type']} {value} = {field['type']}(0);\n")
        members = []
        for kind, name in enumerate(TYPES):
            flag = f"dm_out_{index}_{kind}"
            member = f"dm_out_color_{index}_{kind}"
            component_type = name + scalar[2]
            declarations.append(f"constant bool {flag} [[function_constant({CONSTANT_BASE + 3 * index + kind})]];\n")
            members.append(f"{component_type} {member} [[color({color}), function_constant({flag})]];")
            expression = value if component_type == field["type"] else f"as_type<{component_type}>({value})"
            stores.append(f"    if ({flag}) out.{member} = {expression};\n")
        replacements[field.group(0)] = "\n    ".join(members)

    structure = FIELD.sub(lambda match: replacements[match.group(0)], output.group(1))
    result = source[:output.start(1)] + structure + source[output.end(1):]
    start, end = _body_span(result)
    body = result[start:end]
    initialization = re.compile(r"\bmain0_out\s+out\s*=\s*\{\s*\}\s*;")
    if len(initialization.findall(body)) != 1:
        raise ValueError("fragment output initialization not recognized")
    body = initialization.sub(lambda match: match.group(0) + "\n" + "".join(values), body, count=1)
    for field in fields:
        body = re.sub(r"\bout\." + re.escape(field["name"]) + r"\b", replacements[field["name"]], body)
    if not re.search(r"\breturn\s+out\s*;", body):
        raise ValueError("fragment output return not recognized")
    body = re.sub(r"\breturn\s+out\s*;", lambda match: "{\n" + "".join(stores) + match.group(0) + "\n}", body)
    result = result[:start] + body + result[end:]
    namespace = "using namespace metal;"
    if namespace not in result:
        raise ValueError("Metal namespace not recognized")
    result = result.replace(namespace, namespace + "\n" + "".join(declarations), 1)
    return result, {"version": 1, "constant_base": CONSTANT_BASE, "fields": metadata}
