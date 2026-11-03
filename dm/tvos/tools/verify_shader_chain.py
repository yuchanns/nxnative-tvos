"""Reject incomplete P5 shader libraries before packaging the tvOS application."""

import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import re
import sys
import time

REPO = Path(__file__).resolve().parents[3]
WORK = None
PROFILE = REPO / 'dm/games/01004B10157F2000'
SLOT_KINDS = {'cbuf', 'samp', 'samp_tex', 'tex', 'img', 'tbuf', 'sbuf',
              'ubo_ptr', 'sbuf_ubo', 'tex_ubo', 'samp_ubo', 'samp_id'}
STAGES = {'pixel': 'ps', 'vertex': 'vs', 'compute': 'cs'}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def shader_keys(directory, suffix):
    return {path.name[:-len(suffix)] for path in directory.glob('*' + suffix)}


def check_library(path):
    require(path.is_file() and not path.is_symlink(), 'Missing regular library: ' + str(path))
    with path.open('rb') as stream:
        require(stream.read(4) == b'MTLB', 'Invalid library header: ' + str(path))
    require(path.stat().st_size > 64, 'Truncated library: ' + str(path))


def verify(work=WORK, profile=PROFILE, include_tvos=True, expected_count=30646):
    start = time.monotonic()
    sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
    import dmsh_fetch
    import dmsh_outputs

    chain = re.search(r'#define DMSH_CHAIN "([a-f0-9]+)"',
                      (profile / 'dmsh_chain.h').read_text()).group(1)
    with (profile / 'shader_map.tsv').open() as stream:
        records = list(csv.DictReader((line for line in stream if not line.startswith('#')), delimiter='\t'))
    expected = {row['dm_ctl_sha16']: STAGES[row['stage']] for row in records}
    require(len(expected) == len(records) == expected_count, 'Unexpected original shader key inventory')
    keys = set(expected)
    cache = work / 'shader/cache' / ('b-' + chain)
    built = json.loads((cache / '_b_build.json').read_text())
    require(len(built) == len(keys) and {row['key'] for row in built} == keys,
            'Build report does not cover the complete original shader inventory')
    failed = [row for row in built if row['st'] != 'ok']
    require(not failed, 'Shader compilation failed: ' + json.dumps(failed[:5]))
    require(shader_keys(cache, '_b.metal') == keys, 'Incomplete translated MSL source inventory')
    wanted_file = profile / 'bf_want.tsv'
    wanted = {line.strip() for line in wanted_file.read_text().splitlines()
              if line.strip() and not line.lstrip().startswith('#')} if wanted_file.exists() else set()
    variants = {}
    stages = Counter()
    for key in sorted(keys):
        reflection = json.loads((cache / (key + '_b.json')).read_text())
        require(reflection['dm_ctl'] == key and reflection['key'] == key + '_b'
                and reflection['chain'] == chain and reflection['stage'] == expected[key],
                'Source identity differs: ' + key)
        require(not reflection['drv_unfillable'], 'Unfillable driver reads: ' + key)
        require(not any(binding['kind'] == '?' or
                        (binding['kind'] in SLOT_KINDS and binding.get('reg') is None)
                        for binding in reflection['bindings']), 'Unresolved resource binding: ' + key)
        stages[reflection['stage']] += 1
        metadata = None
        if reflection['stage'] == 'ps':
            source = (cache / (key + '_b.source.metal')).read_text()
            variant, metadata = dmsh_fetch.variant(source, reflection['bindings'], om_only_ok=key in wanted)
            if variant:
                _, outputs = dmsh_outputs.specialize(variant, conditional=True)
                if outputs:
                    metadata['fragment_outputs'] = outputs
                variants[key] = metadata
            else:
                metadata = None
        require(reflection.get('fetch') == metadata, 'Missing or changed generated variant: ' + key)

    targets = {'mac': cache, 'ios': work / 'shader/ios' / ('b-' + chain)}
    if include_tvos:
        targets['tvos'] = work / 'shader/tvos' / ('b-' + chain)
    for target, directory in targets.items():
        require(shader_keys(directory, '_b.metallib') == keys, 'Incomplete base library inventory: ' + target)
        require(shader_keys(directory, '_b.json') == keys, 'Incomplete reflection inventory: ' + target)
        require(shader_keys(directory, '_bf.metallib') == set(variants), 'Incomplete variant inventory: ' + target)
        for key in sorted(keys):
            check_library(directory / (key + '_b.metallib'))
            reflection = json.loads((directory / (key + '_b.json')).read_text())
            require(reflection['dm_ctl'] == key and reflection['chain'] == chain
                    and reflection['stage'] == expected[key] and reflection.get('fetch') == variants.get(key),
                    'Target reflection differs: ' + target + '/' + key)
            if key in variants:
                check_library(directory / (key + '_bf.metallib'))
    if include_tvos:
        proof = json.loads((targets['tvos'] / 'tvos-build.json').read_text())
        require(proof['sdk'] == 'appletvos' and proof['shaders'] == len(keys)
                and proof['variants'] == len(variants) and not proof['failures'], 'Incomplete tvOS build proof')
        for name in ('c1_full.tsv', 'cb_extent.tsv', 'pso_manifest.tsv'):
            require((profile / name).stat().st_size > 0, 'Missing runtime table: ' + name)
    return {'complete': True, 'chain': chain, 'keys': len(keys), 'variants': len(variants),
            'stages': dict(stages), 'targets': list(targets),
            'map_sha256': hashlib.sha256((profile / 'shader_map.tsv').read_bytes()).hexdigest(),
            'seconds': round(time.monotonic() - start, 2), 'guest_started': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--profile', type=Path, default=PROFILE)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--without-tvos', action='store_true')
    args = parser.parse_args()
    result = verify(work=args.work, profile=args.profile, include_tvos=not args.without_tvos)
    output = args.output
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
