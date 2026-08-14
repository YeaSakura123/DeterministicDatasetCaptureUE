# Architecture and determinism contract

## Goal

The plugin treats dataset generation as an offline simulation rather than a screenshot utility. A sample is valid only when HR, LR, depth and metadata refer to the same evaluated world frame and repeated jobs can prove identical outputs by hash.

## Frame pipeline

```text
fixed engine tick
  -> pre-actor: evaluate Sequencer at absolute time
  -> pre-actor: discover/seed/seek Niagara components
  -> pre-actor: call custom deterministic drivers
  -> normal actor, animation, physics and component ticks
  -> post-actor: resolve the active camera
  -> capture HR
  -> derive LR from HR, or render native LR without ticking
  -> capture float SceneDepth without ticking
  -> atomically write files and manifest hashes
```

`FrameStep` only skips writes. Intermediate simulation frames are still evaluated so particles, animation events and physics do not jump over time.

## Control matrix

| Content | Control mechanism | Guarantee within one job | Replay requirement |
|---|---|---:|---|
| Sequencer transforms/camera/animation/events | continuous `SetPlaybackPosition(..., Play)` at absolute time | exact frame evaluation | author the content in the Level Sequence |
| Skeletal animation outside Sequencer | fixed engine delta | stable tick cadence | animation logic and inputs must themselves be deterministic |
| Legacy Cascade particles | fixed engine delta and seeded global random stream | stable sequential playback | use seeded particle modules for strict repeatability |
| Niagara CPU/GPU | Desired Age, fixed seek delta, forced solo, deterministic system seed/fixed tick | exact requested age; stable configured seed | data interfaces that read external/live state need a custom driver or Sim Cache |
| Chaos | fixed engine delta plus `p.Chaos.Solver.Deterministic=1` | enhanced deterministic ordering | platform, engine build and initial conditions must remain identical |
| Third-party/custom VFX and gameplay | `SRDatasetControllable` interface | whatever the implementation restores/evaluates | implement all three callbacks and use the supplied seed/time |
| HR/LR/Depth pairing | all captures happen in one post-actor callback before the next world tick | identical world state and camera transform | no custom render hook may mutate gameplay state |

“Absolute control” therefore means the plugin owns time and every participating system is either covered by a built-in driver or explicitly implements the control protocol. Unsupported live inputs are not silently certified as deterministic.

## Output contract

- Frame numbers are inclusive and expressed in the job's rational frame rate.
- Depth is `SCS_SceneDepth`, stored as floating-point OpenEXR; R is linear distance in Unreal centimeters. Sky/background and translucent-material behavior follow Unreal's SceneDepth semantics.
- PNG and EXR writes use a temporary `.part` file followed by an atomic rename.
- Resume skips a sample only if all required modality files exist; it recalculates their hashes into the new manifest.
- The manifest records the complete normalized job, engine version, state, pairing rule, camera pose and SHA-1 of each encoded file.

## Reproducibility acceptance test

For a candidate scene:

1. Delete or select two empty output roots.
2. Run the same job twice with the same cooked/editor build, RHI, GPU driver and content revision.
3. Compare the sorted `(frame, modality, sha1)` tuples in both manifests.
4. A mismatch is a determinism violation. Move the differing system into Sequencer, enable its native deterministic/cache mode, or implement `SRDatasetControllable`.

Cross-GPU bit-identical floating-point rendering is not guaranteed by Unreal. Training-set provenance should therefore also pin GPU model/driver, RHI, engine changelist, project commit and console-variable profile.
