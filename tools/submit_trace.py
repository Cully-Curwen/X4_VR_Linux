"""Per-frame timing of the submission thread (STUTTER_RESEARCH.md, verification plan).
Requests submit_trace.txt (the layer's last 2048 compositor frames, ~23 s) and trace.txt (X4's
presents) from the running game, then prints where each frame's time went and what the missed
frames had in common.
usage: submit_trace.py [--no-request]"""
import sys, time
from pathlib import Path

CAPTURES = Path(__file__).resolve().parents[1]/'reports'/'captures'
LATE_US = 13500  # more than ~1.2 headset frames between WaitGetPoses returns: a missed compositor frame
LATCH_US = 3200  # SteamVR on the Aero latches ~3.2-4.5 ms after WaitGetPoses returns

def request():
    out = CAPTURES/'submit_trace.txt'
    before = out.stat().st_mtime if out.exists() else 0
    for name in ('submit.request', 'trace.request'): (CAPTURES/name).write_text('1')
    for _ in range(50):
        time.sleep(0.2)
        if out.exists() and out.stat().st_mtime > before: time.sleep(0.5); return
    sys.exit('no submit_trace.txt: is X4 running with asynchronous submission?')

def pct(values, q):
    v = sorted(values)
    return v[min(len(v)-1, int(q*len(v)))] if v else 0

def main():
    if '--no-request' not in sys.argv: request()
    rows = []
    for line in (CAPTURES/'submit_trace.txt').read_text().splitlines():
        if line.startswith('#'): continue
        mine, steamvr = line.split(' | ')
        c, r, lk, rd, sl, sr, h, fresh, fb, resub, theater, late = map(int, mine.split())
        if sl == 0: continue  # skipped frame (not ready, resize)
        rows.append(dict(called=c, returned=r, lock=lk-r, copies=rd-lk, pre=rd-r, left=sl-rd, right=sr-sl, handoff=h-sr,
                         fresh=fresh, fallbacks=fb, resubmit=resub, theater=theater, x4_late=late, steamvr=steamvr.split()))
    for a, b in zip(rows, rows[1:]): b['interval'] = b['returned']-a['returned']; b['blocked'] = b['returned']-b['called']
    rows = rows[1:]
    if not rows: sys.exit('empty trace')
    seconds = (rows[-1]['returned']-rows[0]['returned'])/1e6
    missed = [i for i, r in enumerate(rows) if r['interval'] > LATE_US]
    print(f'{len(rows)} frames over {seconds:.1f} s; missed compositor frames: {len(missed)}; '
          f'fallbacks: {sum(r["fallbacks"] for r in rows)}; resubmits: {sum(r["resubmit"] for r in rows)}; '
          f'late game frames: {rows[-1]["x4_late"]-rows[0]["x4_late"]}')
    print('phase (ms)          p50    p90    p99    max')
    for key, label in (('blocked', 'WaitGetPoses'), ('lock', 'lock'), ('copies', 'copy fences'), ('pre', 'WaitGetPoses->Submit'),
                       ('left', 'Submit(left)'), ('right', 'Submit(right)'), ('handoff', 'PostPresentHandoff'), ('interval', 'interval')):
        v = [r[key]/1000 for r in rows]
        print(f'{label:20}{pct(v, .5):6.2f} {pct(v, .9):6.2f} {pct(v, .99):6.2f} {max(v):6.2f}')
    # A miss shows as a long interval ending at frame i: frame i-1's Submit came too late, or
    # the thread came back late from the runtime.
    over = sum(1 for r in rows if r['pre'] > LATCH_US)
    print(f'frames with WaitGetPoses->Submit over {LATCH_US/1000} ms: {over}; of them followed by a miss: '
          f'{sum(1 for i in missed if i > 0 and rows[i-1]["pre"] > LATCH_US)}')
    for i in missed[:12]:
        p, r = rows[i-1] if i else rows[i], rows[i]
        print(f'  miss +{(r["returned"]-rows[0]["returned"])/1e6:7.3f} s interval {r["interval"]/1000:5.1f} ms; before: '
              f'lock {p["lock"]/1000:.2f} copies {p["copies"]/1000:.2f} submit {(p["left"]+p["right"])/1000:.2f} '
              f'handoff {p["handoff"]/1000:.2f} fallbacks {p["fallbacks"]} resubmit {p["resubmit"]}')
    frames = {}
    for r in rows:  # SteamVR's timing of the frame before, deduplicated by frame index
        s = r['steamvr']
        if s[0] != '0': frames[s[0]] = s
    if frames:
        f = frames.values()
        print(f'SteamVR: {len(frames)} frames, presented more than once {sum(int(s[1]) > 1 for s in f)}, '
              f'mispresented {sum(int(s[2]) for s in f)}, dropped {sum(int(s[3]) for s in f)}, '
              f'reprojected {sum(int(s[4]) != 0 for s in f)}')

if __name__ == '__main__':
    main()
