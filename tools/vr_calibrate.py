"""Automated VR calibration against a running X4 (FreeTrack + presenter build).

Drives synthetic head poses through reports/captures/stereo.txt, then measures the
game's actual camera (read-only process reads of the selected camera record) and the
eye textures the Vulkan layer submits (dump.txt trigger). Per-frame alternating
deltas are analysed with second differences, which cancel linear ship drift.
"""
import argparse
import json
import math
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from peek_camera import Remote, resolve  # noqa: E402

CAPTURES = Path(__file__).resolve().parents[1]/'reports'/'captures'
BASE = dict(stereo=1, delay=2, recenter=0, ipd_scale=1, pos_scale=3.6, yaw_gain=2.1177,
            pitch_gain=2.1177, roll_gain=3.1416, predict=0.035, game_tan_y=0.8675)


def write_settings(**overrides):
    values = dict(BASE, **overrides)
    text = ''.join(f'{k}={v}\n' for k, v in values.items())
    (CAPTURES/'stereo.txt').write_text(text)


def synth(base=(0, 0, 0, 0, 0, 0), alt=(0, 0, 0, 0, 0, 0), rate=0, **extra):
    write_settings(synth=1, synth_base=' '.join(map(str, base)), synth_alt=' '.join(map(str, alt)),
                   synth_rate=rate, **extra)


class Camera:
    def __init__(self, pid):
        self.remote = Remote(pid)
        _, self.base = self.remote.verify()

    def frames(self, seconds):
        """Distinct selected-camera poses (world_from_camera, 4x4 row-major)."""
        out, last, end = [], None, time.time()+seconds
        while time.time() < end:
            try:
                address, _ = resolve(self.remote.read, self.base, (0x6D1B920, 0))
                raw = self.remote.read(address, 64)
            except (OSError, ValueError):
                continue
            if raw != last:
                last = raw
                f = struct.unpack('<16f', raw)
                out.append(np.array([[f[c*4+r] for c in range(4)] for r in range(4)], dtype=float))
        return out


def rotvec(R):
    angle = math.acos(max(-1.0, min(1.0, (np.trace(R)-1)/2)))
    if angle < 1e-9:
        return np.zeros(3)
    axis = np.array([R[2, 1]-R[1, 2], R[0, 2]-R[2, 0], R[1, 0]-R[0, 1]])/(2*math.sin(angle))
    return axis*angle


def alternation(frames):
    """Full A/B separation (translation in camera axes + world, rotation vector in camera axes)."""
    trans_cam, trans_world, rots = [], [], []
    for k in range(1, len(frames)-1):
        a = frames[k][:3, 3]-(frames[k-1][:3, 3]+frames[k+1][:3, 3])/2  # = +-(B-A)
        R = frames[k][:3, :3]
        sign = 1 if k % 2 else -1
        trans_world.append(sign*a)
        trans_cam.append(sign*(R.T@a))
    for k in range(1, len(frames)):
        Q = frames[k-1][:3, :3].T@frames[k][:3, :3]
        rots.append((1 if k % 2 else -1)*rotvec(Q))
    med = lambda v: np.median(np.array(v), axis=0) if v else np.zeros(3)
    return dict(frames=len(frames), trans_cam=med(trans_cam), trans_world=med(trans_world),
                rot_cam_deg=np.degrees(med(rots)))


def fmt(v):
    return '(' + ', '.join(f'{x:+.4f}' for x in v) + ')'


def dump(timeout=20):
    info = CAPTURES/'eye-dump.txt'
    info.unlink(missing_ok=True)
    (CAPTURES/'dump.txt').write_text('')
    end = time.time()+timeout
    while not info.exists():
        if time.time() > end:
            raise TimeoutError('no eye dump (presenter inactive?)')
        time.sleep(0.05)
    time.sleep(0.2)
    lines = info.read_text().split('\n')
    w, h, _, ox, oy, sx, sy = lines[0].split()
    w, h, ox, oy = int(w), int(h), int(ox), int(oy)
    poses = [np.array([float(x) for x in line.split()]).reshape(3, 4) for line in lines[1:3]]
    images = [np.fromfile(CAPTURES/f'eye-{i}.raw', dtype=np.uint8).reshape(h, w, 4)[:, :, :3].astype(float).mean(2)
              for i in range(2)]
    return dict(images=images, poses=poses, offset=(ox, oy), span=(float(sx), float(sy)))


def yaw_of(pose):
    return math.degrees(math.atan2(pose[0, 2], pose[2, 2]))


def shift_x(a, b, rows, cols, search=260):
    """Horizontal shift s so that a[:, x+s] ~ b[:, x] (sub-pixel via parabola)."""
    r0, r1 = rows
    c0, c1 = cols
    B = b[r0:r1, c0:c1]
    B = B-B.mean()
    errors = []
    for s in range(-search, search+1):
        if c0+s < 0 or c1+s > a.shape[1]:
            errors.append(np.inf)
            continue
        A = a[r0:r1, c0+s:c1+s]
        errors.append(np.abs((A-A.mean())-B).mean())
    errors = np.array(errors)
    i = int(np.argmin(errors))
    if 0 < i < len(errors)-1 and np.isfinite(errors[i-1]) and np.isfinite(errors[i+1]):
        d = errors[i-1]-2*errors[i]+errors[i+1]
        frac = 0.5*(errors[i-1]-errors[i+1])/d if d else 0
    else:
        frac = 0
    return i-search+frac, float(errors[i])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--tests', default='translation,rotation,fov,latency')
    parser.add_argument('--settle', type=float, default=1.2)
    args = parser.parse_args()
    camera = Camera(args.pid)
    report = {}
    tests = args.tests.split(',')
    try:
        if 'translation' in tests:
            for name, base, alt in [('x', (0, 0, 0, 0, 0, 0), (0.05, 0, 0, 0, 0, 0)),
                                    ('y', (0, 0, 0, 0, 0, 0), (0, 0.05, 0, 0, 0, 0)),
                                    ('z', (0, 0, 0, 0, 0, 0), (0, 0, 0.05, 0, 0, 0)),
                                    ('x@yaw60', (0, 0, 0, 60, 0, 0), (0.05, 0, 0, 0, 0, 0)),
                                    ('x@pitch30', (0, 0, 0, 0, 30, 0), (0.05, 0, 0, 0, 0, 0))]:
                synth(base, alt); time.sleep(args.settle)
                r = alternation(camera.frames(1.5))
                report[f'translation_{name}'] = {k: (v.tolist() if hasattr(v, 'tolist') else v) for k, v in r.items()}
                print(f'translation {name:10s} frames {r["frames"]:3d} cam {fmt(r["trans_cam"])} '
                      f'|{np.linalg.norm(r["trans_cam"]):.4f}| world {fmt(r["trans_world"])} rot {fmt(r["rot_cam_deg"])}')
        if 'rotation' in tests:
            for name, base, alt in [('yaw', (0, 0, 0, 0, 0, 0), (0, 0, 0, 5, 0, 0)),
                                    ('pitch', (0, 0, 0, 0, 0, 0), (0, 0, 0, 0, 5, 0)),
                                    ('roll', (0, 0, 0, 0, 0, 0), (0, 0, 0, 0, 0, 5)),
                                    ('pitch@yaw60', (0, 0, 0, 60, 0, 0), (0, 0, 0, 0, 5, 0))]:
                synth(base, alt); time.sleep(args.settle)
                r = alternation(camera.frames(1.5))
                report[f'rotation_{name}'] = {k: (v.tolist() if hasattr(v, 'tolist') else v) for k, v in r.items()}
                print(f'rotation    {name:10s} frames {r["frames"]:3d} rot_cam_deg {fmt(r["rot_cam_deg"])} '
                      f'|{np.linalg.norm(r["rot_cam_deg"]):.3f}| trans_cam {fmt(r["trans_cam"])}')
        if 'fov' in tests:
            synth((0, 0, 0, 0, 0, 0), (0, 0, 0, 5, 0, 0)); time.sleep(args.settle)
            d = dump()
            left, right = d['images']
            ox, oy = d['offset']
            h, w = left.shape
            gw, gh = w-2*ox, h-2*oy
            f = gh/(2*BASE['game_tan_y'])  # expected pixels per unit tangent
            cy = oy+gh//2
            yaws = [yaw_of(p) for p in d['poses']]
            print(f'fov: recorded eye yaws {yaws[0]:+.2f} {yaws[1]:+.2f}; expected f {f:.1f} px/tan')
            rows = []
            for frac in (0.3, 0.4, 0.5, 0.6, 0.7):
                cx = int(ox+frac*gw)
                s, err = shift_x(left, right, (cy-80, cy+80), (cx-70, cx+70))
                t = (cx-(ox+gw/2))/f
                rows.append((frac, t, s, err))
            dtheta = math.radians(yaws[1]-yaws[0])
            for frac, t, s, err in rows:
                # right-image column x maps to left-image x+s. Rotation about vertical by dtheta:
                expected = f*(math.tan(math.atan(t)+dtheta)-t)
                print(f'  column {frac:.1f} tan {t:+.3f}: shift {s:+7.2f} px (err {err:.1f}); pinhole+recorded yaw predicts {expected:+7.2f}'
                      f' or {-expected:+7.2f}')
            report['fov'] = dict(yaws=yaws, rows=rows, expected_f=f)
        if 'latency' in tests:
            from star_fov import stars, Tree
            synth(pace=0); time.sleep(args.settle)
            ref = dump()
            ox, oy = ref['offset']
            h, w = ref['images'][0].shape
            gw, gh = w-2*ox, h-2*oy
            k = gh/(2*BASE['game_tan_y'])
            cx, cy = ox+gw/2, oy+gh/2
            ref_stars = stars(ref['images'][0], (oy+10, int(oy+gh*0.55)))

            def image_yaw(img, lo, hi):
                tree = Tree(stars(img, (oy+10, int(oy+gh*0.75)))[:, :2])
                d = np.stack([(ref_stars[:, 0]-cx)/k, (ref_stars[:, 1]-cy)/k, np.ones(len(ref_stars))], 1)
                best = (-1, 0.0, 0.0)
                for yaw in np.arange(lo, hi, 0.05):
                    th = math.radians(yaw)
                    x2 = math.cos(th)*d[:, 0]+math.sin(th)*d[:, 2]
                    z2 = -math.sin(th)*d[:, 0]+math.cos(th)*d[:, 2]
                    dist, _ = tree.query(np.stack([cx+k*x2/z2, cy+k*d[:, 1]/z2], 1))
                    good = dist < 2.5
                    score = (int(good.sum()), -float(np.median(dist[good])) if good.any() else -9, float(yaw))
                    best = max(best, score)
                return best[2], best[0]
            for rate in (0.15, -0.15):
                synth(rate=rate, pace=0); time.sleep(0.5)
                ramp = dump()
                synth(pace=0); time.sleep(0.6)
                for i in range(2):
                    recorded = yaw_of(ramp['poses'][i])
                    lo, hi = sorted((recorded-6, recorded+6))
                    actual, matched = image_yaw(ramp['images'][i], lo, hi)
                    print(f'latency rate {rate:+.2f}/frame eye {i}: recorded {recorded:+.2f} deg, image {actual:+.2f} deg '
                          f'({matched} stars) -> render lags recorded pose by {(recorded-actual)/rate:+.1f} presents')
                    report.setdefault('latency', []).append(dict(rate=rate, eye=i, recorded=recorded, image=actual, stars=matched))
    finally:
        write_settings()
        (CAPTURES/'vr-calibration.json').write_text(json.dumps(report, indent=1, default=float))


if __name__ == '__main__':
    main()
