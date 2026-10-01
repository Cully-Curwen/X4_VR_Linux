"""Timed in-headset A/B of presentation modes; segment n starts with n beeps.
Waits for X4 to be the foreground window, then runs each mode for SECONDS and prints the
layer's pair_stats.txt lines per segment. Restores stereo.txt afterwards.
usage: flicker_ab.py [seconds] [async|hitch|content|pose|stutter|cpu|late|still|eyeuse|delay]"""
import sys, time, ctypes as C, winsound
from ctypes import wintypes as W
from pathlib import Path

CAPTURES = Path(__file__).resolve().parents[1]/'reports'/'captures'
BASE = dict(pair=0, pair_wait=1, eye_from_half=1, half_xor_render=0, half_xor_present=1, submit_pose=1, async_submit=1, hitch_ms=0,
            handoff=0, release_late=1, eye_at_use=1)
MODE_SETS = {
    # the 2026-09-26 finding: inline submission shows every late game frame (grey flashes in
    # pair mode, ghosting on stalls); async submission hides them
    'async': [('pair, async submission', dict(pair=1)),
              ('pair, inline submission', dict(pair=1, async_submit=0)),
              ('pair, async submission', dict(pair=1)),
              ('alternate eyes, async submission', {})],
    'hitch': [('alternate eyes, inline, 35 ms hitch each second', dict(async_submit=0, hitch_ms=35)),
              ('alternate eyes, async, 35 ms hitch each second', dict(hitch_ms=35)),
              ('alternate eyes, inline, 35 ms hitch each second', dict(async_submit=0, hitch_ms=35)),
              ('alternate eyes, async, 35 ms hitch each second', dict(hitch_ms=35))],
    'content': [('alternate eyes, count-based eyes', dict(eye_from_half=0)),
                ('mono (stereo=0)', dict(stereo=0)),
                ('pair, rotation-only tracking', dict(pair=1, pos_scale=0)),
                ('pair', dict(pair=1))],
    'pose': [('pair, per-eye poses', dict(pair=1)),
             ('pair, no submitted pose', dict(pair=1, submit_pose=0)),
             ('alternate eyes, per-eye poses', {}),
             ('alternate eyes, no submitted pose', dict(submit_pose=0))],
    # STUTTER_RESEARCH.md items 3 and 4: compare the late/x4_late/waited_max columns
    'stutter': [('no handoff, late frames go on at once', dict(release_late=1)),
                ('PostPresentHandoff after Submit', dict(handoff=1, release_late=1)),
                ('late frames wait for the next tick', dict(release_late=0)),
                ('no handoff, late frames go on at once', dict(release_late=1))],
    # CPU-bound X4 (late-game saves): 10 ms busy on every present. With release_late=0 a frame
    # that ends after a tick waits for the next one, so X4 drops to 45 fps at 90 Hz.
    'cpu': [('CPU load, late frames wait for the next tick', dict(hitch_ms=10, hitch_every=1, release_late=0)),
            ('CPU load, late frames go on at once', dict(hitch_ms=10, hitch_every=1)),
            ('CPU load, late frames wait for the next tick', dict(hitch_ms=10, hitch_every=1, release_late=0)),
            ('CPU load, late frames go on at once', dict(hitch_ms=10, hitch_every=1))],
    # the same without added load (the 2026-09-28 verdict for release_late=0 was at ~75 fps)
    'late': [('late frames wait for the next tick', dict(release_late=0)), ('late frames go on at once', {}),
             ('late frames wait for the next tick', dict(release_late=0)), ('late frames go on at once', {})],
    # 2026-09-28: stutter with head and ship still; which part of the stereo path causes it
    'still': [('alternate eyes', {}),
              ('mono: both eyes the same image, every frame', dict(stereo=0)),
              ('alternate eyes, no pose prediction', dict(predict=0)),
              ('alternate eyes, no pose submitted with the images', dict(submit_pose=0))],
    # the fix for wrong-eye frames: eye offset added when X4's camera reads the tracker
    'eyeuse': [('eye chosen at use (fix)', {}), ('eye chosen at the tracker read (old)', dict(eye_at_use=0)),
               ('eye chosen at use (fix)', {}), ('eye chosen at the tracker read (old)', dict(eye_at_use=0))],
    # which recorded pose goes with a presented frame (turn the head while comparing)
    'delay': [('delay 2', dict(delay=2)), ('delay 1', dict(delay=1)), ('delay 2', dict(delay=2)), ('delay 1', dict(delay=1))]}

def x4_in_front():
    u, k = C.windll.user32, C.windll.kernel32
    pid = W.DWORD(); u.GetWindowThreadProcessId(u.GetForegroundWindow(), C.byref(pid))
    buf, size = C.create_unicode_buffer(260), W.DWORD(260)
    h = k.OpenProcess(0x1000, False, pid.value)
    ok = h and k.QueryFullProcessImageNameW(h, 0, buf, C.byref(size))
    if h: k.CloseHandle(h)
    return bool(ok) and buf.value.lower().endswith('x4.exe')

def write(settings, overrides):
    (CAPTURES/'stereo.txt').write_text(''.join(f'{k}={v}\n' for k, v in dict(settings, **overrides).items()))

def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20
    modes = MODE_SETS[sys.argv[2] if len(sys.argv) > 2 else 'async']
    path = CAPTURES/'stereo.txt'
    original = path.read_text()
    current = dict(l.split('=', 1) for l in original.splitlines() if '=' in l)
    tick = C.windll.kernel32.GetTickCount64; tick.restype = C.c_uint64
    while not x4_in_front(): time.sleep(0.2)
    marks = []
    try:
        for n, (name, overrides) in enumerate(modes, 1):
            write(current, dict(BASE, **overrides))
            for _ in range(n): winsound.Beep(1200, 120); time.sleep(0.12)
            marks.append((tick(), name)); time.sleep(seconds)
        marks.append((tick(), 'end'))
        for _ in range(3): winsound.Beep(600, 300)
    finally:
        path.write_text(original)
    time.sleep(2.5)
    stats = [l for l in (CAPTURES/'pair_stats.txt').read_text().splitlines() if l and l[0].isdigit()]
    for n, ((start, name), (end, _)) in enumerate(zip(marks, marks[1:]), 1):
        print(f'== segment {n}: {name}')
        fps = []
        for i, line in enumerate(stats):
            c = line.split()
            if i and start+2500 <= int(c[0]) <= end+500:
                print(line)  # X4 fps = new eye images (one per game frame) over the window
                fps.append((2*int(c[1])-int(c[2])-int(c[3]))*1000/(int(c[0])-int(stats[i-1].split()[0])))
        if fps: print(f'   X4 fps median {sorted(fps)[len(fps)//2]:.1f}')

if __name__ == '__main__':
    main()
