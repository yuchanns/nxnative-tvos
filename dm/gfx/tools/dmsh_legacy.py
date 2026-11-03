"""Lower shader operations for pre-Apple6 GPUs.

The shared-memory implementation retains the guest's subgroup size. A barrier
cannot replace a shuffle in divergent control flow: the lowering rejects such
call sites, including divergent loop exits. Kernel returns are safe on Apple
silicon: ended threads no longer participate in barriers (MSL 6.10.1).
"""

from dataclasses import dataclass
import math
import re


class LoweringError(ValueError):
    pass


@dataclass
class Function:
    name: str
    start: int
    args: int
    close_args: int
    body: int
    end: int
    kernel: bool


def _matching(source, start, opening="(", closing=")"):
    depth = 0
    for index in range(start, len(source)):
        if source[index] == opening:
            depth += 1
        elif source[index] == closing:
            depth -= 1
            if not depth:
                return index
    raise LoweringError(f"Unbalanced {opening} at {start}")


def _code(source):
    return re.sub(r"//[^\n]*|/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m[0]), source, flags=re.S)


def constant_buffer_indices(source, bindings):
    """Bound indirect reads to their declared constant-buffer array extent.

    Legacy GPU compilation can speculate a load from an inactive branch whose
    index holds unrelated bits. Select a valid address before the load; legal
    indices retain their value. This is not a bound-buffer-length check.
    """
    buffers = [binding for binding in bindings if binding.get("kind") == "cbuf"]
    if not buffers:
        return source, None
    code = _code(source)
    if re.search(r"\bdm_legacy_cbuf_index\b", code):
        raise LoweringError("Constant-buffer indices already lowered")
    insertions = []
    for binding in buffers:
        name = binding["msl_name"]
        pointer = re.fullmatch(r"constant\s+(\w+)\s*\*", binding["msl_type"])
        if not pointer:
            raise LoweringError(f"Unrecognized constant-buffer type {name}")
        structures = list(re.finditer(r"\bstruct\s+" + re.escape(pointer[1]) + r"\s*\{([^{}]*)\}\s*;", code))
        array = re.fullmatch(r"\s*float4\s+(_m\d+)\s*\[\s*([1-9]\d*)\s*\]\s*;\s*", structures[0][1]) if len(structures) == 1 else None
        if not array or int(array[2]) > 0xffffffff:
            raise LoweringError(f"Unrecognized constant-buffer array {name}")
        extent = int(array[2])
        declaration = (r"\bconstant\s+" + re.escape(pointer[1]) + r"\s*\*\s*" + re.escape(name) +
                       r"\s*\[\[id\(" + str(binding["arg_id"]) + r"\)\]\]\s*;")
        declarations = list(re.finditer(declaration, code))
        if len(declarations) != 1:
            raise LoweringError(f"Unrecognized constant-buffer declaration {name}")
        access = (r"\(\s*\*\s*spvDescriptorSet0\s*\.\s*" + re.escape(name) +
                  r"\s*\)\s*\.\s*" + re.escape(array[1]) + r"\s*\[")
        uses = list(re.finditer(access, code))
        recognized = [declarations[0], *uses]
        for token in re.finditer(r"\b" + re.escape(name) + r"\b", code):
            if not any(match.start() <= token.start() < match.end() for match in recognized):
                raise LoweringError(f"Unrecognized constant-buffer use {name}")
        for use in uses:
            end = _matching(code, use.end() - 1, "[", "]")
            index = code[use.end():end].strip()
            if not index:
                raise LoweringError(f"Empty constant-buffer index {name}")
            literal = re.fullmatch(r"(0[xX][0-9a-fA-F]+|[0-9]+)[uU]?", index)
            if literal:
                value = int(literal[1], 16 if literal[1].lower().startswith("0x") else 10)
                if value < extent:
                    continue
            # Insertions also compose for a buffer read nested inside an index.
            # Passing the expression to a helper evaluates it exactly once.
            insertions.extend([(use.end(), "dm_legacy_cbuf_index(uint("), (end, f"), {extent}u)")])
    if not insertions:
        return source, None
    namespace = list(re.finditer(r"\busing\s+namespace\s+metal\s*;", code))
    if len(namespace) != 1:
        raise LoweringError("Missing Metal namespace for constant-buffer indices")
    helper = ("\n\ninline uint dm_legacy_cbuf_index(uint index, uint count)\n"
              "{\n    return index < count ? index : 0u;\n}\n")
    reads = len(insertions) // 2
    insertions.append((namespace[0].end(), helper))
    for position, text in sorted(insertions, reverse=True):
        source = source[:position] + text + source[position:]
    return source, {"version": 1, "reads": reads}


def _arguments(text):
    parts, start, depth = [], 0, 0
    for index, char in enumerate(text):
        depth += (char in "([") - (char in ")]")
        if char == "," and depth == 0:
            parts.append(text[start:index].strip())
            start = index + 1
    return [*parts, text[start:].strip()]


def _functions(source):
    code = _code(source)
    depth = 0
    depths = []
    for char in code:
        depths.append(depth)
        depth += (char == "{") - (char == "}")
    functions = []
    pattern = r"(?m)^[ \t]*([^\n;{}#=]+?)\b(\w+)[ \t]*\("
    for match in re.finditer(pattern, code):
        if depths[match.start()] or match[2] in ("if", "while", "for", "switch"):
            continue
        args = match.end() - 1
        close = _matching(code, args)
        body = close + 1
        while body < len(code) and code[body].isspace():
            body += 1
        if body >= len(code) or code[body] != "{":
            continue
        functions.append(Function(match[2], match.start(), args, close, body,
                                  _matching(code, body, "{", "}"), bool(re.search(r"\bkernel\b", match[1]))))
    return functions


def _controls(body):
    """Return structured control regions and their conditions."""
    regions = []
    do_tails = set()
    for match in re.finditer(r"\bdo\s*\{", body):
        end = _matching(body, match.end() - 1, "{", "}")
        tail = re.match(r"\s*(while)\s*\(", body[end + 1:])
        if not tail:
            raise LoweringError("Unrecognized do/while loop")
        do_tails.add(end + 1 + tail.start(1))
    for match in re.finditer(r"\b(if|while|for|switch)\s*\(", body):
        opening = match.end() - 1
        close = _matching(body, opening)
        block = close + 1
        while block < len(body) and body[block].isspace():
            block += 1
        if block < len(body) and body[block] == "{":
            end = _matching(body, block, "{", "}")
            regions.append((block, end, body[opening + 1:close]))
            if match[1] == "if":
                alternate = re.match(r"\s*else\s*\{", body[end + 1:])
                if alternate:
                    start = end + alternate.end()
                    regions.append((start, _matching(body, start, "{", "}"), body[opening + 1:close]))
        elif match.start() not in do_tails:
            raise LoweringError("Collective control flow must use braced statements")
    for match in re.finditer(r"\bdo\s*\{", body):
        start = match.end() - 1
        end = _matching(body, start, "{", "}")
        tail = re.match(r"\s*while\s*\(", body[end + 1:])
        if not tail:
            raise LoweringError("Unrecognized do/while loop")
        opening = end + tail.end()
        close = _matching(body, opening)
        regions.append((start, end, body[opening + 1:close]))
    return regions


def _converged(source, function, collective_names):
    body = _code(source[function.body + 1:function.end])
    regions = _controls(body)
    nonuniform = {"gl_LocalInvocationID", "gl_LocalInvocationIndex", "gl_GlobalInvocationID",
                  "gl_SubgroupInvocationID", "gl_SubgroupLtMask", "gl_SubgroupLeMask",
                  "gl_SubgroupEqMask", "gl_SubgroupGeMask", "gl_SubgroupGtMask"}
    args = source[function.args + 1:function.close_args]
    for name, attribute in re.findall(r"\b(\w+)\s*\[\[(\w+)\]\]", args):
        if attribute in ("thread_position_in_threadgroup", "thread_index_in_threadgroup",
                         "thread_position_in_grid", "thread_index_in_simdgroup"):
            nonuniform.add(name)
    if not function.kernel:
        nonuniform.update(re.findall(r"\b(\w+)\s*(?:,|$)", args))
    nonuniform.update(re.findall(r"\bthreadgroup\s+[^;]+?\b(\w+)\s*(?:\[[^;]+\])?\s*;", body))
    assignments = list(re.finditer(r"\b(\w+)(?:\.[A-Za-z_]\w*|\[[^\]]+\])*\s*(?:=(?!=)|(?:<<|>>|[+*/%&|^-])=)\s*([^;]+);", body))
    increments = list(re.finditer(r"\b(\w+)\s*(?:\+\+|--)|(?:\+\+|--)\s*(\w+)\b", body))
    for helper in _functions(source):
        parameters = _arguments(source[helper.args + 1:helper.close_args])
        outputs = [i for i, parameter in enumerate(parameters)
                   if re.search(r"\bthread\b", parameter) and "&" in parameter and not re.search(r"\bconst\b", parameter)]
        if not outputs:
            continue
        for call in re.finditer(r"\b" + re.escape(helper.name) + r"\s*\(", body):
            close = _matching(body, call.end() - 1)
            actual = _arguments(body[call.end():close])
            if len(actual) != len(parameters):
                raise LoweringError("Unrecognized mutable helper arguments")
            for index in outputs:
                name = re.fullmatch(r"(\w+)(?:\.[A-Za-z_]\w*|\[[^\]]+\])*", actual[index])
                if not name:
                    raise LoweringError("Unrecognized mutable helper argument")
                nonuniform.add(name[1])

    def varying(expression):
        # Every remaining lane observes the same ballot, even after kernel returns.
        expression = re.sub(r"spvSubgroupBallot\([^()]*\)", "0", expression)
        if "[" in expression:
            return True
        if re.search(r"\.(?:read|sample|gather|sample_compare)\s*\(", expression):
            return True
        if re.search(r"\b(?:atomic_\w+|spvSubgroupShuffle|_\d+)\s*\(", expression):
            return True
        return bool(set(re.findall(r"\b\w+\b", expression)) & nonuniform)

    while True:
        before = len(nonuniform)
        for match in assignments:
            guards = [condition for start, end, condition in regions if start < match.start() < end]
            if varying(match[2]) or any(varying(condition) for condition in guards):
                nonuniform.add(match[1])
        for match in increments:
            if any(start < match.start() < end and varying(condition) for start, end, condition in regions):
                nonuniform.add(match[1] or match[2])
        if len(nonuniform) == before:
            break
    call_pattern = r"\b(?:" + "|".join(map(re.escape, collective_names)) + r")\s*\("
    for match in re.finditer(call_pattern, body):
        # Generated MSL uses statement-level selection for collectives. Do not
        # assume a short-circuit or conditional expression executes every lane.
        start = max(body.rfind(char, 0, match.start()) for char in ";{}") + 1
        end = body.find(";", match.end())
        if re.search(r"\?|&&|\|\|", body[start:end if end >= 0 else len(body)]):
            raise LoweringError(f"Conditional collective expression in {function.name}")
        if any(start < match.start() < end and varying(condition) for start, end, condition in regions):
            raise LoweringError(f"Divergent collective call in {function.name}")
    for match in re.finditer(r"\b(?:break|continue|return)\b", body):
        if match[0] == "return" and function.kernel:
            continue
        if any(start < match.start() < end and varying(condition) for start, end, condition in regions):
            raise LoweringError(f"Divergent control transfer in {function.name}")


CONTEXT_DECL = ("threadgroup ulong4 *dm_legacy_exchange, threadgroup uint *dm_legacy_votes, "
                "uint dm_legacy_tid")
CONTEXT_ARGS = "dm_legacy_exchange, dm_legacy_votes, dm_legacy_tid"
HELPERS = r"""
// Each slot is private to one lane until the first barrier. The second barrier
// keeps a faster lane from overwriting a value a peer has not consumed yet.
template<typename T>
inline T dm_legacy_shuffle(T value, ushort lane, threadgroup ulong4 *exchange,
                          threadgroup uint *votes, uint index)
{
    static_assert(sizeof(T) <= sizeof(ulong4), "Subgroup value exceeds its scratch slot");
    *reinterpret_cast<threadgroup T *>(exchange + index) = value;
    simdgroup_barrier(mem_flags::mem_threadgroup);
    T result = *reinterpret_cast<threadgroup T *>(exchange + ((index & ~31u) | (uint(lane) & 31u)));
    simdgroup_barrier(mem_flags::mem_threadgroup);
    return result;
}

inline uint4 dm_legacy_ballot(bool value, threadgroup ulong4 *exchange,
                            threadgroup uint *votes, uint index)
{
    votes[index] = uint(value);
    simdgroup_barrier(mem_flags::mem_threadgroup);
    uint mask = 0;
    for (uint lane = 0; lane < 32; ++lane)
        mask |= votes[(index & ~31u) | lane] << lane;
    simdgroup_barrier(mem_flags::mem_threadgroup);
    // A lane which returns before the next ballot must leave an inactive vote.
    votes[index] = 0;
    simdgroup_barrier(mem_flags::mem_threadgroup);
    return uint4(mask, 0, 0, 0);
}
"""


def compute_subgroups(source, threadgroup_size):
    """Return (MSL, metadata); reject unknown or nonconverged collectives."""
    if not re.search(r"\bsimd_(?:shuffle|ballot)\s*\(", source):
        return source, None
    if "dm_legacy_" in source:
        raise LoweringError("Legacy collectives already lowered")
    unsupported = set(re.findall(r"\b(simd_\w+)\s*\(", _code(source))) - {"simd_shuffle", "simd_ballot"}
    if unsupported:
        raise LoweringError(f"Unsupported legacy collective: {sorted(unsupported)}")
    if len(threadgroup_size) != 3 or any(not isinstance(n, int) or n < 1 for n in threadgroup_size):
        raise LoweringError("Missing fixed threadgroup size")
    threads = math.prod(threadgroup_size)
    if threads > 512 or threads % 32:
        raise LoweringError("Legacy collectives require 32 to 512 threads in complete subgroups")
    functions = _functions(source)
    seeds = {function.name for function in functions
             if re.search(r"\bsimd_(?:shuffle|ballot)\s*\(", source[function.body:function.end])}
    needed = set(seeds)
    while True:
        before = len(needed)
        for function in functions:
            body = source[function.body + 1:function.end]
            if any(re.search(r"\b" + re.escape(name) + r"\s*\(", body) for name in needed):
                needed.add(function.name)
        if len(needed) == before:
            break
    kernels = [function for function in functions if function.kernel and function.name in needed]
    if len(kernels) != 1:
        raise LoweringError("Expected one compute entry point")
    for function in functions:
        if function.name in needed:
            _converged(source, function, needed | {"simd_shuffle", "simd_ballot"})
    ballot = [function for function in functions if function.name == "spvSubgroupBallot"]
    if any("simd_ballot" in source[function.body:function.end] for function in functions if function not in ballot):
        raise LoweringError("Unrecognized ballot representation")
    edits = []
    excluded = []
    for function in ballot:
        edits.append((function.body + 1, function.end,
                      "\n    return dm_legacy_ballot(value, " + CONTEXT_ARGS + ");\n"))
        excluded.append((function.body, function.end))
    definitions = {function.args: function for function in functions}
    pattern = r"\b(?:" + "|".join(map(re.escape, needed | {"simd_shuffle"})) + r")\s*\("
    for match in re.finditer(pattern, _code(source)):
        if any(start < match.start() < end for start, end in excluded):
            continue
        opening = match.end() - 1
        close = _matching(source, opening)
        function = definitions.get(opening)
        if function:
            if function.kernel:
                lane = re.findall(r"\b(\w+)\s*\[\[thread_index_in_simdgroup\]\]", source[function.args + 1:close])
                if len(lane) != 1:
                    raise LoweringError("Missing compute SIMD lane identity")
                edits.append((close, close, ", uint dm_legacy_group [[simdgroup_index_in_threadgroup]]"))
                declaration = (f"\n    uint dm_legacy_tid = dm_legacy_group * 32u + {lane[0]};"
                               f"\n    threadgroup ulong4 dm_legacy_exchange[{threads}];"
                               f"\n    threadgroup uint dm_legacy_votes[{threads}];"
                               "\n    dm_legacy_votes[dm_legacy_tid] = 0;"
                               "\n    threadgroup_barrier(mem_flags::mem_threadgroup);\n")
                edits.append((function.body + 1, function.body + 1, declaration))
            else:
                edits.append((close, close, ", " + CONTEXT_DECL))
        else:
            if match[0].startswith("simd_shuffle"):
                edits.append((match.start(), match.start() + len("simd_shuffle"), "dm_legacy_shuffle"))
            edits.append((close, close, ", " + CONTEXT_ARGS))
    for start, end, replacement in sorted(edits, reverse=True):
        source = source[:start] + replacement + source[end:]
    marker = "using namespace metal;"
    if marker not in source:
        raise LoweringError("Missing Metal namespace")
    source = source.replace(marker, marker + "\n" + HELPERS, 1)
    return source, {"width": 32, "scratch_bytes": threads * 36, "converged": True}


def _uniform_expression(expression, source, bindings, stack=()):
    """Prove an address is draw-uniform, using immutable scalar definitions."""
    if re.search(r"\+\+|--|=(?!=)|\b(?:atomic_\w+|in|gl_\w+)\b", expression):
        return False
    for member in re.findall(r"\bspvDescriptorSet0\.(\w+)", expression):
        if member not in bindings:
            return False
    expression = re.sub(r"\bspvDescriptorSet0\.\w+", "0", expression)
    expression = re.sub(r"\.(?:_m\d+|[xyzwrgba]{1,4})\b", "", expression)
    expression = re.sub(r"\b(?:0x[0-9a-fA-F]+|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)[ufhl]*\b", "0", expression)
    safe = {"int", "uint", "float", "bool", "short", "ushort", "long", "ulong",
            "as_type", "trunc", "floor", "ceil", "min", "max", "clamp", "abs", "fma",
            "true", "false"}
    for name in set(re.findall(r"\b[A-Za-z_]\w*\b", expression)) - safe:
        if name in stack:
            return False
        definitions = list(re.finditer(r"\b" + re.escape(name) + r"\s*=(?!=)\s*([^;]+);", source))
        if len(definitions) != 1 or not _uniform_expression(definitions[0][1], source, bindings, (*stack, name)):
            return False
        if re.search(r"\b" + re.escape(name) + r"\s*(?:\+\+|--|(?:<<|>>|[+*/%&|^-])=)", source):
            return False
    return True


def fragment_counters(source):
    """Expand an unused-result subgroup counter into one atomic per fragment.

    Metal suppresses helper-thread stores and atomics (MSL 6.13.4). This is
    equivalent only for a uniform counter address, an active-lane population
    increment, and an unused result. Any other helper observation is rejected.
    """
    if "simd_is_helper_thread(" not in source:
        return source, None
    if not re.search(r"\bfragment\b", source) or re.search(r"\b(?:kernel|vertex)\b", source):
        raise LoweringError("Helper counter lowering requires one fragment entry")
    original = _code(source)
    constants = set(re.findall(r"\bconstant\s+\w+\s*\*\s*(\w+)\s*\[\[id", original))
    pattern = re.compile(
        r"uint\s+(?P<mask>\w+)\s*=\s*spvSubgroupBallot\(true\)\.x;\s*"
        r"float\s+(?P<lane>\w+)\s*=\s*as_type<float>\(gl_SubgroupInvocationID\);\s*"
        r"int\s+(?P<highest>\w+)\s*=\s*int\(spvFindUMSB\((?P=mask)\)\);\s*"
        r"int\s+(?P<count>\w+)\s*=\s*popcount\(int\((?P=mask)\)\);\s*"
        r"bool\s+(?P<elected>\w+)\s*=\s*(?P=highest)\s*==\s*as_type<int>\((?P=lane)\);\s*"
        r"if\s*\((?P=elected)\)\s*\{")
    edits = []
    for match in pattern.finditer(original):
        end = _matching(original, match.end() - 1, "{", "}")
        body = original[match.end():end]
        atomic = re.search(r"uint\s+(\w+)\s*=\s*atomic_fetch_add_explicit\((.*),\s*uint\(" +
                           re.escape(match["count"]) + r"\),\s*memory_order_relaxed\);", body)
        if not atomic:
            raise LoweringError("Unrecognized elected counter atomic")
        pointer = atomic[2]
        pointer_match = re.fullmatch(r"\(device atomic_uint\s*\*\)\s*&\(\*spvDescriptorSet0\.(\w+)\)\._m0\[(.*)\]", pointer)
        if not pointer_match or not _uniform_expression(pointer_match[2], original, constants):
            raise LoweringError("Fragment counter address is not proven draw-uniform")
        prefix, suffix = body[:atomic.start()], body[atomic.end():]
        local_names = []
        for statement in prefix.split(";"):
            if not statement.strip():
                continue
            declaration = re.fullmatch(r"\s*(?:u?int|float)\s+(\w+)\s*=\s*(.+)\s*", statement)
            if not declaration or not _uniform_expression(declaration[2], original, constants):
                raise LoweringError("Elected counter contains other effects")
            local_names.append(declaration[1])
        aliases = {atomic[1]}
        for statement in suffix.split(";"):
            if not statement.strip():
                continue
            alias = re.fullmatch(r"\s*uint\s+(\w+)\s*=\s*(\w+)\s*", statement)
            if not alias or alias[2] not in aliases:
                raise LoweringError("Fragment counter result is observed")
            aliases.add(alias[1])
        outside = original[:match.start()] + original[end + 1:]
        for name in [*match.groupdict().values(), *local_names, *aliases]:
            if re.search(r"\b" + re.escape(name) + r"\b", outside):
                raise LoweringError("Fragment counter value escapes its elected block")
        replacement = prefix + "\n    // dm_legacy_fragment_counter\n    atomic_fetch_add_explicit(" + pointer + ", 1u, memory_order_relaxed);\n"
        edits.append((match.start(), end + 1, replacement))
    if not edits:
        raise LoweringError("No proven fragment counters")
    count = len(edits)
    for start, end, replacement in reversed(edits):
        source = source[:start] + replacement + source[end:]

    # Remove only canonical helper guards around the proven atomic blocks.
    edits, mutable, guards = [], set(), 0
    code = _code(source)
    for match in re.finditer(r"if\s*\(!(?P<guard>\w+)\)\s*\{", code):
        end = _matching(source, match.end() - 1, "{", "}")
        body = source[match.end():end]
        if body.count("// dm_legacy_fragment_counter") != 1:
            continue
        name = match["guard"]
        flag_pattern = r"bool\s+" + re.escape(name) + r"\s*=\s*as_type<int>\((\w+)\)\s*!=\s*0;"
        definitions = list(re.finditer(flag_pattern, code[:match.start()]))
        alias = None
        if not definitions:
            aliases = list(re.finditer(r"\b" + re.escape(name) + r"\s*=\s*(\w+);", code[:match.start()]))
            if not aliases:
                raise LoweringError("Unrecognized helper guard")
            alias = aliases[-1]
            name = alias[1]
            flag_pattern = r"bool\s+" + re.escape(name) + r"\s*=\s*as_type<int>\((\w+)\)\s*!=\s*0;"
            definitions = list(re.finditer(flag_pattern, code[:alias.start()]))
        if len(definitions) != 1:
            raise LoweringError("Unrecognized helper guard definition")
        flag = definitions[0]
        bits_pattern = (r"float\s+" + re.escape(flag[1]) +
                        r"\s*=\s*as_type<float>\(gl_HelperInvocation\s*\?\s*\(-1\)\s*:\s*0\);")
        bits = list(re.finditer(bits_pattern, code))
        if len(bits) != 1 or bits[0].end() > flag.start():
            raise LoweringError("Unrecognized helper status definition")
        outside = code[:match.start()] + code[end + 1:]
        outside = re.sub(bits_pattern, "", outside)
        outside = re.sub(flag_pattern, "", outside)
        if alias:
            outside = re.sub(r"\b" + re.escape(match["guard"]) + r"\s*=\s*" + re.escape(name) + ";", "", outside)
        for local in (flag[1], name):
            if re.search(r"\b" + re.escape(local) + r"\b", outside):
                raise LoweringError("Helper status escapes its guard")
        if alias:
            mutable.add(match["guard"])
            body = re.sub(r"\b" + re.escape(match["guard"]) + r"\s*=\s*true;", "", body)
        # Exactly a uniform address calculation followed by the proven atomic.
        statements = [part.strip() for part in _code(body).split(";") if part.strip()]
        if not statements or not statements[-1].startswith("atomic_fetch_add_explicit("):
            raise LoweringError("Helper guard contains unproven effects")
        if sum("atomic_fetch_add_explicit(" in part for part in statements) != 1:
            raise LoweringError("Helper guard contains unproven effects")
        for statement in statements[:-1]:
            if not re.fullmatch(r"(?:u?int|float)\s+\w+\s*=\s*[^{}]+", statement):
                raise LoweringError("Helper guard contains unproven effects")
        edits.extend([(match.start(), end + 1, body), (flag.start(), flag.end(), ""),
                      (bits[0].start(), bits[0].end(), "")])
        if alias:
            edits.append((alias.start(), alias.end(), ""))
        guards += 1
    if guards != count:
        raise LoweringError("Counter is not enclosed by a canonical helper guard")
    for start, end, replacement in sorted(edits, reverse=True):
        source = source[:start] + replacement + source[end:]
    for name in mutable:
        source = re.sub(r"(?:\bbool\s+)?\b" + re.escape(name) + r"\s*=\s*(?:true|false);", "", source)
        if re.search(r"\b" + re.escape(name) + r"\b", source):
            raise LoweringError("Mutable helper status escapes its guards")
    source = re.sub(r"\bbool\s+gl_HelperInvocation\s*=\s*\{\};", "", source)
    source = source.replace("gl_HelperInvocation = simd_is_helper_thread();", "")
    if re.search(r"\bgl_HelperInvocation\b|\bsimd_is_helper_thread\s*\(", source):
        raise LoweringError("Unproven helper status use")
    functions = _functions(source)
    for function in reversed(functions):
        if function.name == "spvSubgroupBallot":
            outside = source[:function.start] + source[function.end + 1:]
            if re.search(r"\bspvSubgroupBallot\s*\(", outside):
                raise LoweringError("Unproven fragment ballot use")
            source = outside
    source = re.sub(r",\s*uint gl_SubgroupInvocationID\s*\[\[thread_index_in_simdgroup\]\]", "", source)
    if "gl_SubgroupInvocationID" in source:
        raise LoweringError("Unproven fragment lane use")
    return source, {"version": 1, "counters": count}


def compact_samplers(source, bindings):
    """Keep logical guest bindings, but encode at most 16 distinct samplers."""
    arrays = [binding for binding in bindings if re.match(r"array<\s*sampler\s*,", binding.get("msl_type", ""))]
    samplers = [binding for binding in bindings if binding.get("msl_type") == "sampler"]
    count = len(samplers) + sum(int(re.search(r",\s*(\d+)\s*>$", binding["msl_type"])[1]) for binding in arrays)
    if count <= 16:
        return source, None
    if "dm_sampler_" in source:
        raise LoweringError("Sampler table already lowered")
    if arrays:
        raise LoweringError("Compact sampler arrays need an explicit element mapping")
    # The generated resource block has one argument ID per scalar resource.
    ids = [int(value) for value in re.findall(r"\[\[id\((\d+)\)\]\]", source)]
    if not ids:
        raise LoweringError("Missing sampler argument buffer")
    base = max(ids) + 1
    if any(binding.get("msl_type", "").startswith("array<") for binding in bindings):
        raise LoweringError("Compact sampler table needs scalar resource declarations")
    if base + 16 >= 192:
        raise LoweringError("Compact sampler table exceeds the reflection limit")
    original_ids = []
    for index, binding in enumerate(samplers):
        name, aid = binding["msl_name"], binding["arg_id"]
        pattern = r"\bsampler\s+" + re.escape(name) + r"\s*\[\[id\(" + str(aid) + r"\)\]\];"
        source, matches = re.subn(pattern, "", source)
        if matches != 1:
            raise LoweringError(f"Unrecognized sampler declaration {name}")
        source = re.sub(r"\bspvDescriptorSet0\." + re.escape(name) + r"\b",
                        f"spvDescriptorSet0.dm_sampler_table[spvDescriptorSet0.dm_sampler_map[{index}]]", source)
        if re.search(r"\b" + re.escape(name) + r"\b", _code(source)):
            raise LoweringError(f"Unrecognized sampler use {name}")
        original_ids.append(aid)
    blocks = list(re.finditer(r"\bstruct spvDescriptorSetBuffer0\s*\{", _code(source)))
    if len(blocks) != 1:
        raise LoweringError("Unrecognized compact sampler resource block")
    end = _matching(source, blocks[0].end() - 1, "{", "}")
    # Metal requires argument IDs to increase in declaration order, including
    # when textures and the removed logical samplers were interleaved.
    declarations = (f"    array<sampler, 16> dm_sampler_table [[id({base})]];\n"
                    f"    constant uint *dm_sampler_map [[id({base + 16})]];\n")
    source = source[:end] + declarations + source[end:]
    return source, {"version": 1, "slots": 16, "table_arg": base, "map_arg": base + 16,
                    "arg_ids": original_ids}


def fragment_quad_shuffles(source):
    """Use quad exchange only when every target provably remains in that quad."""
    if not re.search(r"\bsimd_shuffle\s*\(", source):
        return source, None
    functions = _functions(source)
    code = _code(source)

    def definition(name, body):
        definitions = list(re.finditer(r"\b" + re.escape(name) + r"\s*=(?!=)\s*([^;]+);", body))
        if len(definitions) != 1 or re.search(r"\b" + re.escape(name) + r"\s*(?:\+\+|--|(?:<<|>>|[+*/%&|^-])=)", body):
            raise LoweringError("Shuffle lane is not an immutable expression")
        return definitions[0][1].strip()

    def constant(expression, body, values, seen=()):
        expression = expression.strip()
        if re.fullmatch(r"[0-3][uU]?", expression):
            return int(expression[0])
        if expression in values:
            return values[expression]
        if re.fullmatch(r"\w+", expression) and expression not in seen:
            return constant(definition(expression, body), body, values, (*seen, expression))
        raise LoweringError("Fragment shuffle mask is not a constant within a quad")

    def lane(expression, body, values, seen=()):
        expression = expression.strip()
        if expression == "gl_SubgroupInvocationID":
            return "integer"
        cast = re.match(r"(as_type<(?:float|int|uint)>|uint|int)\(", expression)
        if cast and _matching(expression, cast.end() - 1) == len(expression) - 1:
            inner = lane(expression[cast.end():-1], body, values, seen)
            if cast[1] == "as_type<float>" and inner == "integer":
                return "float_bits"
            if cast[1] in ("as_type<int>", "as_type<uint>") and inner == "float_bits":
                return "integer"
            if cast[1] in ("uint", "int") and inner == "integer":
                return "integer"
            raise LoweringError("Fragment shuffle lane has an unsupported cast")
        if expression.count("^") == 1:
            left, right = expression.split("^")
            if lane(left, body, values, seen) == "integer":
                constant(right, body, values)
                return "integer"
        if re.fullmatch(r"\w+", expression) and expression not in seen:
            return lane(definition(expression, body), body, values, (*seen, expression))
        raise LoweringError("Fragment shuffle may cross a quad boundary")

    proven = 0
    for function in functions:
        if function.name == "spvSubgroupShuffle":
            continue
        body = code[function.body + 1:function.end]
        calls = list(re.finditer(r"\bspvSubgroupShuffle\s*\(", body))
        if not calls:
            continue
        parameters = _arguments(code[function.args + 1:function.close_args])
        parameter_names = [re.search(r"\b(\w+)\s*$", parameter)[1] for parameter in parameters]
        contexts = []
        for caller in functions:
            caller_body = code[caller.body + 1:caller.end]
            for invocation in re.finditer(r"\b" + re.escape(function.name) + r"\s*\(", caller_body):
                end = _matching(caller_body, invocation.end() - 1)
                actual = _arguments(caller_body[invocation.end():end])
                if len(actual) != len(parameters):
                    raise LoweringError("Unrecognized fragment shuffle helper invocation")
                values = {}
                for name, parameter, value in zip(parameter_names, parameters, actual):
                    if not re.search(r"\bconst\s+int\s*&", parameter):
                        continue
                    try:
                        values[name] = constant(value, caller_body, {})
                    except LoweringError:
                        pass
                contexts.append(values)
        if not contexts:
            raise LoweringError("Fragment shuffle needs bounded helper arguments")
        for context in contexts:
            for call in calls:
                end = _matching(body, call.end() - 1)
                actual = _arguments(body[call.end():end])
                if len(actual) != 2 or lane(actual[1], body, context) != "integer":
                    raise LoweringError("Unrecognized fragment shuffle target")
                proven += 1
    if not proven:
        raise LoweringError("No proven quad-local fragment exchanges")
    edits = []
    for function in functions:
        body = code[function.body + 1:function.end]
        for call in re.finditer(r"\bsimd_shuffle\s*\(", body):
            if function.name != "spvSubgroupShuffle":
                raise LoweringError("Unrecognized native fragment shuffle")
            opening = function.body + 1 + call.end() - 1
            end = _matching(source, opening)
            actual = _arguments(source[opening + 1:end])
            if len(actual) != 2 or actual[1] != "lane":
                raise LoweringError("Unrecognized subgroup shuffle wrapper")
            edits.append((function.body + 1 + call.start(), end + 1,
                          "quad_shuffle(" + actual[0] + ", ushort(uint(lane) & 3u))"))
    for start, end, replacement in sorted(edits, reverse=True):
        source = source[:start] + replacement + source[end:]
    return source, {"version": 1, "calls": proven}
