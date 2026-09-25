"""Reflect observed SPIR-V descriptor blocks without executing or modifying shaders.

Instruction/decoration numbers follow Khronos SPIRV-Headers unified1/spirv.hpp.
This is targeted reflection, not a complete SPIR-V validator. Camera identity uses
actual debug member names and is left unknown when those names were stripped.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct


def literal(words):
    return struct.pack('<' + 'I'*len(words), *words).split(b'\0', 1)[0].decode('utf-8', errors='replace')


def reflect(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError('Truncated or non-word-aligned SPIR-V')
    words = struct.unpack('<' + 'I'*(len(data)//4), data)
    if words[0] != 0x07230203:
        raise ValueError('Unsupported SPIR-V magic/byte order')
    names, member_names, decorations, member_decorations = {}, {}, {}, {}
    types, pointers, variables, entrypoints = {}, {}, [], []
    offset = 5
    while offset < len(words):
        count, op = words[offset] >> 16, words[offset] & 0xffff
        if not count or offset + count > len(words):
            raise ValueError(f'Invalid SPIR-V instruction at word {offset}')
        args = words[offset+1:offset+count]
        if op == 5 and len(args) >= 2:
            names[args[0]] = literal(args[1:])
        elif op == 6 and len(args) >= 3:
            member_names[(args[0], args[1])] = literal(args[2:])
        elif op == 15 and len(args) >= 3:
            entrypoints.append({'execution_model': args[0], 'name': literal(args[2:])})
        elif op == 71 and len(args) >= 2:
            decorations.setdefault(args[0], {})[args[1]] = list(args[2:])
        elif op == 72 and len(args) >= 3:
            member_decorations.setdefault((args[0], args[1]), {})[args[2]] = list(args[3:])
        elif op == 30 and args:
            types[args[0]] = args[1:]
        elif op == 32 and len(args) == 3:
            pointers[args[0]] = (args[1], args[2])
        elif op == 59 and len(args) >= 3:
            variables.append((args[0], args[1], args[2]))
        offset += count
    blocks = []
    for type_id, variable, storage in variables:
        if storage not in (2, 12):  # Uniform / StorageBuffer
            continue
        pointer = pointers.get(type_id)
        if not pointer or pointer[1] not in types:
            continue
        struct_id = pointer[1]
        var_deco = decorations.get(variable, {})
        members = []
        for i, member_type in enumerate(types[struct_id]):
            deco = member_decorations.get((struct_id, i), {})
            members.append({
                'index': i, 'name': member_names.get((struct_id, i)), 'type_id': member_type,
                'offset': deco.get(35, [None])[0], 'matrix_stride': deco.get(7, [None])[0],
                'matrix_order': 'row_major' if 4 in deco else 'column_major' if 5 in deco else None,
            })
        member_set = {m['name'] for m in members}
        identity = 'camera' if {'M_view', 'M_projection', 'M_viewprojection'} <= member_set else (
            'world' if {'M_world', 'M_worldviewprojection'} <= member_set else 'other_or_unknown')
        blocks.append({
            'identity': identity, 'name': names.get(struct_id), 'variable_name': names.get(variable),
            'set': var_deco.get(34, [None])[0], 'binding': var_deco.get(33, [None])[0],
            'storage_class': storage, 'members': members,
        })
    return {'sha256': hashlib.sha256(data).hexdigest(), 'entrypoints': entrypoints, 'blocks': blocks}


def summarize(directory):
    directory = directory.resolve()
    text = (directory/'events.jsonl').read_text(encoding='utf-8')
    # A live writer can leave an incomplete trailing line; never interpret it as a complete event.
    lines = text.splitlines()
    if text and not text.endswith('\n'):
        lines = lines[:-1]
    events = [json.loads(line) for line in lines]
    counts = Counter(e['event'] for e in events)
    shaders, blocks, errors = {}, {}, []
    for e in events:
        if e['event'] != 'shader_module' or 'file' not in e:
            continue
        filename = e['file']
        source = (directory/filename).resolve()
        if not source.is_relative_to(directory):
            raise ValueError('Shader path escapes capture directory')
        try:
            shader = reflect(source.read_bytes())
            shaders[filename] = shader
            for block in shader['blocks']:
                if block['identity'] not in ('camera', 'world'):
                    continue
                # Type IDs are module-local and must not distinguish otherwise identical layouts.
                comparable = dict(block)
                comparable['members'] = [{k: v for k, v in m.items() if k != 'type_id'} for m in block['members']]
                signature = json.dumps(comparable, sort_keys=True)
                group = blocks.setdefault(signature, {'layout': comparable, 'shader_files': []})
                if filename not in group['shader_files']:
                    group['shader_files'].append(filename)
        except (ValueError, OSError, IndexError) as exc:
            errors.append({'file': filename, 'error': str(exc)})
    # Resolve module handles in event order: handles may be reused after destruction.
    live_modules, pipeline_counts = {}, Counter()
    for e in events:
        if e['event'] == 'shader_module':
            live_modules[e['module']] = e.get('file')
        elif e['event'] == 'graphics_pipeline':
            identities = set()
            for stage in e['shaders']:
                shader = shaders.get(live_modules.get(stage['module']), {})
                identities.update(b['identity'] for b in shader.get('blocks', []))
            for identity in identities:
                pipeline_counts[identity] += 1
    return {
        'capture': str(directory), 'event_counts': dict(counts),
        'applications': [e for e in events if e['event'] == 'instance_created'],
        'devices': [e for e in events if e['event'] == 'device_created'],
        'swapchains': [e for e in events if e['event'] == 'swapchain'],
        'last_present': next((e for e in reversed(events) if e['event'] == 'present'), None),
        'reflected_modules': len(shaders), 'unique_modules': len({s['sha256'] for s in shaders.values()}),
        'named_camera_world_layouts': list(blocks.values()),
        'graphics_pipelines_by_named_block': dict(pipeline_counts), 'errors': errors,
        'limitations': 'Observed modules and sampled descriptor calls only. No camera-buffer contents, engine function addresses, stereo rendering, or headset submission are proven. Names may be stripped. Extension overrides and runtime shader variants are represented only if observed.',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = summarize(args.capture)
    if args.output:
        args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    summary = {k: v for k, v in report.items() if k != 'named_camera_world_layouts'}
    summary['named_layouts'] = [{
        'identity': g['layout']['identity'], 'set': g['layout']['set'], 'binding': g['layout']['binding'],
        'members': len(g['layout']['members']), 'shader_count': len(g['shader_files']),
    } for g in report['named_camera_world_layouts']]
    print(json.dumps(summary, indent=2))
    if report['errors']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
