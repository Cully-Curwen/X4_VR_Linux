"""Does the pose submitted with each eye image belong to the frame X4 rendered? Turn the head
steadily while this runs (ship still). Per presented frame it compares the rotation between
consecutive frames of X4's main camera (F events, first bind before the present) with that of
the submitted head pose (S events), and reports at which frame lag they agree. 0 = in sync.
It also checks each frame's eye side (C and X events): the right eye's camera sits half an eye
distance to the right of its neighbours', the left eye's to the left. That catches wrong-eye
frames and whole-view eye swaps, head still or moving, in the cockpit or on foot.
usage: head_sync.py [--no-request] [--last seconds]"""
import sys, time
from pathlib import Path
import numpy as np

CAPTURES = Path(__file__).resolve().parents[1]/'reports'/'captures'

def request():
    out = CAPTURES/'trace.txt'; before = out.stat().st_mtime if out.exists() else 0
    (CAPTURES/'trace.request').write_text('1')
    while not out.exists() or out.stat().st_mtime == before: time.sleep(0.2)
    time.sleep(0.5)

def angles(vectors):  # degrees between consecutive unit vectors
    v = vectors/np.linalg.norm(vectors, axis=1, keepdims=True)
    return np.degrees(np.arccos(np.clip(np.sum(v[1:]*v[:-1], axis=1), -1, 1)))

def frames(ev):
    """Per presented frame: the first camera bind's C, F, X before its present, and the submitted eye and head z axis (S)."""
    presents = [(v, t) for k, v, t, _ in ev if k == 'P']
    binds = {kind: [(t, x) for k, v, t, x in ev if k == kind] for kind in 'CFX'}
    submitted = {v//2: (v % 2, x) for k, v, t, x in ev if k == 'S'}
    rows = []
    for (_, t0), (n, t1) in zip(presents, presents[1:]):
        row = {'n': n, 't': t1}
        for kind, events in binds.items():
            inside = [x for t, x in events if t0 < t <= t1]
            if inside: row[kind] = inside[0]
        if n in submitted: row['eye'], row['S'] = submitted[n]
        rows.append(row)
    return rows

def pose_lag(rows):
    rows = [r for r in rows if 'F' in r and 'S' in r]
    if len(rows) < 50: return print(f'pose lag: only {len(rows)} frames with camera and pose')
    cam = angles(np.array([r['F'] for r in rows])); pose = angles(np.array([r['S'] for r in rows]))
    print(f'{len(rows)} frames; head turn per frame: median {np.median(pose):.3f} deg, p90 {np.percentile(pose, 90):.3f}')
    best = None
    for lag in range(-3, 4):  # pose[i+lag] vs cam[i]
        a = cam[max(0, -lag):len(cam)-max(0, lag)]; b = pose[max(0, lag):len(pose)-max(0, -lag)]
        r = np.corrcoef(a, b)[0, 1]; rms = np.sqrt(np.mean((a-b)**2))
        print(f'  lag {lag:+d}: correlation {r:.3f}, rms difference {rms:.3f} deg')
        if best is None or r > best[1]: best = (lag, r)
    print(f'best lag {best[0]:+d} (positive: the submitted pose is older than the rendered camera)')
    ratio = np.median(cam[pose > 0.05]/pose[pose > 0.05]) if np.any(pose > 0.05) else float('nan')
    print(f'camera/pose rotation ratio (1 = X4 turns 1:1 with the head): {ratio:.3f}')

def eye_side(rows):
    # The camera minus the mean of its neighbours: half the eye distance along the camera x axis
    # while the camera otherwise moves smoothly (walking, flying, turning the head).
    sides, skipped = [], 0
    for a, b, c in zip(rows, rows[1:], rows[2:]):
        if not all('C' in r for r in (a, b, c)) or 'X' not in b or 'eye' not in b: continue
        d = b['C']-(a['C']+c['C'])/2
        sides.append((b['eye'], float(d@(b['X']/np.linalg.norm(b['X']))), float(np.linalg.norm(d)), b['t']))
    if len(sides) < 50: return print(f'eye side: only {len(sides)} frames')
    size = np.median([s[2] for s in sides])
    usable = [s for s in sides if 0.5 < s[2]/size < 1.5]
    # ponytail: sign from X4's view matrix convention; the cockpit (verified in the headset) must read ~100%
    right = [s for s in usable if s[0] == 1]; left = [s for s in usable if s[0] == 0]
    ok_r = sum(s[1] > 0 for s in right); ok_l = sum(s[1] < 0 for s in left)
    print(f'eye side: {len(usable)} of {len(sides)} frames usable (offset median {size:.4f}); '
          f'right eye on +x {ok_r}/{len(right)}, left eye on -x {ok_l}/{len(left)} '
          f'-> {100*(ok_r+ok_l)/max(1, len(usable)):.1f}% on the expected side')
    end = rows[-1]['t']
    wrong = [round((end-s[3])/1e6, 1) for s in usable if (s[1] > 0) != (s[0] == 1)]
    if wrong and len(wrong) < len(usable)/2: print(f'  wrong side (s before the dump): {wrong[:30]}{" ..." if len(wrong) > 30 else ""}')

def main():
    if '--no-request' not in sys.argv: request()
    ev = [l.split() for l in (CAPTURES/'trace.txt').read_text().splitlines()]
    ev = [(k, int(v), float(t), np.array([float(a), float(b), float(c)])) for k, v, h, t, th, a, b, c in ev]
    rows = frames(ev)
    if '--last' in sys.argv:  # only the last N seconds, e.g. since a live setting changed
        seconds = float(sys.argv[sys.argv.index('--last')+1])
        rows = [r for r in rows if r['t'] >= rows[-1]['t']-seconds*1e6]
    pose_lag(rows)
    eye_side(rows)

if __name__ == '__main__':
    main()
