# Deterministic Dataset Capture for Unreal Engine

[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.7-0E1128?logo=unrealengine)](https://www.unrealengine.com/)
[![Release](https://img.shields.io/badge/release-0.2.0-blue)](SuperResolutionDataset.uplugin)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-blue)](#requirements)

Deterministic Dataset Capture is a UE runtime plugin for synchronized HR, LR, depth, motion, object-ID, transparency and VFX-aware training data. It owns the capture clock, evaluates supported scene systems at an explicit logical time, and writes machine-verifiable manifests rather than unrelated screenshots.

用于生成同步 HR、LR、深度、运动、Object ID、透明度及 VFX 训练数据的 Unreal Engine 插件。插件控制固定时间步与采集顺序，并用可验证清单证明每个模态对应同一个逻辑场景状态。

## Certification status

Version 0.2.0 deliberately separates implemented output from certified training contracts:

| Scope | Status | Meaning |
|---|---|---|
| `spatial-sr-data-v1` | Certified | Same-state HR/LR PNG and SceneCapture depth baseline. |
| Main View temporal buffers | Experimental, validated fixture | Real AfterDOF HDR, motion, depth, jitter, matrices and masks are captured, but `nr-sr-data-v2` remains disabled until every production gate passes. |
| FG endpoint/intermediate replay | Experimental, uncertified | Independent endpoint and real `tau=0.5` replay can be assembled. Reverse endpoint motion, UI RGBA, skeletal/WPO endpoint validation and production disocclusion are still missing. |

The plugin rejects `nr-sr-data-v2` and direct `nr-fg-data-v1` capture jobs. Missing fields are never filled with guesses. The experimental FG assembler emits `nr-fg-data-v1` only with `frameGenerationCertified=false` and an explicit `missingRequirements` list.

## What is captured

The spatial baseline writes:

```text
hr/                                  # display-referred FinalColorLDR PNG
lr/                                  # paired or native FinalColorLDR PNG
depth/                               # SceneCapture depth EXR, Unreal centimeters
manifest.json
```

The real Main View diagnostic path can additionally write:

```text
color_lr_scene_hdr/                    # LR AfterDOF, linear scene RGB, pre-exposed
color_hr_native_scene_hdr/             # isolated non-jittered native HR
color_hr_reference_scene_hdr/          # 2x/4x spatial supersample -> fixed HR grid
color_main_view_hudless_after_tonemap/ # display-size, before Slate/UI
velocity_raw/
velocity_coverage/
motion_full_current_to_previous/       # display pixels, top-left origin
motion_valid/
depth_device_raw/
depth_view_linear_meters/
depth_valid/
translucency_after_dof_raw/
transparency_mask/
reactive_mask/
object_id/                             # Custom Stencil uint8; 0 = unlabeled
```

Motion follows this exact convention:

```text
previous_pixel = current_pixel + motion_current_to_previous
+X = right, +Y = down, unit = display pixel
```

Pixels without object Velocity are completed from depth and the unjittered current/previous camera transforms. The manifest records the original Velocity coverage so downstream code can distinguish both sources.

## Scene control

- Fixed rational frame rate and stable global random seed.
- Continuous Level Sequence evaluation for transforms, camera cuts, skeletal animation, events and Sequencer-driven VFX.
- Niagara Desired Age, stable seed offsets, forced solo mode and temporary deterministic/fixed-step system configuration.
- Chaos enhanced determinism through `p.Chaos.Solver.Deterministic`.
- `SRDatasetControllable` Blueprint/C++ interface for gameplay and third-party systems.
- Separate logical-frame and render-submission IDs; reference renders do not advance simulation.
- Persistent, isolated SceneCapture view state for native/reference HR and the real player Main View history for temporal inputs.
- Atomic file/manifest writes, hashes, map guard, CVar provenance and unattended auto-exit.

“Absolute control” is an explicit protocol, not a claim that arbitrary live input becomes deterministic automatically. Network traffic, audio-driven state, nondeterministic third-party data interfaces, custom async work, skeletal endpoint bone motion and WPO endpoint motion require an adapter, cache or additional validation before certification.

## Requirements

- Unreal Engine 5.7.
- Windows and a D3D12-capable GPU for the included PowerShell runner.
- A C++ Unreal project. A Blueprint-only project can add one empty C++ class to enable compilation.
- Python 3.10+ for offline validation.

The plugin is a Runtime module and has no editor-UI dependency.

## Installation

1. Copy this repository to `<YourProject>/Plugins/DeterministicDatasetCaptureUE`.
2. Regenerate project files or reopen the `.uproject`.
3. Enable **Super Resolution Dataset** under **Edit → Plugins** if needed.
4. Build the project for `Development Editor` or the intended runtime target.

The descriptor and C++ module remain named `SuperResolutionDataset`, so upgrades do not change module references.

## Production-oriented 2x template

[`Config/job.production-2x.json`](Config/job.production-2x.json) is the recommended starting point for a temporal-data experiment:

- LR input: `960x540` native Main View render;
- HR output grid: `1920x1080`;
- reference source render: `3840x2160`, downsampled with Lanczos4;
- 60 Hz fixed simulation, 64 warmup frames;
- locked exposure, disabled motion blur/dynamic resolution and synchronous rendering profile;
- all current temporal, semantic and HUD-less outputs enabled.

Change at least `expectedMap`, `jobName`, `outputDirectory`, frame range and optionally `sequence` before running it. It is a production-resolution template, not proof that the still-disabled temporal contract is certified.

The semantic and FG validation jobs intentionally use `256x144` HR and `128x72` LR. Those tiny images make render-hook failures visible within seconds; they are test probes, not recommended training samples and not a plugin limit.

## Command-line capture

From the Unreal project directory:

```powershell
& '.\Plugins\DeterministicDatasetCaptureUE\Scripts\RunDatasetCapture.ps1' `
  -Map '/Game/Maps/YourCaptureMap' `
  -Job '.\Plugins\DeterministicDatasetCaptureUE\Config\job.production-2x.json' `
  -Project '.\YourProject.uproject'
```

The runner reads `EngineAssociation` from the project and resolves an installed Unreal Editor. Use `-Editor` for a source build or unregistered installation. When Main View capture is enabled, it launches an HR-sized offscreen viewport and sets the internal render fraction from LR/HR.

## PIE console commands

These commands belong in Unreal's in-game console while PIE is running:

```text
SRDataset.Start D:/jobs/city_train.json
SRDataset.Status
SRDataset.Cancel
```

Do not enter `SRDataset.Start` in PowerShell or `cmd.exe`; use `RunDatasetCapture.ps1` there. Interactive PIE jobs do not auto-close the editor even when the JSON contains `bAutoQuit=true`.

## Validate a capture

Install the offline dependency set once:

```powershell
python -m pip install -r '.\Plugins\DeterministicDatasetCaptureUE\Scripts\requirements-validation.txt'
```

Validate formats, hashes, dimensions, finite values, depth/motion semantics, matrices and job-role invariants:

```powershell
python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\production_2x_template'
```

Run the same job in a second clean process and compare it:

```powershell
python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\run_b' `
  --compare '.\Saved\SRDataset\run_a'
```

The v3 validator requires exact provenance and temporal/native-HR/reference-HR/HUD-less/semantic metadata. Geometry, depth, motion, masks and IDs must be numerically exact; color permits a narrow documented floating-point tolerance and writes heatmaps when hashes differ.

## Semantic validation fixture

[`Config/job.semantic-validation.json`](Config/job.semantic-validation.json) replaces visible level geometry during the job with a camera-relative chart containing:

- a moving foreground object and a background object with distinct stencil IDs;
- known front surfaces at approximately 1 m, 10 m and 100 m;
- AfterDOF translucency;
- deterministic occlusion and disocclusion geometry.

The validator checks motion direction/magnitude, reset motion, Velocity coverage, integer object IDs, depth accuracy, reversed-Z reconstruction, transparency coverage, matrix alignment and replay repeatability. Original primitive visibility and the Custom Depth mode are restored when the job ends.

## Experimental frame-generation replay

Frame generation uses separate processes so the real intermediate frame never becomes the endpoint's previous Main View state:

```text
endpoint process:      t0 ------------ t1
intermediate process:          t0.5
assembler:             verified t0 / t0.5 / t1 pair
```

Run and validate the endpoint and one-intermediate jobs, then assemble them:

```powershell
python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\fg_endpoint_validation'

python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\fg_intermediate_validation'

python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\AssembleFrameGenerationDataset.py' `
  --endpoints '.\Saved\SRDataset\fg_endpoint_validation' `
  --intermediate '.\Saved\SRDataset\fg_intermediate_validation' `
  --output '.\Saved\SRDataset\fg_pair_001'

python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateFrameGenerationDataset.py' `
  '.\Saved\SRDataset\fg_pair_001'
```

The endpoint pass enables `r.MotionVectorSimulation=1` and supplies the last captured component transforms, so `motion_1_to_0` spans the complete endpoint interval. This scope is explicitly limited to rigid/component transforms. The intermediate role allows exactly one capture per process in v1, preventing an earlier intermediate from remaining in the retained View State.

The assembler refuses unvalidated sources, mismatched engine/GPU/map/shader/CVar provenance, incorrect endpoint history, non-midpoint samples, missing files or hash mismatches. Its integrity validator prints `PASS (UNCERTIFIED)` until all declared FG requirements exist.

## Blueprint API

- `Start Dataset Capture`
- `Get Dataset Capture Status`
- `Cancel Dataset Capture`

Implement `SRDatasetControllable` for systems requiring explicit dataset-time evaluation:

- `DatasetPrepare(RandomSeed, FixedDeltaSeconds)`
- `DatasetEvaluateFrame(FrameNumber, TimeSeconds)`
- `DatasetRestore()`

Actors spawned during capture are discovered and prepared before their first evaluation.

## LR and HR choices

`DownsampleFromHR` captures HR once and creates LR on the CPU with Box, Bilinear, Cubic Mitchell or Lanczos4. It is the strictest spatial-pair baseline but does not reproduce renderer-dependent Mip/LOD behavior.

`NativeRender` uses independent persistent HR and LR render views at the same logical state. It preserves resolution-dependent Mips, LOD, shadows and temporal behavior and is required by the temporal diagnostic path.

`color_hr_reference_scene_hdr` first renders at `HRResolution * ReferenceHRScale`, then downsamples to the fixed non-jittered HR grid. For example, a 1920x1080 HR target with scale 2 renders internally at 3840x2160 but still writes a 1920x1080 EXR.

## Verified release evidence

Version 0.2.0 was compiled as a UE 5.7 Development Editor plugin and exercised on Windows/D3D12 with an AMD Radeon RX 7900 XTX. The checked fixture produced:

- Unreal automation: 1/1 job-validation test passed with zero warnings/errors;
- standard semantic deterministic replay: 458/458 required checks;
- FG endpoint deterministic replay: 462/462 required checks;
- isolated FG intermediate replay: 240/240 required checks;
- assembled FG integrity: 89/89 checks, intentionally uncertified.

Five color files were not byte-identical in the standard replay, but remained inside the numeric gate (HUD-less PSNR at least 61.6 dB in that run). Depth, motion, validity, masks and IDs were exact. Cross-GPU or cross-driver bit identity is not promised.

See [`Docs/ARCHITECTURE.md`](Docs/ARCHITECTURE.md) for the full state/control contract and [`Docs/ROADMAP.md`](Docs/ROADMAP.md) for the remaining certification work.

## Contributing

Issues and focused pull requests are welcome. Include the Unreal version, RHI/GPU, capture JSON, validation report and relevant log excerpt.

## License

Released under the [MIT License](LICENSE).
