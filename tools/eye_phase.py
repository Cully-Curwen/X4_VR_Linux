"""Which eye offset X4 actually rendered each frame with, against the eye the layer showed it to.
Feeds X4 a fixed synthetic head pose whose eye offset alternates by SEPARATION metres (the
headset view freezes: keep eyes closed), then requests the frame probe and trace.txt.
Reports wrong-eye frames and, per main-camera bind, which pose read (L/R) it used and on which
thread, to show where X4 picks up the tracker value.
usage: eye_phase.py [--unpaced] [--no-request]"""
import sys, time
from pathlib import Path
import numpy as np

CAPTURES = Path(__file__).resolve().parents[1]/'reports'/'captures'
FRAME = 64*8*8*4
SEPARATION = 0.05  # metres each side

def capture():
    path = CAPTURES/'stereo.txt'; original = path.read_text()
    try:
        path.write_text(original.rstrip('\n')+'\n'+('pace=0\n' if '--unpaced' in sys.argv else '') +
                        f'synth=1\nsynth_base=0 0 0 0 0 0\nsynth_alt={SEPARATION} 0 0 0 0 0\n')
        time.sleep(8)
        marks = {n: (CAPTURES/n).stat().st_mtime if (CAPTURES/n).exists() else 0 for n in ('frames.txt', 'trace.txt')}
        for r in ('frames.request', 'trace.request'): (CAPTURES/r).write_text('1')
        while any(not (CAPTURES/n).exists() or (CAPTURES/n).stat().st_mtime == m for n, m in marks.items()): time.sleep(0.2)
        time.sleep(0.5)
    finally:
        path.write_text(original)

def two_groups(values):  # split along the principal axis; returns 0/1 per row and the gap
    v = np.asarray(values, float); centred = v-v.mean(0)
    axis = np.linalg.svd(centred, full_matrices=False)[2][0]
    t = centred @ axis
    order = np.sort(t); cut = int(np.argmax(np.diff(order)))
    threshold = (order[cut]+order[cut+1])/2
    return (t > threshold).astype(int), order[cut+1]-order[cut], np.ptp(t)

def main():
    if '--no-request' not in sys.argv: capture()
    rows = [l.split() for l in (CAPTURES/'trace.txt').read_text().splitlines()]
    ev = [(k, int(v), int(h), float(t), int(th), float(a), float(b), float(c)) for k, v, h, t, th, a, b, c in rows]
    t0 = ev[0][3]
    print('threads per kind:', {k: sorted({e[4] for e in ev if e[0] == k}) for k in 'PLRClr'})
    uses = [e for e in ev if e[0] in 'lr']  # eye-at-use hook: X4's camera read the tracker position
    cams = [e for e in ev if e[0] == 'C']
    reads = [e for e in ev if e[0] in 'LR']
    presents = [e for e in ev if e[0] == 'P']
    if not cams: sys.exit('no camera binds traced (memory tracker off?)')
    group, gap, spread = two_groups([e[5:8] for e in cams])
    print(f'{len(presents)} presents, {len(reads)} pose reads, {len(cams)} camera binds; camera positions split with gap {gap:.4f} of spread {spread:.4f}')
    # Match each camera group to an eye: the group that mostly follows L reads is left.
    read_times = np.array([e[3] for e in reads]); read_eye = np.array([e[0] == 'R' for e in reads], int)
    last_read = np.searchsorted(read_times, [e[3] for e in cams])-1
    valid = last_read >= 0
    eye_of_last = read_eye[last_read[valid]]
    if np.mean(group[valid] == eye_of_last) < 0.5: group = 1-group
    print(f'camera eye == eye of the latest pose read before the bind: {np.mean(group[valid] == eye_of_last)*100:.1f}%')
    prev_read = np.clip(last_read-1, 0, None)
    print(f'camera eye == eye of the read before that:              {np.mean(group[valid] == read_eye[prev_read[valid]])*100:.1f}%')
    half = np.array([e[2] for e in cams])
    for h in (0, 1): print(f'  binds while frame half {h}: camera eye 1 in {np.mean(group[half == h])*100:.1f}%')
    # per frame: the binds between two presents and the pose reads in that interval
    p_times = [e[3] for e in presents]
    mixed = 0; lines = []
    for i in range(1, len(presents)):
        inside = [j for j, e in enumerate(cams) if p_times[i-1] < e[3] <= p_times[i]]
        r_inside = [e for e in reads if p_times[i-1] < e[3] <= p_times[i]]
        u_inside = [e for e in uses if p_times[i-1] < e[3] <= p_times[i]]
        eyes = {int(group[j]) for j in inside}
        mixed += len(eyes) > 1
        if len(lines) < 40:
            lines.append(f'  present {presents[i][1]} half {presents[i][2]}: reads ' + ','.join(f'{e[0]}@{(e[3]-p_times[i-1])/1000:.1f}' for e in r_inside) +
                         ' uses ' + ','.join(f'{e[0]}@{(e[3]-p_times[i-1])/1000:.1f}' for e in u_inside) +
                         ' | cams ' + ''.join(str(int(group[j])) for j in inside) + (f' first@{(cams[inside[0]][3]-p_times[i-1])/1000:.1f}' if inside else ''))
    print(f'frames whose binds mix both eyes: {mixed}')
    print('\n'.join(lines))
    meta = np.loadtxt(CAPTURES/'frames.txt', ndmin=2)
    px = np.frombuffer((CAPTURES/'frames.raw').read_bytes(), np.uint8)[:len(meta)*FRAME].reshape(len(meta), 4096, 4)[:, :, :3].astype(np.float32)
    keep = meta[:, 2] >= t0; meta, px = meta[keep], px[keep]
    if len(px) > 4:
        rendered, gap, spread = two_groups(px.reshape(len(px), -1))
        label = meta[:, 1].astype(int)
        if np.mean(rendered == label) < 0.5: rendered = 1-rendered
        print(f'images: {len(px)} frames, split gap {gap:.1f} of spread {spread:.1f}; wrong-eye frames {np.mean(rendered != label)*100:.1f}%')
        print('  label   ', ''.join(map(str, label[:100])))
        print('  rendered', ''.join(map(str, rendered[:100])))
    caller = CAPTURES/'ftgetdata_caller.txt'
    if caller.exists(): print(caller.read_text().strip())

if __name__ == '__main__':
    main()
