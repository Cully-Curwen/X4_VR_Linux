"""Extract selected renderer text from local numbered X4 catalogs for inspection.

Preserves the game archives. No extracted material is installed as an extension.
Later numbered base catalogs win; extension overrides are not included.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re


WANTED = {
    'shadergl/glsl/common.glsl',
    'shadergl/glsl/common_vert.glsl',
    'shadergl/glsl/common_frag.glsl',
    'shadergl/glsl/vertex.glsl',
    'shadergl/glsl/high/common.vert.glsl',
    'shadergl/glsl/common_taa.glsl',
    'shadergl/glsl/p1/deferred/gbuffer_access.glsl',
    'shadergl/glsl/p1/deferred/lighting_common.glsl',
    'libraries/camerasettings.xml',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    found = {}
    catalogs = sorted((p for p in args.game.glob('*.cat') if re.fullmatch(r'\d+\.cat', p.name)),
                      key=lambda p: int(p.stem))
    for cat in catalogs:
        offset = 0
        with cat.with_suffix('.dat').open('rb') as dat:
            for line in cat.read_text(encoding='utf-8').splitlines():
                name, size, timestamp, checksum = line.rsplit(' ', 3)
                size = int(size)
                if name in WANTED:
                    if size > 4*1024*1024:
                        raise ValueError(f'Unexpectedly large source file: {name}')
                    target = (root / PurePosixPath(name)).resolve()
                    if not target.is_relative_to(root):
                        raise ValueError('Catalog path escapes extraction root')
                    dat.seek(offset)
                    data = dat.read(size)
                    if len(data) != size:
                        raise ValueError(f'Truncated archive: {cat}')
                    data.decode('utf-8')  # Refuse binary or unexpected encoding.
                    if target.exists() and name not in found and target.read_bytes() != data:
                        raise ValueError(f'Existing extraction differs; use a fresh output directory: {target}')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(data)
                    found[name] = {'catalog': cat.name, 'offset': offset, 'size': size,
                                   'sha256': hashlib.sha256(data).hexdigest()}
                offset += size
    (root / 'extraction.json').write_text(json.dumps(found, indent=2)+'\n', encoding='utf-8')
    print(f'Extracted {len(found)}/{len(WANTED)} sources to {root}')
    if missing := WANTED - found.keys():
        raise ValueError(f'Missing sources: {sorted(missing)}')


if __name__ == '__main__':
    main()
