"""Image stability of the frames X4 presents, from the layer's frame probe (an 8x8 grid of 8x8
patches per frame, last 1024 frames). With head and ship still, an eye's image should barely
change between its frames; temporal upscaling fed by alternating eyes shows as change.
usage: frame_stability.py [label] [--no-request]"""
import sys, time
from pathlib import Path
import numpy as np

CAPTURES = Path(__file__).resolve().parents[1]/'reports'/'captures'
FRAME = 64*8*8*4

def request():
    out = CAPTURES/'frames.txt'
    before = out.stat().st_mtime if out.exists() else 0
    (CAPTURES/'frames.request').write_text('1')
    for _ in range(50):
        time.sleep(0.2)
        if out.exists() and out.stat().st_mtime > before: time.sleep(0.5); return
    sys.exit('no frames.txt: is X4 running and presenting?')

def main():
    if '--no-request' not in sys.argv: request()
    meta = np.loadtxt(CAPTURES/'frames.txt', ndmin=2)
    raw = np.frombuffer((CAPTURES/'frames.raw').read_bytes(), np.uint8)[:len(meta)*FRAME]
    pixels = raw.reshape(len(meta), 64*8*8, 4)[:, :, :3].astype(np.int16)  # BGR
    eyes = meta[:, 1].astype(int)
    print(f'{len(meta)} frames over {(meta[-1, 2]-meta[0, 2])/1e6:.1f} s; eye alternation breaks: {int(np.sum(eyes[1:] == eyes[:-1]))}')
    for eye in (0, 1):
        frames = pixels[eyes == eye]
        change = np.abs(np.diff(frames, axis=0)).mean(axis=(1, 2))  # mean |delta| per consecutive same-eye pair, 0-255
        print(f'eye {eye}: frame-to-frame change p50 {np.percentile(change, 50):.2f} p90 {np.percentile(change, 90):.2f} '
              f'max {change.max():.2f}; pairs over 4: {np.mean(change > 4)*100:.0f}%')
    left, right = pixels[eyes == 0], pixels[eyes == 1]
    n = min(len(left), len(right))
    print(f'left vs right (parallax plus noise): p50 {np.percentile(np.abs(left[:n]-right[:n]).mean(axis=(1, 2)), 50):.2f}')

if __name__ == '__main__':
    main()
