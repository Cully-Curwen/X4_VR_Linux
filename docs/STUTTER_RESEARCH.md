# Frame stutter: research and fix plan

## Implementation status (updated 2026-09-29)

Done, built and unit-tested; the first headset check follows the list:

- Item 1: no unbounded wait between `WaitGetPoses` and `Submit`. A fence per submission (4)
  replaces the `read` wait; the images of unfinished submissions stay held. `p.mutex` is a
  `timed_mutex` taken with the submit budget; on timeout the last frame is submitted again
  (`resubmit`). The present hook releases it before the driver's present (async mode).
  `turn_stats`, `pair_stats`, `stereo.txt` reload and the request files moved to a background
  thread (`write_file_later`, `take_request` in `runtime_bootstrap.cpp`). The OpenVR cursor
  overlay is updated after `Submit`. Not done: `presenter_copy` still waits on fences under the
  lock (the timed lock covers it), and the eye-fence wait stays unbounded when no fallback image
  exists (only before the first submission after start or a resize).
- Item 2: `THREAD_PRIORITY_TIME_CRITICAL` (no MMCSS).
- Item 3: `PostPresentHandoff` off by default (live key `handoff=1` restores it); `Submit(left)`,
  `Submit(right)` and the handoff are timed separately, and `GetFrameTiming` is logged per frame,
  in the per-frame trace (`submit.request`, `tools/submit_trace.py`).
- Item 4: late game frames go on at once (`release_late=1`, `x4_late` column).
- Item 5, partly: shader modules, pipelines and descriptor layouts are logged only with
  `observe.ps1 -Shaders`; in play only pipeline creations over 2 ms (`slow_pipelines`). `log()`
  and `head.txt`/`state.txt` go through the background thread. The `MemoryTracker` is unchanged.

First headset check (2026-09-28, OpenVR, cockpit, `flicker_ab.py 20 stutter` + `submit_trace.py`):

- 60-80 fps windows: 1 missed compositor frame in 18 windows (0.06 per window, was 1.74).
  About 90 fps: 1 in 9 (0.11, was 0.09). `waited_max` 2.3-3.0 ms per window, `resubmit` 0.
- Per-frame trace (25 s): `WaitGetPoses` to `Submit` p99 2.17 ms, max 2.81 ms, none over the
  3.2 ms latch; each `Submit` ~0.05 ms; lock wait max 0.05 ms.
- Open question answered: with `PostPresentHandoff` the frame-sync block sits in
  Submit/handoff (`submit_max` 11-21 ms, `WaitGetPoses` ~0.3 ms) and misses rose to 1.4 per
  window. Without it the block moves into `WaitGetPoses` (~11 ms) and `Submit` returns at once.
  `handoff=0` stays the default.
- `release_late` made no measurable difference here: X4 was GPU-bound (~75 fps either way), and
  the GPU queue absorbs the wait. It should matter in CPU-bound scenes; not yet tested there.
- CPU-bound check (2026-10-01, `flicker_ab.py 20 cpu`: 10 ms busy-wait on every present, 1440p,
  cockpit, ship still): `release_late=0` held X4 at exactly 45.0 fps, `release_late=1` gave
  67 fps, on OpenVR and on OpenXR (Varjo). `late` stayed 0 in both; with 1, 27-43 fallbacks per
  2 s window and `waited_max` up to 3.0 ms. The user saw 1 as smoother. Players report late-game
  saves with big fleets as CPU-bound, so `release_late=1` is now the default.
- The same without added load (`flicker_ab.py 20 late`, OpenXR, ~90 fps): 88.3-88.8 vs
  89.8-90.0 fps, similar fallbacks, 1 missed compositor frame in each mode. The user's favourites
  were one segment of each mode, with minimal difference, so the 2026-09-28 preference for
  waiting didn't reproduce.
- Minimizing X4 (fullscreen at a DSR resolution) stalled the submission thread for 2.4 s in
  one of the post-submit steps (likely the GPU-side mode switch); X4 stops presenting while
  minimized. Not stutter during play.

The user still saw "extreme image stutter" in every segment, with head and ship still, and the
same as before the changes. Items 1-5 fixed missed compositor frames, which were not what they saw.

### Wrong-eye frames (2026-09-28, the stutter with a still head)

- Mono (`stereo=0`) was smooth; alternate eyes stuttered even at a clean 90 fps with 0 missed
  compositor frames. Pose prediction, the submitted pose, DLSS and `release_late` made no difference.
- `tools/eye_phase.py` feeds a fixed synthetic head with the eyes 10 cm apart and classifies each
  presented frame's image (frame probe) and camera (main-camera binds): 2-5% of frames at 84-90 fps
  paced (17-38% unpaced, 0% at 10 fps) were rendered with the other eye's offset, so that eye's
  image jumped sideways by the eye separation. The layer's labels (frame half at present) were right.
- Cause: X4 reads the tracker (`FTGetData`, main thread) and its camera bridge (`0x9fd870`) reads
  the tracker's position through vtable slot 0x108 (`0xf377b0`) on **another thread**, 0.1-2 ms
  after each present. Which read the bridge picks up varies, so the eye chosen at the read (frame
  half then) was sometimes for the wrong frame.
- Fix (uncommitted, `freetrack_client.cpp`, `EyeOffsets`): `FTGetData` sends the head centre plus
  per-eye position offsets; a vtable hook on slot 0x108 adds the offset for the eye X4 builds at
  that moment (frame half ^ `half_xor_use`) and records the reprojection pose there. Slot 0x28
  (the "tracker still for 30 reads" check, which makes the bridge skip the pose) returns false
  while offsets are added. Signature-checked; on foot and in theater mode the old path remains.
  `half_xor_use=1` (0 swapped the eyes: near objects doubled). Live key `eye_at_use=0` = old path.
  Result: 0% wrong-eye frames paced and unpaced; with a still head the user saw no stutter at all.
- Jitter while the head moved ("different from before"; `delay` 1 vs 2 made no difference): the
  pose ring kept one record per tag, and with long frames the camera thread recorded the next
  frame's pose in the same tag, so a frame fell back to a pose two frames old. Fix: 32 records in
  arrival order (`runtime_bootstrap.cpp`). In the headset the jitter was "way better", with one
  short stutter in the session.
- `tools/head_sync.py` (camera rotation per frame from `F` trace events vs the submitted pose from
  `S` events) after the fix: lag 0, correlation 0.992, camera/pose rotation ratio 1.000,
  per-frame residual 0.002° median (0.008° p99).
- SteamVR and the Varjo driver use the per-eye poses: live `submit_pose=0` (no pose, so SteamVR's
  own pose for both eyes) gave "much worse, extreme ghosting" while turning the head. The Varjo
  driver implements `IVRDriverDirectModeComponent_007`, whose `SubmitLayerPerEye_t` carries
  `mHmdPose` per eye.
- A longer play session after that showed no issues (checkpoint commit 7c82e84).

### On foot (2026-09-28)

- `head_sync.py` now also checks each frame's eye side, which is absolute: the camera minus the
  mean of its neighbours lies along the camera x axis (`X` trace events), to the right for the
  right eye. It catches single wrong-eye frames and whole-view swaps, which `eye_phase.py`'s
  majority mapping misses. Cockpit reference: 100%.
- The old on-foot path (eye chosen at `FTGetData`) put 94% of frames on the correct side. The
  wrong 6% came in bursts.
- The camera bridge reads the tracker on foot too (slot 0x108), but right *after* a present,
  and the frame then being built uses it. In the cockpit the read comes right *before* a present
  and is used two frames later. So on foot the eye at use is half ^ 0 (`half_xor_walk`; ^1
  swapped every frame, 0% correct side), and the eye side is 100%.
- The reprojection pose was still two frames old on foot: `presented_frame` only takes poses
  recorded at least `delay-1` presents earlier, which skipped the pose recorded right after the
  present. User: "extremely stutter" when moving the head on foot, fine when turning with the
  mouse; `head_sync.py` best lag +2. `delay=1` for all fixed on foot but broke the cockpit (there
  it took the next same-eye frame's pose: lag -2), so on-foot poses use `delay_walk` (default 1).
- Result (defaults `walk_at_use=1`, `half_xor_walk=0`, `delay_walk=1`), turning fast (2.1°/frame
  median): cockpit and on foot both lag 0, correlation 1.000, residual 0.003°, eye side 100%.
  User: cockpit and on foot smooth (final build).

### One pacer (item 7, 2026-09-29)

- The layer paces X4 in every mode (`pace_to_compositor`, pair mode's `wait_mid_frame`, inline
  `WaitGetPoses`), so X4's frame limiter is a second clock.
- Headset A/B, alternate eyes, cockpit, ship still, 22 s per run (`submit_trace.py`, X4
  presents from `trace.txt`). At 1440p, where X4 holds ~88 fps and the limiter engages:
  cap 90 vs off gave 87.8 vs 87.8 fps, present p50/p90/p99 11.20/12.44/15.16 vs
  11.22/12.52/14.83 ms, 0 vs 0 missed compositor frames, fallbacks 154 vs 150. At 4K
  (GPU-bound, ~73 fps) off vs cap 90 also matched. The two clocks don't beat measurably.
- So the limiter only matters in pair mode, where any cap under 180 fps defeats it. The launcher
  check now requires it off (it accepted a cap of 90 fps or more), the one value right for both
  modes; *Fix X4 settings* sets `frameratelimit=false`. README: limiter off, NVIDIA Max Frame
  Rate off.
- Reflex: none of the 85 logged `device_created` events (`process-*/events.jsonl`) enables
  `VK_NV_low_latency2`, which Reflex needs on Vulkan, so its sleep isn't pacing X4 in those runs.
  The logs don't record whether DLSS was on.
- Seen in passing: at 4K the frame rate fell from ~89 to ~73 fps mid-session with no settings
  change (scene load), which showed as 2x the eye-image fallbacks.

### Shelved and next (2026-09-29)

- Item 11 is shelved. With the eye picked at use, alternate eyes plus the optional pair mode
  are good enough, no player has reported rendering problems, and engine hooks would need
  rework with every X4 update. Revisit only if players report object doubling or depth errors.
- The player-side settings are in the README (*Smoother frames*).
- The mouse cursor bug is fixed (67ca5c6, STATUS.md).

Next: items 6, 8-10 and 12.

## Research (2026-09-28)

Research notes only: no code or settings were changed. Scope is the default path
(asynchronous OpenVR submission through SteamVR on the Varjo Aero with an RTX 3090), with notes
on the OpenXR backend where it matters. Line numbers refer to the working tree at `d0b3bce`.

## Summary

Stutter in the headset has four separate sources, and each needs a different fix.

| | What you see | Cause | Where the fix lives |
|---|---|---|---|
| A | Both eyes freeze for a frame (a hitch, or a flash) | The submission thread calls `Submit` after SteamVR has latched the frame | The layer: `compositor_loop` and the present hook |
| B | One eye's update slips and world motion judders | X4 needs more than 11.1 ms for a frame (CPU or GPU) | Pacing policy (`pace_to_compositor`), GPU headroom |
| C | Fast-moving objects double or shimmer in depth, even at a steady 90 fps | Alternate-eye rendering: each eye runs at 45 Hz and the eyes are 11 ms apart | Depth for the compositor, synchronized eyes, native stereo |
| D | Occasional single long frames | X4 creating pipelines, autosaves and loading, plus the layer's own disk I/O | The layer (no capture in play mode), driver and game settings |

The two changes most likely to help are both in the layer:

1. Nothing between `WaitGetPoses` and `Submit` should wait without a deadline. Today three
   things can: a fence, a GPU copy, and `p.mutex`, which the present hook holds across the
   driver's own present. In the logs, 2-second windows with a pre-submit wait over 3.2 ms have
   8 times the rate of missed compositor frames.
2. `pace_to_compositor` should return immediately when X4 is already late, as `WaitGetPoses`
   does. Today a frame that finishes 0.1 ms after a tick waits for the next tick, so X4 frames
   of 11.2 ms turn into 22.2 ms (45 fps).

## How a frame reaches the headset (async OpenVR path)

1. X4 samples the head pose through `FTGetData` once per frame, about 1.35 ms before its present
   (`trace.txt`), then calls `vkQueuePresentKHR`.
2. The present hook (`observe_layer.cpp:1368`) takes `p.mutex`, copies the swapchain image into
   the eye's ring slot (`presenter_copy`, line 749), forwards the present to the driver, releases
   the lock, and then blocks X4 in `pace_to_compositor` (line 1306) until the next compositor tick.
3. The submission thread (`compositor_loop`, line 1140) repeats:
   `WaitGetPoses` → publish the tick, which releases X4 (line 1165) → wait for OpenVR's copies of
   the previous submission (`read` fence, line 1171) → take `p.mutex` and pick each eye's newest
   and fallback image (line 1186) → wait up to `submit_budget_ms` (2 ms) for the newest copy
   (line 1220) → turn compensation and `turn_stats` (line 1240) → `Submit` for both eyes with
   `Submit_TextureWithPose`, then `PostPresentHandoff` (`runtime_bootstrap.cpp:143–161`) →
   `pair_stats`.
4. SteamVR latches the frame about 3.2–4.5 ms after `WaitGetPoses` returns (measured
   2026-09-28, `runtime_bootstrap.hpp:87`). A later `Submit` makes both eyes repeat the previous
   frame.

## Measurements

### Submission statistics (`reports/captures/pair_stats.txt`)

Data: the 21 runs logged with the current column layout, i.e. since asynchronous submission
(2026-09-25 to 2026-09-27). Only 2-second windows with async submission, alternate-eye mode
(`pair=0`) and new images for both eyes count, so theater mode is excluded. That leaves 1,307
windows, about 43.6 minutes.

- X4's frame rate is the number of new eye images per second.
- A missed compositor frame is the `late` column: more than 13.5 ms between two `WaitGetPoses`
  returns.
- A fallback means the newest image's copy wasn't finished within the 2 ms budget, so the
  previous image was submitted.
- The wait before `Submit` is the `waited_max` column: the longest time in the window between
  `WaitGetPoses` returning and `Submit`. It includes the `read` fence, `p.mutex`, the eye fences
  and `turn_stats`.

| X4 frame rate | Windows | Share | Missed compositor frames per window | Fallbacks per window | Wait before `Submit` (median of window maxima) | p90 |
|---|---|---|---|---|---|---|
| ≥ 88 fps | 877 | 67% | 0.09 | 1.3 | 0.24 ms | 5.10 ms |
| 80–88 fps | 115 | 9% | 0.64 | 39.0 | 3.56 ms | 9.34 ms |
| 60–80 fps | 133 | 10% | 1.74 | 17.1 | 3.87 ms | 6.95 ms |
| < 60 fps | 182 | 14% | 0.95 | 5.2 | 0.98 ms | 4.94 ms |

561 of 235,278 submissions (0.24%) missed a compositor frame, and 165 windows (13%) contain at
least one miss. Windows whose longest pre-submit wait passed the latch have far more misses,
and more the longer the wait:

| Longest wait before `Submit` | Windows | Windows with a miss | Misses per window |
|---|---|---|---|
| ≤ 3.2 ms | 862 | 3.7% | 0.12 |
| > 3.2 ms | 445 | 30% | 1.02 |
| > 4.5 ms | 247 | 37% | 1.45 |
| > 6.0 ms | 102 | 43% | 2.55 |

These are per-window aggregates (maxima over 2 s), so they show a correlation, not a per-frame
cause. The per-frame logging under [Verification plan](#verification-plan) would settle it.

Two more observations from the same data:

- `WaitGetPoses` blocks for 0.24 ms on average (median window), while the `Submit` +
  `PostPresentHandoff` call reaches at least 10.9 ms in 90% of windows (median of the window
  maxima 11.42 ms). The thread's frame-sync wait therefore happens inside `Submit` or
  `PostPresentHandoff`, not in `WaitGetPoses`. openvr issue #1401 reports the same pattern
  (there with the Oculus driver).
- The worst miss rates come in GPU-bound phases (80–88 fps: 39 fallbacks per window). That is
  also when the driver's present is most likely to block while the hook holds `p.mutex`.

### Pipeline creation during play (`reports/captures/process-*/events.jsonl`)

The layer logs every pipeline and shader module X4 creates. X4 keeps creating them long after
loading:

| Run | Last present logged | Graphics pipelines after present 6000 | Shader modules after present 6000 |
|---|---|---|---|
| process-35400 | 16,200 | 619 | 212 |
| process-39040 | 78,300 | 557 | 228 |
| process-48184 | 14,700 | 189 | 56 |

Present 6000 is about 67 s at 90 fps; presents are logged every 300. Each creation can stall
X4's thread while the driver compiles, unless NVIDIA's shader cache already has it. In normal
play the layer adds a disk write per shader module and a flushed log line per pipeline.

### Pose sampling (`reports/captures/trace.txt`, 2026-09-25 build)

In stereo mode X4 called `FTGetData` exactly once between consecutive presents (all 1,023
intervals in the trace), 1.35 ms (median; p10 1.25, p90 1.55) before its present.
Present-to-present intervals: median 11.12 ms, p90 11.39 ms, max 12.77 ms. The pose recorded for
`Submit_TextureWithPose` is therefore the pose X4 rendered with; the "last call wins" rule in
`record_render_pose` doesn't cost anything.

## Findings in the code

The critical path runs from `WaitGetPoses` returning to `Submit`. Anything on it that waits
without a deadline can push the submission past the latch.

1. `observe_layer.cpp:1171`: `WaitForFences(read, UINT64_MAX)` right after `WaitGetPoses`. It
   waits for OpenVR's copies of the previous submission, which queue behind X4's GPU work on a
   normal-priority queue.
2. `observe_layer.cpp:1220`: the eye-fence wait is `UINT64_MAX` when no older finished image
   exists, which is exactly when the GPU is furthest behind.
3. `observe_layer.cpp:1371–1384`: the present hook holds `p.mutex` across `presenter_copy`,
   which itself waits on fences with `UINT64_MAX`, and across the driver's `vkQueuePresentKHR`.
   The submission thread takes the same lock at line 1186, right after `WaitGetPoses`. When the
   driver's present blocks (drivers typically throttle frames in flight while the GPU is
   behind), the submission blocks with it.
4. File-system calls on hot paths:
   - `turn_stats` (line 1116, called at 1240) opens and rewrites `turn.txt` every 2 s, before
     `Submit`.
   - `stereo_settings()` (`runtime_bootstrap.cpp:233`) re-reads `stereo.txt` every 500 ms on
     whichever thread calls it first: the game thread in `FTGetData`, X4's render threads through
     `track_camera` (`observe_layer.cpp:206`), the present hook under `p.mutex` (line 755), or the
     submission thread (line 1153).
   - Every present calls `std::filesystem::remove` for `frames.request` and `dump.txt` under
     `p.mutex` (lines 774, 846), and `GetFileAttributesA` for `trace.request`
     (`runtime_bootstrap.cpp:333`).
   - `FTGetData` writes `head.txt` every 200 ms and appends to `state.txt` on X4's game thread
     (`freetrack_client.cpp:227, 237`).
5. `observe_layer.cpp:1165`: the tick is published right after `WaitGetPoses`, before `Submit`,
   so X4 starts its next frame (and its GPU work) before SteamVR's copy is queued.
6. `runtime_bootstrap.cpp:160`: `PostPresentHandoff` follows `Submit`. Per openvr.h it is only
   needed when `WaitGetPoses` can't follow the present immediately, and here it can.
7. Nothing sets the submission thread's priority: there is no `SetThreadPriority` or
   `AvSetMmThreadCharacteristics` anywhere in the project.
8. `observe_layer.cpp:1306–1311` (`pace_to_compositor`): after every present X4 waits for the
   next tick, even when the frame is already late. A frame that finishes just after a tick waits
   almost a whole frame.
9. `observe_layer.cpp:411`: the `MemoryTracker` is always created, because turn compensation
   needs the camera uniform. It takes one global mutex on every `vkUpdateDescriptorSets`,
   allocation, bind and map. `track_camera` (line 199) runs `ReadProcessMemory`,
   `stereo_settings()` and a second mutex on every camera-set bind. All of this happens on X4's
   command-recording threads.
10. `observe_layer.cpp:443–516`: in normal play the launcher sets `X4VR_CAPTURE_DIR`, so every
    shader module is written to disk (up to 4,096 modules or 128 MiB) and every pipeline
    creation appends a flushed JSON line (no cap).
11. `observe_layer.cpp:346–367`: the layer's queue is created in X4's graphics family with
    priority 1.0 and no global priority.
12. `observe_layer.cpp:948` (`wait_mid_frame`, pair mode): sleeps in `Sleep(1)` steps until
    1.5 ms before the target, then spins. Windows only guarantees the default timer resolution
    to processes that haven't called `timeBeginPeriod`, so each `Sleep(1)` can overshoot.

## Research findings

### SteamVR and OpenVR frame timing

- `WaitGetPoses` "will block until 'running start' milliseconds before the start of the frame"
  (openvr.h). In practice it "will sometimes return immediately if you are running behind"
  (openvr #1515; there, turning off Motion Smoothing and avoiding Windows sleep functions
  helped).
- `PostPresentHandoff` "is an optional call, which only needs to be used if you can't instead
  call WaitGetPoses immediately after Present", and it "should only be called from the same
  thread you are rendering on" (openvr.h). In openvr #1401 the frame-sync block happened inside
  `PostPresentHandoff`, with `WaitGetPoses` returning "almost immediately" afterwards. A commenter
  reports that removing the call made rendering smooth and that using it "makes rendering just
  stutter".
- Queue rules (OpenVR Vulkan wiki): `Submit`, `PostPresentHandoff`, `SubmitExplicitTimingData`
  and, conditionally, `WaitGetPoses` schedule work on `m_pQueue`, and no other thread may use
  that queue during those calls. The layer's private queue already satisfies this.
- Explicit timing (`SetExplicitTimingMode`, `SubmitExplicitTimingData`) gives SteamVR a more
  accurate GPU start time and, with `Explicit_ApplicationPerformsPostPresentHandoff`, keeps
  `WaitGetPoses` off the queue. It doesn't change when frames latch, so it isn't a stutter fix
  here.
- `GetFrameTiming` and `GetFrameTimings` report per frame: `m_nNumFramePresents`,
  `m_nNumMisPresented`, `m_nNumDroppedFrames`, `m_nReprojectionFlags`, `m_flNewFrameReadyMs`
  (the second `Submit`), `m_flCompositorRenderStartMs`, `m_flSubmitFrameMs`,
  `m_flCompositorIdleCpuMs` and `m_flTransferLatencyMs`. STATUS.md notes that SteamVR's
  cumulative counters missed the earlier flashes, probably because Varjo's compositor sits
  downstream, but per-frame timing is still the best view from the app's side.
- Submit flags in the pinned openvr.h: `Submit_TextureWithPose` (used today),
  `Submit_TextureWithDepth` (`VRTextureDepthInfo_t`: depth image, projection, range),
  `Submit_TextureWithMotion` (`VRTextureWithMotion_t`: a motion-vector image plus `mDeltaPose`,
  the "incremental application-applied transform ... since the previous frame"), and
  `Submit_FrameDiscontinuity`, which stops motion smoothing from extrapolating across a cut.
- Predicting the pose for display time (OpenVR wiki): `frameDuration - secondsSinceLastVsync +
  Prop_SecondsFromVsyncToPhotons_Float`. The mod predicts a constant 35 ms. Because
  `Submit_TextureWithPose` lets the compositor correct rotation, this mainly affects positional
  accuracy. Low priority.

### GPU and CPU scheduling

- NVIDIA's Windows driver supports `VK_EXT_global_priority`, but priorities above medium need
  `SeIncreaseBasePriorityPrivilege`, which only an administrator process can enable (NVIDIA
  developer forum, NVIDIA engineer's reply, 2022). A higher-priority layer queue is therefore
  not available to a normally launched X4.
- MMCSS (`AvSetMmThreadCharacteristics`) boosts registered threads by task category: High
  (priorities 23–26), which "is designed for Pro Audio tasks", or Medium (16–22) for the
  foreground application (Microsoft).
- `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` (Windows 10 1803 and later) is meant "for
  time-critical situations when short expiration delays on the order of a few milliseconds are
  unacceptable" (Microsoft). Since Windows 10 2004, `timeBeginPeriod` only affects the calling
  process, and on Windows 11 an occluded or minimized window loses the guarantee.
- Valve's Aaron Leiby, on unexplained CPU hiccups at 90 Hz: "I suspect this is core parking
  which is rearing its ugly head."
- Valve, *Advanced VR Rendering Performance* (GDC 2016): reprojection is "a last-resort safety
  net". Adaptive quality keeps GPU time between 70% and 90% of the frame and leaves 10% idle for
  other processes such as the compositor.

### Frame-rate policy

- Meta, *Asynchronous Timewarp Examined*: "ATW should run at a fixed fraction of the game frame
  rate. For example, at 90Hz refresh rate, we should either hit 90Hz or fall down to the
  half-rate of 45Hz with ATW. [...] Rendering at an intermediate rate, such as 65Hz, will result
  in a constantly changing number and position of the images on the retina, which is a worse
  artifact." Moving objects in timewarped frames "are effectively frozen in time", which shows
  as judder.

### How other alternate-eye mods handle it

- UEVR offers three methods. *Native Stereo* keeps temporal effects intact. *Synchronized
  Sequential* "renders two frames sequentially in a synchronized fashion on the same engine
  tick", in *Skip Draw* and *Skip Tick* variants; temporal AA ghosts and motion blur must be off.
  *Alternating/AFR* renders "with the game world advancing time in between frames" and "causes
  eye desyncs and usually nausea along with it".
- Luke Ross's R.E.A.L. mods use alternate-eye rendering and accept that "one of the two eyes will
  always have 11 ms of additional latency, which leads to some doubling of objects that are
  moving quickly relative to the camera". The runtime's reprojection covers head rotation, and
  the mods advise turning ASW and Motion Smoothing off.
- A delay between the eyes makes sideways-moving objects appear at the wrong depth (the Pulfrich
  effect, seen in VR as the "Zuckerfrich" effect of rolling displays). With AFR the late eye
  swaps every frame.

### Varjo

- The Aero runs at 90 Hz only; Varjo support had no plans for other rates (user report, December
  2023). The 75/90 Hz setting added in Varjo Base 4.3 is for the XR-4 series.
- "Submitting a depth buffer automatically enables 6-DOF (positional) timewarp in the
  compositor" (Varjo native SDK docs). The Unity plugin describes depth submission the same way.
- Varjo Base 3.2 (2021) "added adaptive synchronous client frame submission and updated
  positional timewarp to improve the visual experience in low-FPS scenarios".
- Varjo Base 3.8 (2022-12-21) introduced Motion smoothing and "new options in Vertical
  synchronization setting in the Headset tab: Automatic, Enabled – Fixed 45 fps, and Enabled –
  Fixed 30 fps". Motion smoothing left beta in 3.9 (2023-03-06).
- Motion smoothing (Settings > Headset > Motion smoothing: *Always enabled*, *Enabled if app
  supports it*, or *Disabled*, the default) uses depth and motion vectors. Without motion vectors
  from the app, it estimates them "based on pixel movements", and it may cause artifacts in apps
  that don't support it.
- Apps hand motion vectors to Varjo through the native API (`varjo_ViewExtensionVelocity`, with
  `velocityScale` to pixels per second and `includesHMDMotion`). Varjo's OpenXR extension list
  includes `XR_KHR_composition_layer_depth` and `XR_VARJO_composition_layer_depth_test`, but no
  motion-vector extension (no `XR_FB_space_warp`).

### OpenXR depth layers

- `XrCompositionLayerDepthInfoKHR`: `minDepth` and `maxDepth` (in [0, 1]) are the window-space
  depths of the near and far frustum planes; `nearZ` and `farZ` are their distances in metres,
  in (0, +infinity]. "A reversed mapping of depth [...] can be achieved by making
  nearZ > farZ." For X4's infinite reverse-Z projection that means `minDepth = 0`,
  `maxDepth = 1`, `nearZ = +infinity`, and `farZ` = X4's near distance (0.1 in the captured
  camera records).

### NVIDIA Streamline (X4's DLSS integration)

- Apps tag resources with `slSetTag` or `slSetTagForFrame` (`kBufferTypeDepth`,
  `kBufferTypeMotionVectors`, `kBufferTypeHUDLessColor`, ...) and pass per-frame `sl::Constants`
  with `slSetConstants`: `cameraViewToClip`, `clipToCameraView`, `clipToPrevClip`, `cameraPos`,
  `cameraFwd`, `jitterOffset`, `mvecScale`, `depthInverted`, `cameraMotionIncluded`,
  `motionVectors3D` and `reset` (matrices without jitter). Tags are only provided while a
  feature such as DLSS is active. Hooking these in `sl.interposer.dll` would give the layer X4's
  depth, motion vectors and camera matrices without reverse-engineering render passes.

### X4

- `X4.exe` (9.00) contains `slReflexSleep`, `slReflexSetOptions` and `ActivateReflex`, and the
  X4 folder ships `sl.reflex.dll` and `nvlowlatencyvk.dll`, so X4 drives Reflex through
  Streamline. No `reflex` key appears among the `config.xml` option names near `dlssg`/`fsr3g`.
  Reflex sleeps at the start of each frame, for a time predicted from recent frames, to keep the
  render queue short. That makes it a second pacer next to `pace_to_compositor`.
- `X4.exe` also contains `PipelineCache`/`pipelinecache` and `shadercache` (X4 keeps its own
  pipeline cache), and the config keys `frameratelimit`, `frameratetarget`, `presentmode`,
  `autosaveintervalfactor` and `adaptivesampling`.
- Community reports: micro-stutter when moving the camera, attributed to object loading with
  Vulkan on NVIDIA. Lowering mouse polling from 1000 Hz to 250–500 Hz, fullscreen mode (steadier
  GPU clocks) and FXAA helped some players. A Steam troubleshooting guide for X4 says to disable
  tools that hook the game's graphics API (RTSS, and the Steam overlay on some systems). Players
  also report frame drops in large battles with low CPU and GPU utilization, which points to one
  busy thread.

## Fix plan

Ordered by expected effect per effort. Each item says how to check it; the `pair_stats` columns,
`trace.request` and `tools/flicker_ab.py` already cover most of this.

### 1. No unbounded waits between `WaitGetPoses` and `Submit` (done)

Stutter kind A. Files: `observe_layer.cpp` (`compositor_loop`, present hook).

- Move the `read` fence wait (line 1171) to after the handoff. Or keep one fence per submission
  and release an image's hold only once that submission's fence has signaled; if the holds then
  exhaust the 3-image ring (`ring_size`, line 543), use 4.
- Bound the eye-fence wait (line 1220) by the budget even when there's no fallback image. If
  nothing new has finished, resubmit the image already on screen (`shown[e]`, always complete)
  with its pose.
- In async mode, release `p.mutex` before calling the driver's `vkQueuePresentKHR`, and don't
  wait on fences under the lock in `presenter_copy`: pick a slot whose fence has signaled
  (`vkGetFenceStatus`) or skip the copy. Inline mode (`async_submit=0`) still needs the lock
  around its own `Submit`.
- Move the `turn_stats` and `pair_stats` file writes, the `stereo.txt` reload and the
  request-file checks to a background thread (2 Hz is plenty) that publishes through atomics.
- Check: `waited_max` p99 under about 2.5 ms in every frame-rate bucket, and missed compositor
  frames in the 60–88 fps buckets down to the ≥ 88 fps level (about 0.1 per window).

### 2. Submission thread priority (done)

Stutter kind A.

- Call `AvSetMmThreadCharacteristicsW(L"Pro Audio", &index)` at the top of `compositor_loop`
  (MMCSS High category), or `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)`. The thread
  mostly blocks, so it won't starve X4.
- Recommend a High-performance power plan to players, against core parking.
- Check: the tails of `blocked_max` and `interval_max` in CPU-bound windows (< 60 fps).

### 3. SteamVR handoff and visibility (done)

Stutter kind A.

- Remove `PostPresentHandoff` from `submit_frame` (`runtime_bootstrap.cpp:160`); the loop calls
  `WaitGetPoses` right after `Submit` anyway. Before and after the change, time
  `Submit(left)`, `Submit(right)` and `PostPresentHandoff` separately.
- Each iteration, log `GetFrameTimings` for the previous frames (presents, mispresents, dropped
  frames, `m_flNewFrameReadyMs`, `m_flCompositorRenderStartMs`) to see the latch from SteamVR's
  side.
- Check: which call carries the ~11 ms wait, and whether `m_nNumMisPresented` and
  `m_nNumDroppedFrames` line up with the `late` column.

### 4. Don't penalize late frames (done)

Stutter kind B. File: `observe_layer.cpp:1306` (`pace_to_compositor`).

- Today X4 always waits for the next tick after a present. If every X4 frame takes 11.2 ms, X4
  presents only on every second tick (45 fps). `WaitGetPoses` itself returns immediately when an
  app runs behind (openvr #1515).
- Proposal: remember the tick count at which X4 was last released. At the present, if a tick has
  passed since then, the frame is late: return immediately (it will be picked up at the next tick
  anyway). Otherwise wait for the tick as now. X4 then degrades from 90 fps to about 89 instead
  of 45. Late frames are no longer aligned to ticks, so their display latency varies by up to one
  frame, which is far milder than halving the rate.
- Optional GPU guard: while the GPU is behind (the previous frame's copy fence hasn't signaled),
  hold X4 until it has. The driver then never queues more than one extra frame and never blocks
  inside the present.
- Check: fewer windows in the 60–88 fps buckets, and no ~22 ms mode in the present intervals
  (`trace.request`) while X4 runs at 80–89 fps.

### 5. Play mode without capture overhead (partly done: `MemoryTracker` unchanged)

Stutter kinds B and D. File: `observe_layer.cpp`.

- Write shader modules and pipeline events only when a capture was requested (the `observe.ps1`
  diagnostics), not whenever the launcher sets `X4VR_CAPTURE_DIR`. Keep `log()` off X4's threads
  by queueing lines to a writer thread.
- Measure the `MemoryTracker`'s cost first: compare runs with it disabled (an environment switch
  checked at device creation, since the tracker must see allocations from the start). If it
  matters, keep the bind hook cheap: store set-1 handles in a small lock-free ring and evaluate
  them (snapshot, main-camera test) once per present instead of on every bind, and read the
  matrices with a guarded `memcpy` (SEH) instead of `ReadProcessMemory`.
- Measure how long `vkCreateGraphicsPipelines` takes during play; log only calls over 2 ms, from
  the writer thread. If many are long, raise NVIDIA's shader cache size. A Fossilize-style replay
  of the recorded pipelines at startup would be the next step.
- Check: the frame-rate buckets with the tracker on and off, in the cockpit and on foot.

### 6. Release X4 after `Submit` (experiment)

Stutter kind A.

- Publish the tick after `Submit` instead of right after `WaitGetPoses` (line 1165), so X4's
  next GPU work is queued behind SteamVR's copy instead of ahead of it. This costs X4 the time
  from `WaitGetPoses` to `Submit` every frame, so only keep it together with item 4.
- Check: fallbacks and misses in the 80–88 fps bucket.

### 7. One pacer (done)

Stutter kind B.

- Turn X4's frame limiter off (`frameratelimit=false`); the launcher currently accepts a 90 fps
  cap. In pair mode, any cap below 180 fps defeats the mode.
- Find out whether Reflex's sleep is active in your configuration (it runs through Streamline;
  the layer can log whether `nvlowlatencyvk.dll` or `sl.reflex.dll` load, and whether
  `VK_NV_low_latency2` is enabled on the device). If X4 lets you, compare with it off.
- Check: present-interval jitter in `trace.txt`, misses per window.

### 8. GPU headroom

Stutter kinds A and B.

- Keep X4's GPU time under about 90% of the frame (Valve's adaptive-quality thresholds): DLSS
  upscaling, which is already allowed, or a smaller DSR factor when the 80–88 fps bucket grows.
- Each eye uses about 70% of the frame width (STATUS.md). A frame aspect ratio closer to the eye
  frustum would cut wasted pixels, if X4's HUD tolerates it.
- The OpenXR path copies both eyes into runtime swapchains every frame
  (`openxr_runtime.cpp:441`). Copying only the eye with a new image saves one copy per frame.

### 9. Half-rate mode for heavy scenes (experiment)

Stutter kinds B and C.

- A variant of pair mode that renders left and right back to back but submits a pair every other
  compositor frame, so X4 needs 90 fps instead of 180. Both eyes then change together at 45 Hz,
  the two frame budgets pool into 22.2 ms, and the compositor reprojects the frames in between
  (on Varjo with positional timewarp once depth is submitted). Varjo Base's Motion smoothing and
  its "Fixed 45 fps" VSync option, both added in 3.8, are meant for apps running below the
  refresh rate. Meta's fixed-fraction guidance favors this over hovering at 70–85 fps. Switch
  automatically with hysteresis, like ASW.
- Replace the `Sleep(1)` loop in `wait_mid_frame` (line 948) with a
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer.
- Check with `flicker_ab.py` against AFR in the same scene.

### 10. Depth for the compositor (OpenXR first)

Stutter kind C, and every repeated frame.

- Varjo does positional timewarp when it receives depth (documented for the native SDK; see
  [Open questions](#open-questions) for OpenXR). Chain an
  `XrCompositionLayerDepthInfoKHR` to each projection view (reverse-Z: `minDepth = 0`,
  `maxDepth = 1`, `nearZ = +infinity`, `farZ` = X4's near distance in metres), with each eye's
  depth copied at the same present as its color. SteamVR takes depth through
  `Submit_TextureWithDepth`, but whether the Varjo driver uses it is unknown.
- Getting the depth: find X4's scene depth attachment in the layer, or use Streamline's
  `kBufferTypeDepth` tag when DLSS is on.
- Risk: the HUD and cursor are baked into the color image without their own depth, so they will
  move with the scene behind them. `XR_VARJO_composition_layer_depth_test` or clamping near
  depth may limit this. Test in the cockpit first.
- Depth corrects head translation for the stale eye and for repeated frames. It doesn't correct
  object motion.

### 11. Synchronized eyes and native stereo (large, shelved)

Stutter kind C.

- UEVR-style synchronized rendering for X4: advance the simulation clock only every other frame,
  so both eyes of a pair show the same world state. This removes AFR's eye desync and the
  Pulfrich-like depth errors; world motion updates at 45 Hz. It needs X4's frame-time source
  (hook the delta time) and care with particles and animation (UEVR's Skip Draw vs Skip Tick
  trade-off).
- Native stereo, with both eye views rendered from one simulation tick, remains the complete fix.
  STATUS.md's leads are the 2×100 view pool and the scene-camera producer at `0x77a376`.

### 12. Motion vectors (large)

Stutter kind C.

- Varjo's Motion smoothing takes app motion vectors only through the native SDK. SteamVR's
  `Submit_TextureWithMotion` only helps where SteamVR's own motion smoothing runs, and the Varjo
  driver doesn't use it. X4's motion vectors (Streamline's `kBufferTypeMotionVectors`) include
  AFR's left/right camera jump, so that parallax would have to be removed first.
- An in-layer alternative: extrapolate the stale eye with the game camera's angular velocity
  (turn compensation extended over time), using depth to separate near cockpit pixels from far
  world pixels.

### Player-side settings (in the README since 2026-09-29)

- NVIDIA Control Panel, X4 profile: Power management "Prefer maximum performance", a large or
  unlimited Shader Cache Size, Max Frame Rate off. Low Latency Mode doesn't matter: it only
  applies to DirectX 9 and 11, and X4 uses Vulkan.
- X4: frame limiter off, VSync off (already required), and a higher autosave interval factor if
  autosaves hitch.
- Windows: High-performance power plan, and no overlays that hook Vulkan (RTSS, OBS game capture,
  Overwolf, and the Steam overlay on some systems).
- Mouse polling at 250–500 Hz (reported to help X4 camera stutter).

## Verification plan

1. Add a per-frame ring buffer to the submission thread, dumped on request like
   `trace.request`. For each frame record when `WaitGetPoses` returned, when the `read` fence was
   done, when `p.mutex` was acquired, when the eye fences were done, when each `Submit` and
   `PostPresentHandoff` returned, whether each eye was fresh or a fallback, X4's present time and
   whether X4 was released late, plus `GetFrameTiming` for the previous frame.
2. Use the hitch injector (`hitch_ms`, `hitch_every`) with GPU-heavy settings to reproduce misses
   on demand, and `tools/flicker_ab.py` for timed A/B runs.
3. Targets: in every frame-rate bucket, missed compositor frames per window at the ≥ 88 fps level
   (about 0.1) and `waited_max` p99 under 2.5 ms; no ~22 ms mode in the present intervals while
   X4 runs at 80–89 fps.

## Open questions

- Answered: with the Varjo driver SteamVR blocks in `Submit`/`PostPresentHandoff` while the
  handoff is called, and in `WaitGetPoses` without it (see Implementation status). Still open:
  does the latch stay at 3.2–4.5 ms under GPU load?
- Does Varjo's OpenXR runtime use `XR_KHR_composition_layer_depth` for positional timewarp the way
  its native API does? `openxr_probe` lists the extension; confirm it visually.
- Is Reflex active in X4 9.00 with DLSS on? Without it, no (see [One pacer](#one-pacer-item-7-2026-09-29)).
- What does X4's `adaptivesampling` option do? If it's dynamic resolution, it's a GPU-headroom
  lever.
- How long do X4's in-game pipeline creations take with a warm NVIDIA shader cache?
- Are X4's world units metres? The depth layer's `farZ` depends on it.

## Sources

OpenVR and SteamVR

- [openvr.h](https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h) (the project pins revision `0924064` in `external/openvr`)
- [Vulkan support in SteamVR (OpenVR wiki)](https://github.com/ValveSoftware/openvr/wiki/Vulkan)
- [IVRSystem::GetDeviceToAbsoluteTrackingPose (OpenVR wiki)](https://github.com/ValveSoftware/openvr/wiki/IVRSystem::GetDeviceToAbsoluteTrackingPose)
- [openvr #1401: blocking in PostPresentHandoff instead of WaitGetPoses](https://github.com/ValveSoftware/openvr/issues/1401)
- [openvr #1515: WaitGetPoses not blocking for consistent amounts of time](https://github.com/ValveSoftware/openvr/issues/1515)
- [Alex Vlachos, Advanced VR Rendering Performance, GDC 2016](http://media.steampowered.com/apps/valve/2016/Alex_Vlachos_Advanced_VR_Rendering_Performance_GDC2016.pdf)
- [SteamVR Developer Hardware: CPU "delays" and process priority](https://steamcommunity.com/app/358720/discussions/0/141136086933022141/)

Frame-rate policy and perception

- [Meta: Asynchronous Timewarp Examined](https://developers.meta.com/horizon/blog/asynchronous-timewarp-examined/)
- [UploadVR: The Zuckerfrich Effect, False 3D in VR](https://www.uploadvr.com/the-zuckerfrich-effect-false-3d-in-vr/)

Other VR mods

- [UEVR README (rendering methods)](https://github.com/praydog/UEVR/blob/master/README.md)
- [R.E.A.L. mod for GTA V (alternate-eye rendering)](https://github.com/LukeRoss00/gta5-real-mod)

Varjo

- [Rendering to Varjo headsets (native SDK)](https://developer.varjo.com/docs/v3.0.0/native/rendering-to-varjo-headsets)
- [Varjo XR plugin rendering settings (depth submission)](https://developer.varjo.com/docs/unity-xr-sdk/rendering-settings-in-varjo-xr-plugin)
- [Varjo_types_layers.h (velocity and depth extensions)](https://developer.varjo.com/docs/apidocs/_varjo__types__layers_8h.html)
- [Varjo OpenXR overview (supported extensions)](https://developer.varjo.com/docs/openxr/overview)
- [Varjo: Motion smoothing](https://support.varjo.com/hc/en-us/motion-smoothing)
- [Varjo Base release notes 4.7.1 and older](https://support.varjo.com/hc/en-us/varjo-base-release-notes-4.7.1-and-older)
- [DCS forum: Varjo Aero refresh rates](https://forum.dcs.world/topic/339974-varjo-aero-refresh-ratesthe-missing-options/)

OpenXR

- [XrCompositionLayerDepthInfoKHR](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrCompositionLayerDepthInfoKHR.html)

Windows and GPU scheduling

- [NVIDIA developer forum: VK_EXT_global_priority on Windows](https://forums.developer.nvidia.com/t/windows-vk-ext-global-priority/196010)
- [Multimedia Class Scheduler Service](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service)
- [CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
- [timeBeginPeriod](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod)

NVIDIA

- [Streamline programming guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuide.md)
- [How NVIDIA Reflex works (GamePerfTesting)](https://github.com/klasbo/GamePerfTesting/blob/master/text/02-reflex.md)

X4

- [Micro-stutter while moving the camera (Steam)](https://steamcommunity.com/app/392160/discussions/3/3083250789418342419/)
- [Stutter and FPS drop despite low GPU/CPU utilization (Steam)](https://steamcommunity.com/app/392160/discussions/0/3818531283318922188/)
- [X4 Troubleshooting guide (Steam)](https://steamcommunity.com/sharedfiles/filedetails/?id=2796807980)
