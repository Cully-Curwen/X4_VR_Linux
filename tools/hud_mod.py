"""Generate the local X4 extension that moves the HUD away from the eyes for VR.

X4 places each HUD element at a UI anchor 0.29-0.44 m in front of the pilot
(assets/ui/ui_screen_positions.xml; the radar and event monitor panels come from
assets/ui/ui_panels.xml), and each HUD script scales its presentation by a
world-space factor (config.scalingFactor etc. in ui/core/lua/*.lua). Multiplying both by
the same k scales the whole HUD about the eye point: same apparent size, k times farther
away. In stereo the unmodified HUD sits a hand's width from the face.

The extension is built from the player's own game files (nothing of Egosoft's is shipped
with this project) as extensions/x4vr_hud with a subst catalog. Usage:
    python tools/hud_mod.py [--scale 4] [--remove]
"""
import argparse
import hashlib
import re
import shutil
import time
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
EXTENSION = GAME / 'extensions' / 'x4vr_hud'
# Cutscenes (the main menu too) have their own anchors for the same presentations.
POSITIONS = ['assets/ui/ui_screen_positions.xml', 'assets/ui/ui_panels.xml',
             'assets/ui/ui_screen_positions_cutscene.xml', 'assets/cutscenecore/startmenu.xml']
SCRIPTS = [f'ui/core/lua/{name}.lua' for name in
           ('monitors', 'infobar', 'infobar2', 'infobar3', 'infobar4', 'compass', 'dialogmenu', 'subchannelbar', 'overlay')]
# Menus (widget system) stay untouched, anchors included: they are shown on the theater screen,
# and their scale is shared with cutscene anchors (main menu) that this mod does not move;
# moving only their anchors shrank them, scaling the widgets too pushed the main menu off-screen.
MENU_ANCHORS = ('uianchor_front', 'uianchor_left', 'uianchor_right')
CONNECTION = re.compile(r'<connection [^>]*tags="([^"]*)"[^>]*>.*?</connection>', re.S)
FACTOR = re.compile(r'^(\s*(?:config\.|private\.)?(?:scalingFactor|targetMonitorScaleFactor|radarScaleFactor|messageTickerScaleFactor|missionBarScaleFactor)\s*=\s*)([0-9.]+)', re.M)
POSITION = re.compile(r'<position x="([^"]+)" y="([^"]+)" z="([^"]+)"')


def read_game_files(paths):
    """The newest copy of each path from the base game's catalogs (later catalogs win)."""
    found = {}
    for cat in sorted(GAME.glob('[0-9][0-9].cat')):
        offset = 0
        with open(cat.with_suffix('.dat'), 'rb') as data:
            for line in cat.read_text(encoding='utf-8', errors='replace').splitlines():
                parts = line.rsplit(' ', 3)
                if len(parts) != 4:
                    continue
                path, size = parts[0], int(parts[1])
                if path in paths:
                    data.seek(offset)
                    found[path] = data.read(size)
                offset += size
    missing = set(paths) - set(found)
    if missing:
        raise SystemExit(f'not found in the game catalogs: {sorted(missing)}')
    return found


def scale_positions(text, k):
    count = 0

    def scaled(m):
        x, y, z = (float(v) * k for v in m.groups())
        return f'<position x="{x:.7g}" y="{y:.7g}" z="{z:.7g}"'

    def connection(m):
        nonlocal count
        tags = m.group(1).split()
        if not any(tag.startswith('uianchor_') for tag in tags) or any(tag in tags for tag in MENU_ANCHORS):
            return m.group(0)
        out, n = POSITION.subn(scaled, m.group(0))
        count += n
        return out
    return CONNECTION.sub(connection, text), count


def scale_factors(text, k):
    return FACTOR.subn(lambda m: f'{m.group(1)}{float(m.group(2)) * k:.7g}', text)


def write_catalog(folder, files):
    folder.mkdir(parents=True, exist_ok=True)
    stamp = int(time.time())
    with open(folder / 'subst_01.dat', 'wb') as data:
        lines = []
        for path, blob in files.items():
            data.write(blob)
            lines.append(f'{path} {len(blob)} {stamp} {hashlib.md5(blob).hexdigest()}')
    (folder / 'subst_01.cat').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--scale', type=float, default=2.5, help='HUD distance multiplier (1 = unchanged)')
    parser.add_argument('--remove', action='store_true', help='delete the extension')
    args = parser.parse_args()
    if args.remove or args.scale == 1:
        shutil.rmtree(EXTENSION, ignore_errors=True)
        print('removed', EXTENSION)
        return
    k = args.scale
    lua = set(SCRIPTS)
    originals = read_game_files(lua | set(POSITIONS))
    files = {}
    for path in POSITIONS:
        text, count = scale_positions(originals[path].decode('utf-8'), k)
        assert count >= 3, f'unexpected {path}: {count} positions'
        files[path] = text.encode('utf-8')
    for path in sorted(lua):
        text, count = scale_factors(originals[path].decode('utf-8'), k)
        assert count >= 1, f'no scale factor found in {path}'
        files[path] = text.encode('utf-8')
    shutil.rmtree(EXTENSION, ignore_errors=True)
    write_catalog(EXTENSION, files)
    (EXTENSION / 'content.xml').write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        f'<content id="x4vr_hud" name="X4 VR HUD distance" version="100" date="{time.strftime("%Y-%m-%d")}" save="0" enabled="1"\n'
        f'  description="Generated by X4 Rebirth: HUD {k:g}x farther away at the same apparent size, for VR. Built from your own game files.">\n'
        '</content>\n', encoding='utf-8')
    print(f'wrote {EXTENSION} (HUD x{k:g}): {len(files)} files')


if __name__ == '__main__':
    main()
