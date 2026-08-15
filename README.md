# Deterministic Dataset Capture for Unreal Engine

[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.7-0E1128?logo=unrealengine)](https://www.unrealengine.com/)
[![Release](https://img.shields.io/badge/release-0.5.0-blue)](SuperResolutionDataset.uplugin)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-blue)](#requirements)

Deterministic Dataset Capture is a UE runtime plugin for synchronized HR, LR, depth, motion, object-ID, transparency and VFX-aware training data. It owns the capture clock, evaluates supported scene systems at an explicit logical time, and writes machine-verifiable manifests rather than unrelated screenshots.

用于生成同步 HR、LR、深度、运动、Object ID、透明度及 VFX 训练数据的 Unreal Engine 插件。插件控制固定时间步与采集顺序，并用可验证清单证明每个模态对应同一个逻辑场景状态。

## Certification status

Version 0.5.0 deliberately separates implemented output from certified training contracts:

| Scope | Status | Meaning |
|---|---|---|
| `spatial-sr-data-v1` | Certified | Same-state HR/LR PNG and SceneCapture depth baseline. |
| Main View temporal buffers | Experimental, validated fixture | Real AfterDOF HDR, motion, depth, jitter, matrices, masks and validity-gated history rejection are captured, but `nr-sr-data-v2` remains disabled until every production gate passes. |
| FG forward/reverse endpoint plus intermediate replay | Experimental, uncertified | `motion_1_to_0` and `motion_0_to_1` come from independent processes and assemble with a real `tau=0.5` frame. Controlled skinning/WPO fixtures, project AnimBP replay, project animated-material logical time, bidirectional skeletal disocclusion, independent UI RGBA and zero visible `UWidgetComponent` residue have passed. The checked project has no project-authored WPO asset, so that external-content gate remains open. |

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
depth_previous_reprojected_device/  # current surface reprojected into previous device-Z
history_rejection_mask/             # 1 = reject motion-reprojected previous history
history_rejection_valid/            # 1 = decision has reset/ID/depth evidence
translucency_after_dof_raw/
transparency_mask/
reactive_mask/
object_id/                             # Custom Stencil uint8; 0 = unlabeled
ui_color_alpha/                        # independent display-size Slate/UMG RGBA PNG
```

Motion follows this exact convention:

```text
previous_pixel = current_pixel + motion_current_to_previous
+X = right, +Y = down, unit = display pixel
```

Pixels without object Velocity are completed from depth and the unjittered current/previous camera transforms. The manifest records the original Velocity coverage so downstream code can distinguish both sources.

The experimental history-rejection mask uses motion-reprojected Custom Stencil identity where labels exist, then falls back to previous-frame Reversed-Z depth for static/camera-only pixels. Reset and out-of-bounds pixels are rejected exactly. Unlabeled Velocity-covered geometry is rejected conservatively with `history_rejection_valid=0`; always gate training or losses with the validity mask. Custom Stencil is uint8 rather than instance-unique, so same-ID self-occlusion remains unresolved. This is useful deterministic data, not yet production-certified disocclusion ground truth for skeletal, WPO or unlabeled moving geometry.

## Scene control

- Fixed rational frame rate and stable global random seed.
- Continuous Level Sequence evaluation for transforms, camera cuts, skeletal animation, events and Sequencer-driven VFX.
- Niagara Desired Age, stable seed offsets, forced solo mode and temporary deterministic/fixed-step system configuration.
- Chaos enhanced determinism through `p.Chaos.Solver.Deterministic`.
- `SRDatasetControllable` Blueprint/C++ interface for gameplay and third-party systems.
- Separate logical-frame and render-submission IDs; reference renders do not advance simulation.
- Endpoint-history injection for SceneComponent transforms and double-buffered skinned component-space bones.
- Portable skeletal pose-cache artifacts for exact project AnimBP forward/reverse endpoint replay.
- A shipped validation-only WPO material with explicit current/previous world offsets through UE's `PreviousFrameSwitch`; vertex-deformation velocity output is forced and recorded for temporal jobs.
- Logical-frame `FSceneViewFamily` game time with signed previous time for forward/reverse material animation; real time is frozen to keep real-time material inputs deterministic.
- Independent display-size Slate/UMG RGBA capture. Any visible registered world-space `UWidgetComponent` is rejected before and during capture so it cannot silently contaminate HUD-less color.
- Persistent, isolated SceneCapture view state for native/reference HR and the real player Main View history for temporal inputs.
- Atomic file/manifest writes, hashes, map guard, CVar provenance and unattended auto-exit.
- A pre-capture streaming barrier that fails on timeout, plus per-frame resident-texture counts and a sorted streaming-state SHA-1.
- Actual GPU View Uniform Mip bias for Main View, native HR, reference HR and HUD-less color; the validator independently reproduces UE's quantized automatic-bias formula.
- A per-frame scene-state SHA-1 over sorted Actor/component transforms, visibility/tick state, skeletal component-space bones, Niagara component time/seed controls and Cascade component state. The manifest also lists actors that tick without implementing `SRDatasetControllable`.

“Absolute control” is an explicit protocol, not a claim that arbitrary live input becomes deterministic automatically. The plugin can cache and reapply evaluated skeletal poses, lock ordinary game-time material expressions to the logical frame, and explicitly drive supported Niagara systems. Network traffic, audio-driven state, nondeterministic third-party data interfaces, custom async work, AnimBP logic that consumes external state, Material Parameter Collections and project-authored WPO without an explicit previous-frame contract still require an adapter, cache or project-specific validation. The included fixtures and project-asset probes prove the declared paths, not every possible asset.

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
- a 120-second streaming barrier so a production frame cannot silently use incomplete texture residency.

Change at least `expectedMap`, `jobName`, `outputDirectory`, frame range and optionally `sequence` before running it. It is a production-resolution template, not proof that the still-disabled temporal contract is certified.

The semantic and FG validation jobs intentionally use `256x144` HR and `128x72` LR. Those tiny images make render-hook failures visible within seconds; they are test probes, not recommended training samples and not a plugin limit.

For an immediately runnable production-resolution acceptance check on the First Person template map, use [`Config/job.formal-2x-validation.json`](Config/job.formal-2x-validation.json). It writes two synchronized frames with `1920x1080` HR/HUD-less/UI/main depth and `960x540` native LR/temporal inputs. The checked UE 5.7 run passed all 431 validator checks. This formal job disables the optional 4K supersampled reference view; enable `bCaptureReferenceHR` only when that extra ground truth is required.

```powershell
& '.\Plugins\DeterministicDatasetCaptureUE\Scripts\RunDatasetCapture.ps1' `
  -Map '/Game/FirstPerson/Lvl_FirstPerson' `
  -Job '.\Plugins\DeterministicDatasetCaptureUE\Config\job.formal-2x-validation.json' `
  -Project '.\YourProject.uproject'
```

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

The v9 validator requires exact provenance and temporal/native-HR/reference-HR/HUD-less/UI/semantic/streaming metadata. It verifies the effective material texture Mip bias, logical material time, signed reverse-time delta and zero visible `UWidgetComponent` residue. Geometry, depth, motion, masks and IDs must be numerically exact; color permits a narrow documented floating-point tolerance and writes heatmaps when hashes differ.

### Capture-order invariance gate

The paired validation jobs submit the same logical scene in two genuinely different orders:

```text
HighResolutionFirst: HR -> Reference -> LR -> Depth -> Main View
LowResolutionFirst:  LR -> Depth -> HR -> Reference -> Main View
```

Run both clean processes and the strict comparison with one command:

```powershell
& '.\Plugins\DeterministicDatasetCaptureUE\Scripts\RunCaptureOrderValidation.ps1' `
  -Project '.\YourProject.uproject' `
  -Map '/Game/Maps/YourCaptureMap'
```

The comparator requires opposite recorded submission orders, jobs equal except name/output/order, exact engine/GPU/content/shader/CVar/streaming provenance after removing only the capture-config hash, byte-exact non-color modalities and tolerance-gated color. `LowResolutionFirst` is rejected for `DownsampleFromHR`, because a CPU-derived LR image has no independent render submission to reorder.

## Semantic validation fixture

[`Config/job.semantic-validation.json`](Config/job.semantic-validation.json) replaces visible level geometry during the job with a camera-relative chart containing:

- a moving foreground object and a background object with distinct stencil IDs;
- known front surfaces at approximately 1 m, 10 m and 100 m;
- AfterDOF translucency;
- deterministic occlusion and disocclusion geometry.

The validator checks motion direction/magnitude, reset motion, Velocity coverage, integer object IDs, depth accuracy, reversed-Z reconstruction, transparency coverage, matrix alignment and replay repeatability. Original primitive visibility and the Custom Depth mode are restored when the job ends.

## Experimental frame-generation replay

Frame generation uses three separate processes so neither the real intermediate frame nor the opposite traversal can contaminate endpoint Main View history:

```text
forward endpoint:      t0 ------------> t1   captures motion_1_to_0 at t1
reverse endpoint:      t0 <------------ t1   captures motion_0_to_1 at t0
intermediate:                  t0.5           isolated real frame
assembler:             verified t0 / t0.5 / t1 pair with both motions
```

Run each supplied job through `RunDatasetCapture.ps1` in a fresh Unreal process. Then validate all three sources and assemble them:

```powershell
python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\fg_endpoint_validation'

python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\fg_reverse_endpoint_validation'

python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\fg_intermediate_validation'

python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\AssembleFrameGenerationDataset.py' `
  --endpoints '.\Saved\SRDataset\fg_endpoint_validation' `
  --reverse-endpoints '.\Saved\SRDataset\fg_reverse_endpoint_validation' `
  --intermediate '.\Saved\SRDataset\fg_intermediate_validation' `
  --output '.\Saved\SRDataset\fg_pair_001'

python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateFrameGenerationDataset.py' `
  '.\Saved\SRDataset\fg_pair_001'
```

Both endpoint passes enable `r.MotionVectorSimulation=1`. The forward pass supplies the last captured component transforms and skeletal poses, while the reverse pass starts at `t1` and supplies those future transforms/poses when it captures `t0`; the two motion fields are never derived from each other. A portable `.srcache` artifact proves exact project AnimBP pose application in both directions. Reverse Sequencer evaluation currently jumps to absolute logical frames, so opaque event-driven state still requires an adapter or cache. The intermediate role allows exactly one capture per process in v1, preventing an earlier intermediate from remaining in the retained View State.

FG jobs lock `r.TemporalAA.Debug.OverrideTemporalIndex` to a phase computed from the logical frame ID. This is a non-shipping diagnostic CVar; use a Development/Debug capture build. The assembler additionally requires the actual forward/reverse jitter, camera, exposure, depth, Object ID and validity raster grids to match at each endpoint before it accepts the pair.

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

Version 0.5.0 was compiled as a UE 5.7 Development Editor plugin and exercised on Windows/D3D12 with an AMD Radeon RX 7900 XTX. The checked captures produced:

- Unreal automation: 1/1 job-validation test passed with zero warnings/errors;
- fast semantic regression with independent UI and widget-residue policy: 461/461 required checks;
- formal `1920x1080` HR / `960x540` LR capture, including `1920x1080` SceneCapture depth: 431/431 required checks;
- FG forward endpoint replay: 469/469 required checks;
- independent reverse endpoint replay: 469/469 required checks;
- isolated FG intermediate replay: 247/247 required checks;
- real project AnimBP forward/reverse replay: 385/385 checks in each direction, including exact shared pose-cache application and bidirectional skeletal disocclusion;
- real project animated-material forward/reverse captures: 1279/1279 checks in each direction and 1452/1452 cross-direction comparison checks;
- assembled bidirectional FG integrity with portable project skeletal/material/UI evidence: 186/186 checks, intentionally uncertified only because the checked project contains no project-authored WPO asset.

The validation Main View used a 50% render fraction. Its GPU View Uniform reported a material texture Mip bias of `-1.296875`, exactly matching the independent formula and recorded CVar profile; both full-resolution isolated HR views reported `0`. The streaming barrier ended with 0 requests and 0 pending textures, and its 81-texture residency hash was stable across both semantic replay processes.

The known reveal fixture rejected all 174/174 revealed pixels with valid evidence and retained 734 stable background pixels. Both history-rejection EXRs were byte-exact across the two replay processes. This validates the declared scope; it does not extend production certification to unlabeled moving, skeletal or WPO geometry.

The fixture process recorded 81 actors, 105 components, three skeletal components and 180 component-space bones in the formal run. Its pure-skinning gate measured the analytic endpoint displacement within 0.02 display pixels. Its WPO object had 100% native velocity coverage in both endpoint directions: expected `-17.23077/+17.23077` pixels and measured `-17.23032/+17.23033` pixels. The project AnimBP probe produced 17–18 revealed/occluded pixels per direction and rejected every valid revealed pixel. The project material probe showed a maximum mean-RGB logical-time change of `0.421563`, while matching the same logical frame across opposite traversal directions within the numeric color gate. Seven ticking actors lacked `SRDatasetControllable`; their paths/classes are listed for audit rather than automatically treated as failures. GPU particle payload readback and project-authored WPO coverage remain outside this certification scope.

Five color files were not byte-identical in the standard replay, but remained inside the numeric gate (HUD-less PSNR at least 61.6 dB in that run). Depth, motion, validity, masks and IDs were exact. Cross-GPU or cross-driver bit identity is not promised.

In the capture-order pair, all 30 non-color modality/frame pairs were byte-exact. The order-sensitive Lumen/color outputs remained within the numeric gate; the worst HUD-less comparison was 59.1 dB PSNR. This closes the fixture-level capture-order gate, while non-fixture scene coverage and Main View/reference pixel-domain equivalence remain open.

For the bidirectional FG fixture, forward and reverse endpoint depth, Object ID, motion-validity and logical-frame jitter grids were byte-exact. At logical frame 0 the reverse pass measured approximately `(+71.116, +0.001)` display pixels against an analytic `(+71.111, 0)` expectation. The two independently captured motion fields were not pixelwise negations. Full scene-state hashes still differed between forward and reverse traversal because seven hidden ticking actors remain outside the control interface; the assembler allows that mismatch only for the explicit semantic fixture. It is a production blocker, not a relaxed production rule.

See [`Docs/ARCHITECTURE.md`](Docs/ARCHITECTURE.md) for the full state/control contract and [`Docs/ROADMAP.md`](Docs/ROADMAP.md) for the remaining certification work.

## Contributing

Issues and focused pull requests are welcome. Include the Unreal version, RHI/GPU, capture JSON, validation report and relevant log excerpt.

## License

Released under the [MIT License](LICENSE).
