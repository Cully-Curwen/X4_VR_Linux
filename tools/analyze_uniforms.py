"""Analyze bounded CPU snapshots; observations are not proof of GPU-visible state."""
import argparse
import bisect
import hashlib
import json
import math
from pathlib import Path
import struct
from inspect_x4 import PE


def matrix(data, offset):
    values = struct.unpack_from('<16f', data, offset)
    return [[values[c*4+r] for c in range(4)] for r in range(4)]


def multiply(a, b):
    return [[sum(a[r][k]*b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def error(a, b):
    return max(abs(a[r][c]-b[r][c]) for r in range(4) for c in range(4))


def camera_candidate(data):
    if len(data) < 576:
        return {'status': 'too_short'}
    view, projection, vp, inverse = [matrix(data, offset) for offset in (0, 64, 448, 512)]
    values = [v for m in (view, projection, vp, inverse) for row in m for v in row]
    if not all(math.isfinite(v) for v in values):
        return {'status': 'nonfinite'}
    identity = [[float(r == c) for c in range(4)] for r in range(4)]
    vp_error = error(multiply(projection, view), vp)
    inverse_error = error(multiply(view, inverse), identity)
    status = 'relationships_consistent' if vp_error < 1e-3 and inverse_error < 1e-3 else 'relationships_inconsistent'
    if not any(values):
        status = 'all_zero'
    return dict(status=status, view=view, projection=projection, inverse_view=inverse,
        projection_view_error=vp_error, view_inverse_error=inverse_error)


def runtime_functions(pe):
    rva, size = struct.unpack_from('<II', pe.data, pe.directories+3*8)
    offset = pe.offset(rva)
    return sorted(struct.iter_unpack('<III', pe.data[offset:offset+size]))


def native_relationships(camera, temporary, uniform):
    result = {}
    if temporary is not None and len(temporary) == len(uniform):
        result['temporary_matches_uniform'] = temporary == uniform
        result['temporary_differing_bytes'] = sum(a != b for a, b in zip(temporary, uniform))
    if camera is not None and len(camera) >= 0x200 and len(uniform) >= 576:
        result['native_view_matches_uniform'] = camera[0x40:0x80] == uniform[:64]
        result['native_inverse_matches_uniform'] = camera[:64] == uniform[512:576]
        projection, jitter = matrix(camera, 0x140), matrix(camera, 0x1c0)
        # Static SSE instructions multiply jitter*projection then flip output Y.
        product = multiply(jitter, projection)
        product[1] = [-v for v in product[1]]
        values = [v for row in product for v in row]
        if all(math.isfinite(v) for v in values):
            result['native_projection_error'] = error(product, matrix(uniform, 64))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pe = PE(args.exe)
    functions = runtime_functions(pe)
    starts = [f[0] for f in functions]
    def locate(return_rva):
        # Return addresses point after a call; subtract one for boundary lookup.
        index = bisect.bisect_right(starts, return_rva-1)-1
        result = {'return_rva': hex(return_rva)}
        if index >= 0 and functions[index][0] <= return_rva-1 < functions[index][1]:
            start, end, unwind = functions[index]
            result.update(function_begin=hex(start), function_end=hex(end), unwind=hex(unwind))
        return result
    samples = []
    for line in (args.capture/'events.jsonl').read_text().splitlines():
        event = json.loads(line)
        if event['event'] != 'uniform_snapshot':
            continue
        event['stack_functions'] = [locate(rva) for rva in event.pop('exe_return_rvas', [])]
        if event.get('file') and event['slot_candidate'] == 1:
            data = (args.capture/event['file']).read_bytes()
            event['camera_candidate'] = camera_candidate(data)
            if native := event.get('native_camera'):
                camera = (args.capture/native['native-camera']).read_bytes() if native.get('native-camera') else None
                temporary = (args.capture/native['native-uniform']).read_bytes() if native.get('native-uniform') else None
                native['relationships'] = native_relationships(camera, temporary, data)
        samples.append(event)
    report = dict(executable_sha256=hashlib.sha256(pe.data).hexdigest(),
        capture=str(args.capture.resolve()), samples=samples,
        limitations='Set slots are candidates, not semantic proof. CPU samples can precede final writes or differ from GPU-visible data. Unwind ranges identify containing code, not camera-update entry points.')
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(f'{len(samples)} samples analyzed: {args.output}')


if __name__ == '__main__':
    main()
