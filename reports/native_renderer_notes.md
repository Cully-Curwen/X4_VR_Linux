# Native renderer observations (X4 9.00, local executable only)

These are research leads, not callable API declarations or verified stereo hooks.
Executable SHA-256: `19750a6563889a970f434b5566eb396c6b2dc29ff814bd3e336f838176ad6891`.

## Verified upload path

`0x1216770` assembles a camera uniform, copies 0x700 bytes to mapped Vulkan
storage, and binds it. At return site `0x1218468`, reconstructed R14 references
the wrapper and `[R14+8]` references a camera object. In process 30648, 16 samples
matched the uploaded temporary block byte-for-byte. Camera +0x40 supplies view,
+0x00 supplies inverse view, and `YFlip * camera[0x1c0] * camera[0x140]`
supplies the projection in those samples. Some objects are stack-local.

## Higher-level paths (static interpretation; not a replay contract)

- `0xf8a120..0xf8aad2` preserves first argument in RBX and second in R12.
  It selects a 0x270-byte slot using the integer at RVA 0x6b66280, takes a
  critical section, and writes RBX and the slot pointer to 0x6d1b900/0x6d1b908.
  It calls `0xf758f0` to reset render state, configures many pass records, invokes
  Streamline-related work, then calls `0xf874c0` at 0xf8a7fa. Resource copies and
  cleanup follow. Do not repeat this routine without understanding its ownership,
  frame-slot, temporal state and synchronization effects.
- `0xf874c0..0xf89cca` dispatches numerous render stages, invokes camera upload
  directly in some stages, and includes DLSS evaluation. Its stack frames occur
  in the observed camera upload traces. This is broader than a geometry-only draw.
- `0xfa2d70..0xfa35bf` includes Streamline frame-token creation and Reflex work.
  Calling it twice is not equivalent to drawing two views of one simulation tick.
- `0xf758f0..0xf76115` resets globals and copies 0xd10-byte camera records from
  `0xf403e0` to RVA 0x6d1c950 and 0x6d1d660. The latter camera address appears
  in live multi-view samples. The camera-record capture currently includes 0xce0
  bytes, enough for the verified matrix fields, not the entire inferred record.

Bounded annotated disassembly is in `render_views_candidate.asm`,
`render_stage_dispatch.asm`, `render_frame_candidate.asm`, `render_parent.asm`,
and `prepare_camera_candidate.asm`. Data references and unwind ranges alone do
not prove function semantics, camera ownership, or suitability for hooks.

## Crash and current reproduction

The user reported an unexpected crash in process 5736 after loading gameplay.
Its trace selected zero UI matrices under the old sampler; there is no verified
cockpit camera from that run. No crash cause is established.

The new sampler captures multiple nonzero views. Process 17548 was launched
with it and the external crash recorder (PID 48052). Reports are in
`captures/process-17548-134346647933747864/` and
`captures/debug-8e727a2a6e0d4043ad17bf4a14e5335f/`. At the latest inspection both
processes were alive and perspective camera samples were being recorded. The
user controls loading the save; cockpit readiness has not yet been reconfirmed.
Process 17548 subsequently crashed with access violation in X4's XML patch loader,
not at a captured camera call. See `x4_crash_analysis.json` for the dump evidence;
that location does not establish the upstream cause. The user says this save is
normally stable, so a tracing-induced regression must be assumed for isolation.
The next run uses only API observation plus the external recorder, disabling both
mapped-memory sampling and native stack reconstruction. Its debug directory is
`captures/debug-8cc7b011f162463ba9d1af6d8d3136ae/`.
Recheck live process state before relying on PIDs. Do not close a user-loaded
session without coordinating with the user.

## Subsequent startup and external camera reads

The user abandoned the old-save comparison and chose to continue with a new game;
save corruption has not been established. Process 41400 exited normally on request.
Process 6876 now runs the OpenVR startup bridge without memory/native-stack tracing;
it successfully created a runtime-compatible device and presented frames.

The global camera records at 0x6d1c950 and 0x6d1d660 were copied externally using
PROCESS_QUERY_INFORMATION | PROCESS_VM_READ after executable and loaded-signature
verification. See `camera-peek-6876-initial` and `camera-peek-6876-followup`.
Both are active, coherent-looking perspective records with forward +Z and infinite
reverse depth (P22=0, P23=0.1, P32=1). They share position but differ slightly in
orientation. This does not establish whether they are current/previous views,
camera-shake variants, or the cockpit camera. Controlled user movement is pending.
CPU eye-matrix composition now passes against these records; no live camera writes
or stereo scene-pass replay has been attempted.

## Native camera construction / finite visibility projection

- `0xf403e0..0xf4055c` initializes the camera record. It does not retrieve the
  current gameplay camera. `0xf758f0` copies these initial records to globals,
  which are then updated elsewhere.
- `0xf40560..0xf40a24` copies the world-from-camera input from RDX to +0x00,
  computes its inverse at +0x40, copies a stack-argument matrix to +0x80 and jitter
  to +0x1c0, writes scalar fields at +0xcc0/+0xcc4/+0xcc8, then calls 0xf414c0.
  Static calling-convention interpretation is not yet a callable ABI contract.
- `0xf414c0..0xf41f04` computes projections and visibility data. Perspective mode
  uses finite forward depth at +0xc0 and infinite reverse depth at +0x140.
  It writes P_finite*V at +0x100 and P_reverse*V at +0x180, then calls 0x1218f80
  for perspective visibility data at +0x280 (orthographic path calls 0x1219820).
  Captured scalar near/far at +0xcc0/+0xcc4 are 0.1 and 400000 game units.
  Camera-dependent visibility data therefore cannot be left untouched when adding
  stereo poses/projections. The CPU adapter now constructs independent typed eye
  visibility planes; it does not yet populate X4's opaque native visibility record.

Process 6876 reached 12000 presents and exited cleanly, with runtime shutdown.
The user-confirmed centered cockpit capture exists; no controlled left-look capture
was obtained. Latest live state must be revalidated before attempting further reads.

## Selected and derived camera update path (static analysis)

`camera_native_refs.json` was produced by `tools/find_native_refs.py` against the
pinned executable. It searches unwind-described ranges only; 845 ranges were
incompletely decoded. This is not an exhaustive call graph (leaf/gap code and
computed addresses/indirect calls are not resolved).

- `0xf76620..0xf76c3f` (`camera_scene_setup.asm`) chooses a camera-bearing object,
  takes object+0x10, stores the pointer at `0x6d1b920`, and copies 0xd10 bytes to
  both `0x6d1c950` and `0x6d1d660`. They are initially copies of one selected
  source, not evidence of native left/right eyes or current/previous frames.
- `0xf7e980..0xf7f7f4` (`camera_derived_setup.asm`) uses the second global as
  its destination. At `0xf7f462` it calls `0xf41000` with RCX=derived camera,
  RDX=stack+0x20 (a computed 64-byte pose). The additional transform's meaning
  has not been established; do not label it cockpit head movement or shake yet.
- `0xf41000..0xf41343` (`camera_pose_update.asm`) copies RDX's matrix to camera
  +0x00, rebuilds +0x40, and tail-jumps to `0xf414c0`. The first ten bytes are
  `48 8b c4 48 81 ec 68 01 00 00` (`mov rax,rsp; sub rsp,0x168`). It is a
  promising point to apply a pose before native projection/visibility refresh.
  A two-pointer calling convention is inferred, not yet runtime-validated;
  return-value semantics are unknown.
- `0xf8d8d0..0xf8db42` (`camera_constructor_caller.asm`) constructs a camera at
  object+0x10 through `0xf40560`, consistent with the selected-object layout.

`x4_pose_detour` implements a selected-pointer-filtered two-argument trampoline.
It is deliberately not linked into `x4vr_observe` and has no X4 addresses. Its
call behavior is exercised in `pose_detour_tests`. The assembly fixture shares the ten-byte
entry sequence but otherwise just copies a pose, counts calls and returns an
integer. Tests validate that mechanism, NOT native X4 ABI, camera ownership,
projection compatibility, in-game teardown or stable head tracking. The wrapper
keeps the original input pointer for other cameras and invalid/declined/throwing
transforms; accepted poses are temporary synchronous inputs. C++ exceptions from
the callback fall back, but access violations/native faults are not suppressed.
All target callers must be quiescent for installation/removal/destruction; an
in-flight counter alone cannot establish safe trampoline reclamation.

The separate startup-only `x4vr_native_pose` module now supplies the pinned X4
hash/signatures and selected derived-camera address. It always declines pose
replacement and records at most 320 input samples. The recorder loads it before
the new child's PE entry point; this is not an attach/hot-unload path. Its first
retail trial (38392) installed successfully but was explicitly closed by the user
before a sample arrived. No native call or stable gameplay is proven by that run.

`camera_pose_update_refs.json` additionally finds a second direct caller at
`0xb31ee8`, whose camera is object+0x90, subsequently copied to another record.
See `camera_pose_other_caller.asm`. This reinforces the need for pointer filtering
and rules out treating all calls to `0xf41000` as player-camera updates. Both known
callers ignore return values, but indirect/uncovered callers remain unresolved.

## Controlled cockpit look verification (process 16860)

The user identified a centered new-game cockpit and then held left, right and
upward free-look without intentionally turning the ship. Each direction capture
waited two seconds after the user's reply to allow switching back into the game.
The reports `camera-peek-16860-cockpit-{center,left,right,up}` each contain eight
double-read samples of both globals. All 64 view/inverse pairs passed consistency
checks; repeated reads matched, which is still not an atomicity guarantee.

For each sample, local rotation was computed as
`transpose(center.inverse_view.rotation) * look.inverse_view.rotation`, using
the last centered sample for that same global. Yaw is `atan2(R02,R22)`; pitch is
`asin(-R12)`. On source global `0x6d1c950` all eight samples per direction yield:

| User action | Local yaw (degrees) | Local pitch (degrees) |
| --- | ---: | ---: |
| Look left | -64.999032 | approximately 0 |
| Look right | +64.998992 | approximately 0 |
| Look up | +0.000436 | -34.998905 |

Derived global `0x6d1d660` follows the same directions, with small extra offsets:
left yaw -64.999249 to -64.799074; right yaw +64.999205 to +65.398849; upward
pitch -34.893021 to -34.786892. The extra transform's meaning remains unknown.
Both globals therefore correlate with cockpit free-look in this tested mode;
this does NOT prove changing either global changes the rendered view. Native pose hook
samples were also captured on thread 6152, selecting base+0x6d1d660, while the
game stayed responsive. Callback always returned false, so input poses were not
changed. The ship/world position drifted; these tests do not calibrate units/metre.
They do not verify headset tracking, stereo geometry, other camera modes, or
long-duration gameplay stability.

## Failed head-look experiment and render camera links (September 25)

PID45976 acknowledged enable and reported applied headset orientation. The
disabled/active camera-peek reports contain consistent inverse pairs and changing
relative orientation. The user nevertheless reported no visible cockpit movement.
The global-copy hook is insufficient; successful callback logs are not a visual
success criterion. X4 was absent on the subsequent process check (exit cause unknown).

Static findings for the pinned executable:

- `f76620` selects object `[6d1bae0]`, stores object+0x10 at `[6d1b920]`,
  and copies the camera to the context and globals. Those globals are downstream
  copies, not necessarily the records used by the main geometry passes.
- `f76c40..f76f3c` enumerates view objects from a container. R14 starts at
  `6d1e5a0`, advances 0x180, and stops at `6d215a0` (32 slots).
  At `f76c9d`, slot-0x90 receives the view object. At `f76dfa..f76dfe`,
  slot-0x78 receives object+0x10. The wrapper at slot-0x80 thus contains the
  camera pointer at +8, matching the uniform uploader's layout.
- `render_stage_dispatch.asm` reads the view object at `f89600` and passes its
  camera (object+0x10) at `f898c2`. Its call to `1216190` at `f898f0` leads
  to the previously captured `1216770` upload. This directly explains how heap
  camera pointers reach that captured upload, not which view is the cockpit.
- `103c5d0` consumes camera viewport fields +0xcf4..+0xd00; it is not itself
  sufficient evidence of camera selection or pose update.
- `camera_derived_dispatch.asm` calls `f7e980` at `f7c653`/`f7caa2` under
  conditional rendering flags. Its exact pass purpose remains unresolved.

The read-only sampler's `--render-links` option records selected/context cameras
and the 32 wrapper pointers at `6d1e528 + i*0x180`. Slots may retain stale views.
It rechecks the full pointer chain around two bounded record reads and rejects
changed/inaccessible links. No atomicity, lifetime ownership, frame membership or
safe write location is claimed. Next: correlate these records with normal cockpit
free-look before choosing a replacement hook. GPU camera uniforms alone are not
sufficient: object WVP, visibility and temporal history must remain coherent.

### Camera producers and live labels

`camera_full_update_refs.json` finds seven decoded direct calls to `f40560`
(845 ranges incompletely decoded, so not exhaustive). Several construct defaults:
`779b1d` feeds a `QueueLensflare` view; `9861de` feeds `U::OverlayCamera`;
`b2dc26` belongs to an `Anark` object. `983a00` instead composes a variable
pose and projection from its inputs. It is called by `779a00` at `77a254`
and a cache routine `983e40` at `983ee3`; these paths copy the resulting record
again. None is yet authorized by evidence as a cockpit-only replacement hook.

The view objects have a 64-byte diagnostic label at object+0xf0c, demonstrated
by literal label writes in those producers. The sampler optionally reads this
field only for known view-object paths, within its pointer recheck interval.
Names are hints, not proof of active frame membership or safe mutation.

New normal-launch PID62052 passed executable/loaded signature guards. After the
user's readiness confirmation, `camera-peek-62052-render-links-center` captured
eight rounds read-only. Selected object/current camera pointed at a camera labeled
`class U::Zone`, while the per-view slots contained weapon aim indicators,
QueueLensflare and Anark cockpit_crosshair/targetsystem/infobar/compass/etc.
This distinguishes the selected scene path from ancillary passes. In particular,
the previously captured f898f0 upload path cannot automatically be called the
main scene path: these slots include UI and other ancillary cameras.
One source-global sample was inverse-inconsistent despite repeated equal reads;
this demonstrates why equality alone cannot establish a coherent frame snapshot.
The user then confirmed looking left; after the requested two-second delay,
`camera-peek-62052-render-links-left` captured eight rounds. Relative orientation
uses transpose(center R)*left R with the last consistent centered record, matching
by diagnostic label for view objects (not slot/address), by source for fixed paths.
The selected `class U::Zone` camera and context copy follow approximately -65deg
yaw in all eight samples, as do cockpit_crosshair, monitors, widget_fullscreen
and weapon aim indicators. Lensflare and the other captured Anark cameras remain
near 0deg relative yaw. The source global follows -65deg; the derived global ranges
about -65.4 to -65deg. There are multiple weapon-indicator instances sharing a
label, so that label comparison does not prove individual identity.
Selected scene camera addresses alternate between two heap records, consistent
with the producer's two-buffer allocation, not a permanent writable pointer.
Head-look mutation is not enabled.

`camera_queue_producer.asm` supplies a more direct pre-render copy site:
`77a365..77a376` copies 0xd10 bytes from stack camera RBP+0x860 to R13+0x10.
R13 comes from `fa0a40` at `77a1ba`; preceding branches populate the stack camera
from `983a00` or a virtual camera-cache method. Native projection may be rebuilt
at `77a360` before this copy. A prospective hook must cover both branches and
preserve later viewport/scalar updates. This is a candidate only, not implemented
or verified as a safe mutation point at that point in the investigation;
constructor/default cameras must be excluded.

### Replacement implementation (not yet game-tested)

`CopyCallHook` now replaces only the call at `77a376`, preserving the original
copy function and its return. A bounded near allocation holds an absolute-jump
relay, sealed RX after initialization. Installation/removal require same-thread
quiescence; original call opcode/target are verified and a changed installed
patch is not overwritten. The hosting module stays pinned until process exit.
The owned assembly fixture proves unrelated calls are unaffected, original
copy precedes observation, callback exceptions/recursion are contained, 8000
concurrent copies complete, wrong-thread removal fails and bytes are restored.
No real game ABI/synchronization safety is proven by fixture tests.

`-SceneHeadLook` selects this path and prepares tracking disabled. The callback
checks destination size, the double-buffer pool at `[[6cf1578]+1f0]`, stride
0xf80/offset0x10, producer half `[6b66280]^1`, count `[[6cf1578]+1e8]` within
1..100, and exact Zone label. It records fresh poses, then optionally composes
tracking. A private aligned 0xd10 record is rebuilt through f41000, then committed
to the destination; original source/other camera types remain untouched. Loaded
argument setup and rebuild entry are signature-checked in addition to image hash.
Release/Debug each pass 16 suites. Native producer filter acceptance, visible
effect, culling/WVP/history and cockpit UI consistency still need game validation.

Read-only preflight against normal-launch PID62052 also matched all 17 loaded
argument-setup bytes at 77a365, E8 at 77a376 and target19efe90. Pool read at
0x226dd8a0090, half1, produced count30; selected Zone camera0x226dd90e7a0
was aligned slot114 in that pool. This supports the two-half/stride/layout
assumptions, but the snapshot is not atomic and observes the selected read-side
camera, not an intercepted producer callback. It does not prove guard acceptance
on the write side. The current process remains unmodified pending restart consent.
