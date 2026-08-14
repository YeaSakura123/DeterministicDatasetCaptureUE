# Architecture and determinism contract

## Goal

The plugin treats dataset generation as an offline simulation rather than a screenshot utility. A sample is valid only when every modality and its metadata refer to the same evaluated logical frame. Repeated jobs must prove exact metadata and auxiliary-buffer replay; renderer-dependent color may use a declared numeric tolerance with a generated heatmap.

## Frame pipeline

```text
fixed engine tick
  -> pre-actor: evaluate Sequencer at absolute time
  -> pre-actor: discover/seed/seek Niagara components
  -> pre-actor: call custom deterministic drivers
  -> normal actor, animation, physics and component ticks
  -> post-actor: resolve the active camera
  -> capture HR
  -> capture isolated native-HR linear HDR
  -> render spatially supersampled reference HR and downsample to a fixed grid
  -> derive LR from HR, or render native LR without ticking
  -> capture float SceneDepth without ticking
  -> optionally arm AfterDOF and after-tonemap real Main View RDG extraction
  -> render the player Main View at LR internal / HR display size
  -> next pre-actor callback: read back and commit the pending sample
  -> atomically write files and manifest hashes
```

Warmup ticks also submit each enabled SceneCapture without writing files. This initializes the persistent per-modality View State before frame zero; without render warmup, the first native LR frame showed repeat-run differences even when simulation state and all auxiliary buffers matched.

`FrameStep` normally skips writes while still evaluating intermediate simulation frames, so particles, animation events and physics do not jump over time. FG replay roles may also suppress the player Main View on uncaptured ticks. This prevents an intermediate render from advancing endpoint View State while the world simulation still advances at every fixed tick.

## Control matrix

| Content | Control mechanism | Guarantee within one job | Replay requirement |
|---|---|---:|---|
| Sequencer transforms/camera/animation/events | continuous `SetPlaybackPosition(..., Play)` at absolute time | exact frame evaluation | author the content in the Level Sequence |
| Skeletal animation outside Sequencer | fixed engine delta | stable tick cadence | animation logic and inputs must themselves be deterministic |
| Legacy Cascade particles | fixed engine delta and seeded global random stream | stable sequential playback | use seeded particle modules for strict repeatability |
| Niagara CPU/GPU | Desired Age, fixed seek delta, forced solo, deterministic system seed/fixed tick | exact requested age; stable configured seed | data interfaces that read external/live state need a custom driver or Sim Cache |
| Chaos | fixed engine delta plus `p.Chaos.Solver.Deterministic=1` | enhanced deterministic ordering | platform, engine build and initial conditions must remain identical |
| Third-party/custom VFX and gameplay | `SRDatasetControllable` interface | whatever the implementation restores/evaluates | implement all three callbacks and use the supplied seed/time |
| HR/LR/Depth/reference pairing | all captures happen after the same world tick before the next simulation advance | identical logical state and camera transform | no custom render hook may mutate gameplay state |
| Rigid FG endpoint motion | `FMotionVectorSimulation` previous transforms point to the last captured endpoint | complete endpoint interval for scene components | skeletal bones and WPO remain outside this guarantee |

“Absolute control” therefore means the plugin owns time and every participating system is either covered by a built-in driver or explicitly implements the control protocol. Unsupported live inputs are not silently certified as deterministic.

## Output contract

- The current backend emits `spatial-sr-data-v1` and is certified only for single-frame spatial SR. It deliberately records `temporalTrainingCertified=false` and rejects `nr-sr-data-v2`/`nr-fg-data-v1` jobs until the Main View/RDG backend supplies their mandatory buffers.
- Frame numbers are inclusive and expressed in the job's rational frame rate.
- Every written frame separates `logicalFrameId` from its actual `renderSubmissionId` values. Render submissions never advance scene simulation; each reference modality owns a persistent SceneCapture view state, while optional temporal diagnostics use the persistent player Main View state.
- HR/LR PNG uses `SCS_FinalColorLDR`: display-referred, tonemapped output. It is not pre-tonemap linear HDR and must not be labelled as such.
- Legacy `depth/` is `SCS_SceneDepth` in Unreal centimeters. The temporal path separately stores raw reversed-Z device depth, linear view depth in meters and a validity mask.
- `color_lr_scene_hdr` is real Main View AfterDOF linear scene RGB before the Temporal Upscaler. It records pre-exposure, current/previous jitter and complete view/projection transforms.
- `color_hr_native_scene_hdr` is an isolated non-jittered HR view. `color_hr_reference_scene_hdr` renders at 2x-4x spatial resolution and is filtered back to the same fixed HR output grid without simulation/history advance.
- `color_main_view_hudless_after_tonemap` is captured after tonemap and before Slate/UI composition. It does not yet prove that every possible in-world UI primitive is absent.
- `object_id` reads Custom Stencil as an integer in `[0,255]`; zero is unlabeled.
- PNG and EXR writes use a temporary `.part` file followed by an atomic rename.
- Resume skips a sample only if all required modality files exist; it recalculates their hashes into the new manifest.
- The manifest records the complete normalized job, engine version, state, pairing rule, camera pose and SHA-1 of each encoded file.

## Reproducibility acceptance test

For a candidate scene:

1. Delete or select two empty output roots.
2. Run the same job twice with the same cooked/editor build, RHI, GPU driver and content revision.
3. Run `ValidateDataset.py <run-b> --compare <run-a>`.
4. Metadata and non-color buffers must meet their exact gates. A color mismatch is accepted only when it meets the versioned numeric tolerance; the validator writes a heatmap for inspection.
5. A failed required check is a determinism violation. Move the differing system into Sequencer, enable its native deterministic/cache mode, or implement `SRDatasetControllable`.

Cross-GPU bit-identical floating-point rendering is not guaranteed by Unreal. Training-set provenance should therefore also pin GPU model/driver, RHI, engine changelist, project commit and console-variable profile.

Each frame also records a CPU scene-state SHA-1. Its sorted canonical input covers Actor identity/class/transform/visibility/tick/interface ownership, SceneComponent state/transforms, every current component-space skeletal bone transform, Niagara asset/age mode/desired age/seed/solo/seek settings and Cascade asset/activity. Actor paths that tick without `SRDatasetControllable` are emitted as an audit list. This proves the declared CPU/component state replays; it deliberately excludes Niagara/Cascade GPU particle payloads, material/WPO shader state, Chaos internal solver state and opaque third-party buffers.

## Temporal certification boundary

The implementation now captures pre-exposed linear HDR Color, decoded raw Velocity and coverage, camera-completed current-to-previous Motion, device/linear depth plus validity, current/previous jitter, exact jittered/unjittered matrices, exposure/pre-exposure, reactive/transparency masks, Object ID and isolated HR native/reference outputs. It also blocks on engine streaming before the first sample, hashes sorted `UTexture2D` residency state per frame, snapshots Mip/LOD/streaming CVars and reads the actual material texture Mip bias from each GPU View Uniform. A paired-process gate now swaps the actual auxiliary render order between `HR -> Reference -> LR -> Depth` and `LR -> Depth -> HR -> Reference`, requires normalized provenance equality and compares every output. `nr-sr-data-v2` nevertheless remains disabled until production disocclusion, Main View/reference pixel-domain comparison and non-fixture scene coverage all pass. Adding placeholder metadata or renaming a SceneCapture output is not acceptable.

Frame generation uses separate endpoint and intermediate replay roles. Its intermediate `Sτ` cannot update either endpoint's previous state. `motion_1_to_0` is currently captured independently across the full endpoint interval for rigid components; `motion_0_to_1` is deliberately absent rather than being synthesized by negation.

### Experimental RDG diagnostic path

The UE 5.7 `AfterDOF` scene-view-extension callback reads the `SceneColor`, `GBufferVelocity`, `SceneDepth`, separate translucency and Custom Stencil resources associated with the real player view. A compute pass decodes UE Velocity, fills uncovered static pixels from depth and `ClipToPrevClip`, converts motion to display pixels/top-left origin, linearizes depth to meters, derives validity/reactive/transparency channels, copies Object ID and writes exact View uniform metadata to readback. A second extension copies display-resolution scene color after tonemap but before Slate/UI.

Two request modes exist: persistent native-LR SceneCapture and real player Main View. The Main View runner fixes the viewport to HR, locks `r.ScreenPercentage` to the LR/HR ratio, disables dynamic resolution, filters out SceneCapture views, and defers sample commit until the next pre-actor callback so the real view has rendered without advancing the logical state.

The real Main View path has produced the requested LR render rect, non-zero TSR jitter and valid HDR/Motion/Depth/Object-ID/mask buffers in D3D12 tests. The semantic fixture verifies moving-object direction/magnitude, reset motion, Velocity coverage, 1/10/100 m depth, reversed-Z reconstruction, transparency and occlusion/disocclusion geometry. Geometry/motion outputs replay exactly in the checked fixture, while Lumen-dependent color remains numerically close but not always byte-identical across processes.

Capture order is a job-level enum rather than a metadata-only label. Each SceneCapture is actually submitted in the selected order, warmup follows the same ordering, and submission IDs are assigned and serialized in execution order. The v0.3.2 paired fixture recorded both opposite orderings, produced byte-identical hashes for all 30 non-color modality/frame pairs, and passed 574/574 required checks. Color uses the same numeric reproducibility policy as clean replay; the worst HUD-less pair measured 59.1 dB PSNR.

The capture also exports `depth_previous_reprojected_device`, `history_rejection_mask` and `history_rejection_valid`. At a motion-reprojected previous pixel, labeled geometry uses Custom Stencil identity; unlabeled static/camera-only geometry compares the prior visible Reversed-Z depth with the current surface reprojected into the previous camera. Resets and out-of-bounds reprojections are exact rejections. Unlabeled Velocity-covered pixels are conservatively rejected with validity zero because a camera-only previous depth cannot represent arbitrary object deformation. Custom Stencil is uint8 rather than instance-unique, so same-ID self-occlusion is also unresolved. The mask is therefore training-usable only when gated by validity and remains explicitly production-uncertified.

## Experimental FG isolation and assembly

Endpoint replay captures `t0, t1, ...` while suppressing player Main View submissions between endpoints. Before each later endpoint render, the subsystem supplies the last captured `USceneComponent` transforms through `FMotionVectorSimulation`. The retained Main View camera history and the object previous transforms therefore both span the endpoint interval. Per-frame metadata records `motionPreviousLogicalFrameId`, the time span in frames/seconds and the limited transform scope.

Intermediate replay runs in a separate process and allows exactly one captured offset frame. This prevents a previous intermediate from becoming the retained Main View history for a later sample. A dataset producer launches one such process per endpoint pair until a restartable world-state cache is implemented.

`AssembleFrameGenerationDataset.py` accepts only completed source captures whose v3 validation reports pass. It verifies engine, GPU, content, shader, streaming-residency and normalized CVar provenance; the sole intentional CVar difference is `r.MotionVectorSimulation` (`1` for endpoints, `0` for the intermediate). It verifies hashes again before copying each buffer through an atomic staging directory.

`ValidateFrameGenerationDataset.py` then checks the self-contained pair, paths, hashes, EXR shape/range/finiteness, midpoint time, camera matrices, exposure, endpoint history, experimental t1-to-t0 history rejection and provenance. A successful report is an integrity pass, not FG certification. The manifest continues to list reverse motion, UI RGBA, skeletal/WPO endpoint motion, production-certified bidirectional disocclusion and HUD-less in-world-UI validation as missing.
