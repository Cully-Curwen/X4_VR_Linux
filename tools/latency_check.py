"""Star-ramp latency check under a chosen presentation mode (e.g. pair=1)."""
import math, sys, time, ctypes as C
from ctypes import wintypes as W
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
import vr_calibrate as v
from star_fov import stars, Tree

def main():
    pid = int(sys.argv[1]); extra = dict(kv.split('=') for kv in sys.argv[2:])
    u = C.windll.user32
    def fg():
        p = W.DWORD(); u.GetWindowThreadProcessId(u.GetForegroundWindow(), C.byref(p)); return p.value == pid
    while not fg(): time.sleep(0.2)
    v.synth(**extra); time.sleep(1.5)
    ref = v.dump()
    ox, oy = ref['offset']; h, w = ref['images'][0].shape; gw, gh = w-2*ox, h-2*oy
    k = gh/(2*v.BASE['game_tan_y']); cx, cy = ox+gw/2, oy+gh/2
    rs = stars(ref['images'][0], (oy+10, int(oy+gh*0.55)))
    def image_yaw(img, lo, hi):
        tree = Tree(stars(img, (oy+10, int(oy+gh*0.75)))[:, :2])
        d = np.stack([(rs[:, 0]-cx)/k, (rs[:, 1]-cy)/k, np.ones(len(rs))], 1); best = (-1, 0, 0.0)
        for yaw in np.arange(lo, hi, 0.05):
            th = math.radians(yaw); x2 = math.cos(th)*d[:, 0]+math.sin(th)*d[:, 2]; z2 = -math.sin(th)*d[:, 0]+math.cos(th)*d[:, 2]
            dist, _ = tree.query(np.stack([cx+k*x2/z2, cy+k*d[:, 1]/z2], 1)); g = dist < 2.5
            best = max(best, (int(g.sum()), -float(np.median(dist[g])) if g.any() else -9, float(yaw)))
        return best[2], best[0]
    for trial in range(3):
        rate = 0.15
        v.synth(rate=rate, **extra); time.sleep(0.6)
        ramp = v.dump()
        for i in range(2):
            rec = v.yaw_of(ramp['poses'][i]); act, n = image_yaw(ramp['images'][i], rec-8, rec+8)
            print(f'trial {trial} eye {i}: recorded {rec:+.2f} image {act:+.2f} ({n} stars) lag {(rec-act)/rate:+.1f} presents')
        v.synth(**extra); time.sleep(0.8)
    v.write_settings()

if __name__ == '__main__':
    main()
