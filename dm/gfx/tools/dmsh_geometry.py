"""Geometry-as-compute and canonical vertex capture for the offline Metal chain."""
import re

INPUT_BUFFER, INFO_BUFFER, OUTPUT_BUFFER, INDEX_BUFFER, REMAP_BUFFER = range(8, 13)
INPUT_WORDS = 142


def entry(source, stage):
    matches = list(re.finditer(rf'\b{stage}\s+(\w+)\s+(\w+)\s*\(', source))
    if len(matches) != 1:
        raise ValueError(f'Expected one {stage} entry')
    match = matches[0]
    start = match.end()
    depth = 1
    for end in range(start, len(source)):
        depth += (source[end] == '(') - (source[end] == ')')
        if not depth:
            return match, source[start:end], end + 1
    raise ValueError('Unterminated Metal entry parameters')


def discard(source):
    """Keep vertex storage side effects in Metal's void rasterizer-discard entry."""
    match, parameters, end = entry(source, 'vertex')
    body, count = re.subn(r'\breturn\s+out\s*;', 'return;', source[end:])
    if not count:
        raise ValueError('Rasterizer discard requires structured output returns')
    return f'\nvertex void main0_discard({parameters})' + body


def capture(source):
    """Append a capture entry using the unmodified, pre-geometry position."""
    match, parameters, end = entry(source, 'vertex')
    fields = re.search(r'\bstruct\s+' + match[1] + r'\s*\{(.*?)\};', source, re.S)
    if not fields:
        raise ValueError('Missing vertex output structure')
    outputs = []
    for field in re.finditer(r'\b(float|uint|int)([234]?)\s+(\w+)\s*\[\[(.*?)\]\]\s*;', fields[1]):
        kind, width, name, semantic = field.groups()
        width = int(width or 1)
        if semantic.startswith('position'):
            offset = 0
        elif semantic == 'point_size':
            offset = 4
        elif semantic == 'render_target_array_index':
            offset = 13
        elif user := re.match(r'user\(locn(\d+)\)', semantic):
            offset = 14 + int(user[1]) * 4
        else:
            raise ValueError(f'Unsupported geometry capture output {semantic}')
        if offset + width > INPUT_WORDS:
            raise ValueError('Geometry capture output exceeds the register ABI')
        outputs.extend((offset + component, 'out.' + name + ('.' + 'xyzw'[component] if width > 1 else ''), kind) for component in range(width))
    # Clip distances are arrays in SPIRV-Cross's output structure.
    for field in re.finditer(r'\bfloat\s+(\w+)\s*(?:\[(\d+)\]\s*\[\[clip_distance\]\]|\[\[clip_distance\]\]\s*\[(\d+)\])\s*;', fields[1]):
        count = int(field[2] or field[3])
        if count > 8:
            raise ValueError('Too many geometry clip distances')
        outputs.extend((5 + index, f'out.{field[1]}[{index}]', 'float') for index in range(count))
    builtins = {}
    for builtin in ('vertex_id', 'instance_id'):
        found = re.search(r'\buint\s+(\w+)\s*\[\[' + builtin + r'\]\]', parameters)
        builtins[builtin] = found[1] if found else 'dm_capture_' + builtin
        if not found:
            parameters += (', ' if parameters.strip() else '') + f'uint {builtins[builtin]} [[{builtin}]]'
    parameters += f', device uint* dm_capture_output [[buffer({INPUT_BUFFER})]], constant uint* dm_capture_info [[buffer({INFO_BUFFER})]], const device uint* dm_capture_ids [[buffer(14)]]'
    body = source[end:]
    if not re.search(r'\breturn\s+out\s*;', body):
        raise ValueError('Geometry capture requires structured output returns')
    stores = ('{ uint dm_capture_id = ' + builtins['vertex_id'] + ' - dm_capture_info[2];'
              ' if (dm_capture_info[6]) { uint dm_lo = 0, dm_hi = dm_capture_info[0];'
              ' while (dm_lo < dm_hi) { uint dm_mid = dm_lo + (dm_hi - dm_lo) / 2;'
              ' if (dm_capture_ids[dm_mid] < dm_capture_id) dm_lo = dm_mid + 1; else dm_hi = dm_mid; }'
              ' if (dm_lo == dm_capture_info[0] || dm_capture_ids[dm_lo] != dm_capture_id) return; dm_capture_id = dm_lo; }'
              ' ulong dm_capture_record = ulong(dm_capture_id)'
              ' + ulong(' + builtins['instance_id'] + ' - dm_capture_info[3]) * dm_capture_info[0];'
              f' device uint* dm_capture_at = dm_capture_output + dm_capture_record * {INPUT_WORDS};'
              f' for (uint dm_i = 0; dm_i < {INPUT_WORDS}; ++dm_i) dm_capture_at[dm_i] = 0u;'
              ' dm_capture_at[3] = dm_capture_at[4] = 0x3f800000u;'
              ' for (uint dm_i = 0; dm_i < 32; ++dm_i) dm_capture_at[17 + 4 * dm_i] = 0x3f800000u;')
    for offset, expression, _ in outputs:
        stores += f' dm_capture_at[{offset}] = as_type<uint>({expression});'
    stores += ' return; }'
    body = re.sub(r'\breturn\s+out\s*;', lambda _: stores, body)
    result = f'\nvertex void main0_gc({parameters})' + body
    loads = f'{match[1]} out = {{}};'
    for offset, expression, kind in outputs:
        loads += f' {expression} = as_type<{kind}>(dm_geometry_vertices[ulong(dm_vertex) * {INPUT_WORDS} + {offset}]);'
    position = next((expression.rsplit('.', 1)[0] for offset, expression, _ in outputs if offset == 0), None)
    for suffix, depth in (('_z', ''), ('', f' {position}.z = ({position}.z + {position}.w) * 0.5f;' if position else '')):
        result += f'\nvertex {match[1]} main0_gp{suffix}(const device uint* dm_geometry_vertices [[buffer({INPUT_BUFFER})]], uint dm_vertex [[vertex_id]])' + '{' + loads + depth + ' return out; }\n'
    return result


def compute(source):
    """Move the translator's reserved resources outside the guest argument buffer."""
    match, parameters, end = entry(source, 'kernel')
    sets = {int(m[1]): m for m in re.finditer(r'struct spvDescriptorSetBuffer(\d+)\s*\{(.*?)\};', source, re.S)}
    if set(sets) != {0, 1, 2}:
        raise ValueError('Unexpected geometry descriptor sets')
    info = re.search(r'constant\s+(\w+)\*\s+vb_info\s*\[\[id\(\d+\)\]\];', sets[0][2])
    if not info:
        raise ValueError('Missing geometry state buffer')
    extra = [f'constant {info[1]}* dm_geometry_info [[buffer({INFO_BUFFER})]]']
    body = source[end:].replace('spvDescriptorSet0.vb_info', 'dm_geometry_info')
    resources = {'vertex_output': ('input', INPUT_BUFFER), 'geometry_vb_output': ('vertices', OUTPUT_BUFFER), 'geometry_ib_output': ('indices', INDEX_BUFFER)}
    for field in re.finditer(r'(const\s+)?device\s+(\w+)\*\s+(\w+)\s*\[\[id\(\d+\)\]\];', sets[1][2]):
        qualifier, kind, name = field.groups()
        if name not in resources:
            raise ValueError(f'Unknown geometry storage {name}')
        replacement, buffer = resources[name]
        extra.append(f'{qualifier or ""}device {kind}* dm_geometry_{replacement} [[buffer({buffer})]]')
        body = body.replace('spvDescriptorSet1.' + name, 'dm_geometry_' + replacement)
    extra.append(f'const device uint* dm_geometry_remap [[buffer({REMAP_BUFFER})]]')
    body = re.sub(r'spvDescriptorSet2\.trb_data\.read\(uint\((.*?)\)\)\.x', r'as_type<float>(dm_geometry_remap[uint(\1)])', body)
    if 'spvDescriptorSet1' in body or 'spvDescriptorSet2' in body:
        raise ValueError('Incomplete geometry resource lowering')
    for number in (1, 2):
        parameters, count = re.subn(r'constant spvDescriptorSetBuffer' + str(number) + r'& spvDescriptorSet' + str(number) + r' \[\[buffer\(' + str(number) + r'\)\]\],?\s*', '', parameters)
        if count != 1:
            raise ValueError('Missing geometry descriptor argument')
    before = source[:match.start()]
    for number in sorted(sets, key=lambda n: sets[n].start(), reverse=True):
        declaration = sets[number]
        if number:
            before = before[:declaration.start()] + before[declaration.end():]
        else:
            fields = declaration[2].replace(info[0], '')
            ids = sorted(map(int, re.findall(r'\[\[id\((\d+)\)\]\]', fields)))
            remap = {old: new for new, old in enumerate(ids)}
            fields = re.sub(r'\[\[id\((\d+)\)\]\]', lambda m: f'[[id({remap[int(m[1])]})]]', fields)
            before = before[:declaration.start(2)] + fields + before[declaration.end(2):]
    return before + f'kernel void {match[2]}({parameters.rstrip().rstrip(",")}, ' + ', '.join(extra) + ')' + body


def passthrough(source, points):
    match, parameters, end = entry(source, 'vertex')
    declaration = re.search(r'struct spvDescriptorSetBuffer1\s*\{\s*const device (\w+)\* vb_input \[\[id\(0\)\]\];\s*\};', source)
    if not declaration:
        raise ValueError('Unexpected geometry passthrough storage')
    parameters, count = re.subn(r'constant spvDescriptorSetBuffer1& spvDescriptorSet1 \[\[buffer\(1\)\]\]', f'const device {declaration[1]}* dm_geometry_vertices [[buffer({INPUT_BUFFER})]]', parameters)
    if count != 1:
        raise ValueError('Missing geometry passthrough argument')
    before = source[:declaration.start()] + source[declaration.end():match.start()]
    body = source[end:].replace('spvDescriptorSet1.vb_input', 'dm_geometry_vertices')
    if not points:
        before = re.sub(r'\s*float gl_PointSize \[\[point_size\]\];', '', before)
        body = re.sub(r'\s*out\.gl_PointSize\s*=[^;]+;', '', body)
    return before + f'vertex {match[1]} {match[2]}({parameters})' + body


def unique(source, prefix):
    """Combine independently generated stages without global symbol collisions."""
    return re.sub(r'\b(?:_\d+\w*|spv\w+|main0\w*)\b', lambda m: prefix + '_' + m[0], source)
