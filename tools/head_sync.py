"""Does the pose submitted with each eye image belong to the frame X4 rendered? Turn the head
steadily while this runs (ship still). Per presented frame it compares the rotation between
consecutive frames of X4's main camera (F events, first bind before the present) with that of
the submitted head pose (S events), and reports at which frame lag they agree. 0 = in sync.
usage: head_sync.py [--no-request]"""
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

def main():
    if '--no-request' not in sys.argv: request()
    ev = [l.split() for l in (CAPTURES/'trace.txt').read_text().splitlines()]
    ev = [(k, int(v), float(t), np.array([float(a), float(b), float(c)])) for k, v, h, t, th, a, b, c in ev]
    presents = [(v, t) for k, v, t, _ in ev if k == 'P']
    cams = [(t, x) for k, v, t, x in ev if k == 'F']
    submitted = {v//2: x for k, v, t, x in ev if k == 'S'}
    rows = []
    for (prev, t0), (n, t1) in zip(presents, presents[1:]):
        inside = [x for t, x in cams if t0 < t <= t1]
        if inside and n in submitted: rows.append((n, inside[0], submitted[n]))
    if len(rows) < 50: sys.exit(f'only {len(rows)} frames with camera and pose')
    cam = angles(np.array([r[1] for r in rows])); pose = angles(np.array([r[2] for r in rows]))
    print(f'{len(rows)} frames; head turn per frame: median {np.median(pose):.3f} deg, p90 {np.percentile(pose, 90):.3f}')
    best = None
    for lag in range(-3, 4):  # pose[i+lag] vs cam[i]
        a = cam[max(0, -lag):len(cam)-max(0, lag)]; b = pose[max(0, lag):len(pose)-max(0, -lag)]
        r = np.corrcoef(a, b)[0, 1]; rms = np.sqrt(np.mean((a-b)**2))
        print(f'  lag {lag:+d}: correlation {r:.3f}, rms difference {rms:.3f} deg')
        if best is None or r > best[1]: best = (lag, r)
    print(f'best lag {best[0]:+d} (positive: the submitted pose is newer than the rendered camera)')
    ratio = np.median(cam[pose > 0.05]/pose[pose > 0.05]) if np.any(pose > 0.05) else float('nan')
    print(f'camera/pose rotation ratio (1 = X4 turns 1:1 with the head): {ratio:.3f}')

if __name__ == '__main__':
    main()
