"""Fit the game image's projection from star positions under a known synthetic yaw.

Stars are effectively at infinity, so a pure camera rotation maps them exactly by the
projection model. Compares pinhole (x = f tan a) against equidistant (x = k a).
Usage: python star_fov.py <pid> [yaw_degrees]
"""
import math
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vr_calibrate as v  # noqa: E402


def stars(img, mask_rows):
    """Point-like local maxima (3x3) that are bright but isolated (not UI/geometry edges)."""
    r0, r1 = mask_rows
    sub = img[r0:r1]
    thr = max(60.0, float(np.percentile(sub, 99.5)))
    c = sub[2:-2, 2:-2]
    peak = c > thr
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dy or dx:
                peak &= c >= sub[2+dy:sub.shape[0]-2+dy, 2+dx:sub.shape[1]-2+dx]
    # isolation: ring at distance 2 must be much darker than the peak
    ring = np.maximum.reduce([sub[0:-4, 2:-2], sub[4:, 2:-2], sub[2:-2, 0:-4], sub[2:-2, 4:]])
    peak &= ring < 0.6*c
    ys, xs = np.nonzero(peak)
    return np.stack([xs+2.0, ys+2.0+r0, c[ys, xs]], 1)


class Tree:
    def __init__(self, pts):
        self.pts = pts

    def query(self, q):
        d = np.sqrt(((q[:, None, :]-self.pts[None, :, :])**2).sum(2))
        i = d.argmin(1)
        return d[np.arange(len(q)), i], i


def main():
    pid = int(sys.argv[1])
    yaw = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    v.Camera(pid)  # verifies the executable
    v.synth(); time.sleep(2.5); a = v.dump()
    v.synth(base=(0, 0, 0, yaw, 0, 0)); time.sleep(2.5); b = v.dump()
    v.write_settings()
    A, B = a['images'][0], b['images'][0]
    ox, oy = a['offset']
    h, w = A.shape
    gw, gh = w-2*ox, h-2*oy
    cx, cy = ox+gw/2, oy+gh/2
    rows = (oy+10, int(oy+gh*0.55))  # upper part: mostly space through the canopy
    sa, sb = stars(A, rows), stars(B, rows)
    print(f'stars: {len(sa)} in reference, {len(sb)} rotated; image {w}x{h}, game {gw}x{gh}')
    th = math.radians(yaw)

    def project(model, k, xs, ys):
        # direction from reference pixel, rotate about vertical by the synthetic yaw, reproject
        if model == 'pinhole':
            tx, ty = (xs-cx)/k, (ys-cy)/k
            d = np.stack([tx, ty, np.ones_like(tx)], 1)
        else:  # equidistant: radius in pixels = k * angle
            rx, ry = xs-cx, ys-cy
            rr = np.hypot(rx, ry)/k
            s = np.where(rr > 1e-9, np.sin(rr)/np.maximum(rr, 1e-9), 1.0)
            d = np.stack([rx/k*s, ry/k*s, np.cos(rr)], 1)
        best = []
        for sign in (1, -1):
            c, s_ = math.cos(sign*th), math.sin(sign*th)
            x2 = c*d[:, 0]+s_*d[:, 2]
            z2 = -s_*d[:, 0]+c*d[:, 2]
            y2 = d[:, 1]
            if model == 'pinhole':
                px, py = cx+k*x2/z2, cy+k*y2/z2
            else:
                n = np.linalg.norm(np.stack([x2, y2, z2], 1), axis=1)
                ang = np.arccos(np.clip(z2/n, -1, 1))
                rad = np.hypot(x2, y2)
                px, py = cx+k*ang*x2/np.maximum(rad, 1e-9), cy+k*ang*y2/np.maximum(rad, 1e-9)
            best.append((px, py))
        return best

    tree = Tree(sb[:, :2])
    for model in ('pinhole', 'equidistant'):
        results = []
        for k in np.arange(480, 760, 2.0):
            for px, py in project(model, k, sa[:, 0], sa[:, 1]):
                dist, _ = tree.query(np.stack([px, py], 1))
                good = dist < 3
                results.append((good.sum(), -np.median(dist[good]) if good.any() else -99, k))
        count, neg_err, k = max(results)
        print(f'{model:12s}: best k={k:.0f} px, {count} of {len(sa)} stars matched within 3 px, median err {-neg_err:.2f}px')
    print(f'expected pinhole f from game_tan_y: {gh/(2*v.BASE["game_tan_y"]):.1f} px/tan')


if __name__ == '__main__':
    main()
