# Deterministic Dataset Capture for Unreal Engine

[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.7-0E1128?logo=unrealengine)](https://www.unrealengine.com/)
[![Release](https://img.shields.io/badge/release-0.17.0-blue)](SuperResolutionDataset.uplugin)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-blue)](#requirements)

Deterministic Dataset Capture is a UE runtime plugin for synchronized HR, LR, depth, motion, object-ID, transparency and VFX-aware training data. It owns the capture clock, evaluates supported scene systems at an explicit logical time, and writes machine-verifiable manifests rather than unrelated screenshots.

用于生成同步 HR、LR、深度、运动、Object ID、透明度及 VFX 训练数据的 Unreal Engine 插件。插件控制固定时间步与采集顺序，并用可验证清单证明每个模态对应同一个逻辑场景状态。

中文完整说明：[`Docs/USAGE_ZH.md`](Docs/USAGE_ZH.md)

**0.17.0 correctness update:** Standard temporal capture now requires consecutive frames and a fresh capture. Spatial reuse verifies a complete original manifest, matching configuration and original file hashes, and preserves the original provenance. History rejection uses the jittered input grids; real renderer camera cuts reset history. Validator v22 rejects legacy temporal masks as a current correctness result. The 960×540 → 1920×1080 controlled acceptance evidence, independent consumers and remaining M1 work are documented in [`Docs/TEMPORAL_ACCEPTANCE.md`](Docs/TEMPORAL_ACCEPTANCE.md). This update does not certify `nr-sr-data-v2` or FG.

## Certification status

Version 0.17.0 deliberately separates implemented output from certified training contracts:

| Scope | Status | Meaning |
|---|---|---|
| `spatial-sr-data-v1` | Certified | Same-state HR/LR PNG and SceneCapture depth baseline. |
| Main View temporal buffers | Experimental, validated fixture | Real AfterDOF HDR, motion, depth, jitter, matrices, masks, same-pixel deferred GBuffer attributes and reason-coded, validity-gated disocclusion/history rejection are captured. Cross-instance and static-depth decisions are independently reconstructed; uncertain dynamic same-instance pixels are rejected with validity zero. A same-stage Main View/native-LR SceneCapture pixel-domain gate passes, but `nr-sr-data-v2` remains disabled until every production gate passes. |
| FG forward/reverse endpoint plus intermediate replay | Experimental, uncertified | `motion_1_to_0` and `motion_0_to_1` come from independent processes and assemble with a real `tau=0.5` frame. Controlled skinning/WPO fixtures, project AnimBP replay, project animated-material logical time, bidirectional skeletal disocclusion, independent UI RGBA and zero visible `UWidgetComponent` residue have passed. The checked project has no project-authored WPO asset, so that external-content gate remains open. |
| Native Niagara CPU/GPU Sim Cache replay | Experimental, validated fixture | Fixed-topology systems record all particle attributes; GPU emitters use immediate readback. A portable binary binds engine/map/rate/seed plus exact component, system and CPU/GPU emitter topology, and replay verifies the attached cache and payload counts before rendering. Custom Data Interface storage is deliberately excluded and remains a preflight/project-adapter obligation. |
| Native Chaos rigid-body transform replay | Experimental, validated fixture | Fixed-topology simulated `UStaticMeshComponent` bodies record native `UChaosCache` transform tracks plus fixed component scale. Replay random-accesses the exact logical frame, checks native output within `0.05 cm`/`0.05 degrees`, applies the hash-validated reference pose before dataset renders and proves real collision/motion in a two-body fixture. Velocity, constraints, contacts, sleep state and full solver internals are not serialized. |

The plugin rejects `nr-sr-data-v2` and direct `nr-fg-data-v1` capture jobs. Missing fields are never filled with guesses. The experimental FG assembler emits `nr-fg-data-v1` only with `frameGenerationCertified=false` and an explicit `missingRequirements` list.

## What is captured

The spatial baseline writes:

```text
hr/                                  # display-referred FinalColorLDR PNG
lr/                                  # paired or native FinalColorLDR PNG
depth/                               # SceneCapture depth EXR, Unreal centimeters
manifest.json
instance_id_map.json                  # optional stable ID -> component/Actor/class map
```

The real Main View diagnostic path can additionally write:

```text
color_lr_scene_hdr/                    # LR AfterDOF, linear scene RGB, pre-exposed
color_lr_scene_capture_hdr/            # optional paired native-LR SceneCapture comparison
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
history_rejection_reason/           # integer reason code 0..8
disocclusion_mask/                  # exact explicit alias of history_rejection_mask
disocclusion_valid/                 # exact explicit alias of history_rejection_valid
disocclusion_reason/                # exact explicit alias of history_rejection_reason
translucency_after_dof_raw/
transparency_mask/
reactive_mask/
object_id/                             # Custom Stencil uint8; optional stable component IDs
normal_world/                           # deferred Main View world normal XYZ, A=valid
base_color_linear/                      # deferred material Base Color RGB, A=valid
material_properties/                    # R=roughness G=metallic B=specular A=valid
gbuffer_valid/                          # opaque finite-positive-depth validity
ui_color_alpha/                        # independent display-size Slate/UMG RGBA PNG
```

Motion follows this exact convention:

```text
previous_pixel = current_pixel + motion_current_to_previous
+X = right, +Y = down, unit = display pixel
```

Pixels without object Velocity are completed from depth and the unjittered current/previous camera transforms. The manifest records the original Velocity coverage so downstream code can distinguish both sources.

The experimental v2 history-rejection/disocclusion policy first compares motion-reprojected component identity, then uses previous-frame Reversed-Z depth for static/camera-only pixels. Reset, out-of-bounds and cross-instance changes are exact reject-valid decisions. Static depth can retain history or identify occlusion. Velocity-covered geometry with the same component ID cannot prove per-surface identity, so it is conservatively emitted as `reject=1, valid=0`; unlabeled moving geometry follows the same safe policy. `history_rejection_reason` records one of nine audited reasons, and validator v14 independently reconstructs every non-reset decision from the saved current/previous buffers. Always gate training or losses with the validity mask. Dynamic same-instance self-occlusion still needs a per-surface/wider identity solution before production certification.

## Scene control

- Fixed rational frame rate and stable global random seed.
- Continuous Level Sequence evaluation for transforms, camera cuts, skeletal animation, events and Sequencer-driven VFX.
- Niagara Desired Age, stable seed offsets, forced solo mode and temporary deterministic/fixed-step system configuration.
- Optional native Niagara Sim Cache recording/replay for fixed-topology CPU and GPU emitters. It captures all attributes, performs immediate GPU readback, binds exact component/system identity and verifies per-frame cached payload before any dataset render submission.
- Chaos enhanced determinism through `p.Chaos.Solver.Deterministic`, plus optional native fixed-topology `UStaticMeshComponent` rigid transform recording/replay through `ChaosCaching`.
- `SRDatasetControllable` Blueprint/C++ interface for gameplay and third-party systems.
- Optional portable per-logical-frame `SRDatasetControllable` state artifacts. Replay applies each canonical payload after Actor ticks, before render-data submission, then requires byte-exact state readback.
- Separate logical-frame and render-submission IDs; reference renders do not advance simulation.
- Endpoint-history injection for SceneComponent transforms and double-buffered skinned component-space bones.
- Portable skeletal pose-cache artifacts for exact project AnimBP forward/reverse endpoint replay.
- A shipped validation-only WPO material with explicit current/previous world offsets through UE's `PreviousFrameSwitch`; vertex-deformation velocity output is forced and recorded for temporal jobs.
- Logical-frame `FSceneViewFamily` game time with signed previous time for forward/reverse material animation; real time is frozen to keep real-time material inputs deterministic.
- Logical-frame `FSceneView::OverrideFrameIndexValue` and output-frame index when jitter locking is enabled, so base-pass material dithering and stochastic View consumers do not inherit process-uptime frame counts.
- Independent display-size Slate/UMG RGBA capture. Any visible registered world-space `UWidgetComponent` is rejected before and during capture so it cannot silently contaminate HUD-less color.
- Persistent, isolated SceneCapture view state for native/reference HR and the real player Main View history for temporal inputs.
- Atomic file/manifest writes, hashes, map guard, CVar provenance and unattended auto-exit.
- A pre-capture streaming barrier that fails on timeout, plus per-frame resident-texture counts and a sorted streaming-state SHA-1.
- Actual GPU View Uniform Mip bias for Main View, native HR, reference HR and HUD-less color; the validator independently reproduces UE's quantized automatic-bias formula.
- A per-frame scene-state SHA-1 over sorted Actor/component transforms, visibility/tick state, skeletal component-space bones, Niagara component time/seed controls and Cascade component state. The manifest also lists actors that tick without implementing `SRDatasetControllable`.
- A pre-warmup scene-control preflight that inventories every registered ticking Actor/component, loaded Niagara Data Interface, and known time/per-instance/particle-random material input. It writes a canonical SHA-1 report and can reject any unclassified record before frame zero.
- Optional stable instance IDs finalized after warmup and the streaming barrier. Fixed mode assigns collision-free Custom Stencil IDs in sorted component-path order and fails on topology drift. Dynamic mode monotonically allocates never-reused IDs to newly registered component paths, permits removal/path-stable respawn, retains a final hashed ID→component/Actor/class/first-seen map and publishes per-frame active/new ID sets. Both modes restore every prior stencil state and reject more than 255 identities.

“Absolute control” is an explicit protocol, not a claim that arbitrary live input becomes deterministic automatically. The plugin can cache and reapply evaluated skeletal poses and adapter-owned canonical gameplay/VFX state, lock ordinary game-time material expressions to the logical frame, explicitly drive supported Niagara systems, record/replay native fixed-topology CPU/GPU Sim Caches and authoritatively replay visible fixed-topology Chaos static-mesh transforms. Network traffic, audio-driven state, nondeterministic or custom-storage Niagara Data Interfaces, custom async work, Material Parameter Collections, Chaos velocities/constraints/contacts/sleep/full solver state and project-authored WPO without an explicit previous-frame contract still require a specialized adapter/cache and project validation. The included fixtures and project-asset probes prove the declared paths, not every possible asset.

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

The runner reads `EngineAssociation` from the project and resolves an installed Unreal Editor. Use `-Editor` for a source build or unregistered installation. When Main View capture is enabled, it launches an HR-sized offscreen viewport and sets the internal render fraction from LR/HR. It verifies that this run wrote a fresh manifest, prints the captured sample count, and returns exit code 1 when the manifest state is not `Completed`, even if Unreal itself loses an early startup failure code.

### Strict scene-control preflight

`bRunSceneControlPreflight=true` writes `scene_control_preflight.json` before warmup. Report-only mode is the default so an existing project can discover its gaps. Set `bRequireSceneControlPreflight=true` for production jobs; every unclassified ticking Actor/component, Niagara Data Interface or scanned material input then fails the job before frame zero.

Exception rules are case-sensitive class paths. They are exact unless the sole `*` is the final character after a non-empty class-name prefix, which makes the rule a prefix match. Module-wide forms such as `/Script/Engine.*` are rejected. Keep rules narrow and review each exception; an allowlist says that the project has audited the class, not that the plugin has invented a deterministic adapter for it.

The checked First Person strict fixture is [`Config/job.scene-control-preflight-validation.json`](Config/job.scene-control-preflight-validation.json). The negative fixture intentionally has no exceptions and must fail with zero samples:

```powershell
& '.\Plugins\DeterministicDatasetCaptureUE\Scripts\RunDatasetCapture.ps1' `
  -Map '/Game/FirstPerson/Lvl_FirstPerson' `
  -Job '.\Plugins\DeterministicDatasetCaptureUE\Config\job.scene-control-preflight-validation.json' `
  -Project '.\YourProject.uproject'
```

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

Validator v21 requires exact provenance and temporal/native-HR/reference-HR/HUD-less/UI/semantic/streaming metadata. It independently reconstructs the scene-control report, stable instance-map SHA-1 and v2 disocclusion reason grids, checks every count and exact allowlist/ID record, and enforces the requested zero-unclassified and fixed-topology gates. It also verifies the effective material texture Mip bias, logical material time, signed reverse-time delta, logical View State frame index, zero visible `UWidgetComponent` residue, native Niagara cache evidence, native Chaos cache artifact/topology/frame/collision/motion/error evidence and the optional Main View/SceneCapture pixel-domain contract. Geometry, depth, motion, validity, masks and IDs remain numerically exact. Color and the quantized deferred attributes use separate narrow numeric contracts and produce heatmaps whenever hashes differ; GBuffer validity itself remains exact.

### Main View / SceneCapture LR pixel-domain gate

Set `bCaptureSceneCaptureLRComparison=true` to extract `color_lr_scene_capture_hdr` from the already scheduled native-LR SceneCapture. This does not submit another render and does not advance the world. The real Main View still owns `color_lr_scene_hdr` and all temporal buffers. Each frame records independent exposure, jitter, projection and View metadata for both paths.

[`Config/job.main-view-scene-capture-pixel-domain-validation.json`](Config/job.main-view-scene-capture-pixel-domain-validation.json) also enables `bValidateMainViewSceneCapturePixelDomain`. The validator divides each image by its own pre-exposure, translates the non-jittered SceneCapture image by the Main View's recorded current render-pixel jitter, crops the interpolation border and applies a hard fixture threshold. The job is restricted to a deterministic Static semantic fixture with locked exposure, disabled motion blur and logical-frame jitter locking, so motion cannot masquerade as a capture-path difference.

The supplied validation job deliberately uses `512x288` HR and `256x144` LR for fast regression. Smoke and semantic jobs may be even smaller. These are test resolutions, not production defaults. [`Config/job.production-first-person-2x-strict.json`](Config/job.production-first-person-2x-strict.json) is a directly runnable two-frame First Person example at `1920x1080` HR and `960x540` LR, with stable IDs and strict scene-control preflight. [`Config/job.production-2x.json`](Config/job.production-2x.json) is the longer project-agnostic template; replace its map and audit settings before use.

### Stable instance-ID gate

Set `bAssignStableInstanceIds=true` on a native temporal job to replace ambiguous scene-authored stencil reuse with component-unique IDs for a fixed loaded topology. Assignment occurs only after renderer warmup and the streaming barrier. ID `0` is reserved for background; IDs `1..N` follow case-sensitive sorted component paths. `instance_id_map.json` records the component path, owner Actor path, Actor class and component class for every ID, plus a canonical SHA-1 that the validator independently reconstructs.

[`Config/job.stable-instance-id-validation.json`](Config/job.stable-instance-id-validation.json) exercises this mode. It intentionally fails if the renderable component set changes after assignment, if another system changes a stencil, or if more than 255 components require labels. It cannot be combined with validation fixtures that reserve stencil values. This closes collisions between mapped components. Dynamic same-component self-occlusion is now conservatively invalid instead of silently trusted. Dynamic spawn/despawn topology is supported by the mode below, but production-valid self-occlusion still needs a per-surface/wider encoding.

### Dynamic instance-ID topology gate

Set `bAllowDynamicInstanceIdTopology=true` together with `bAssignStableInstanceIds=true` when controlled gameplay may register or remove renderable components. Initial components keep path-sorted IDs. Each newly observed component path receives the next ID in discovery-frame/path order; IDs are never recycled, and a path-stable respawn retains its ID. Resume is deliberately rejected until the allocator has a persistent journal. `instance_id_map.json` schema v2 records `firstSeenLogicalFrame`, the final map hash is propagated to every frame, and each frame records `stableInstanceIdActiveIds` plus `stableInstanceIdNewIds`.

[`Config/job.dynamic-instance-id-validation.json`](Config/job.dynamic-instance-id-validation.json) creates a real `UStaticMeshComponent` on logical frame 1 and destroys it on frame 2. The checked run expanded the map from 60 to 61 identities and recorded active counts `60 -> 61 -> 60`, new-ID sets `[] -> [61] -> []`, and visible ID-61 pixels only on the middle frame. It passed 694/694 standalone and 1099/1099 two-process replay checks. The mode is dynamic component identity, not triangle/surface identity, and remains limited to 255 never-reused IDs per job.

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

Both endpoint passes enable `r.MotionVectorSimulation=1`. The forward pass supplies the last captured component transforms and skeletal poses, while the reverse pass starts at `t1` and supplies those future transforms/poses when it captures `t0`; the two motion fields are never derived from each other. A portable `.srcache` artifact proves exact project AnimBP pose application in both directions. Reverse Sequencer evaluation currently jumps to absolute logical frames; adapter-owned event state can use the v0.14 controllable-state cache, and fixed-topology Niagara CPU/GPU systems can use the v0.15 native Sim Cache. Other autonomous systems still require a dedicated cache/replay adapter. The intermediate role allows exactly one capture per process in v1, preventing an earlier intermediate from remaining in the retained View State.

FG jobs lock `r.TemporalAA.Debug.OverrideTemporalIndex` to a phase computed from the logical frame ID and set the renderer-supported Scene View frame/output-frame overrides to the full logical frame plus phase. This is a non-shipping diagnostic CVar path; use a Development/Debug capture build. The assembler additionally requires the actual forward/reverse jitter, camera, exposure, depth, Object ID and validity raster grids to match at each endpoint before it accepts the pair.

The assembler refuses unvalidated sources, mismatched engine/GPU/map/shader/CVar provenance, incorrect endpoint history, non-midpoint samples, missing files or hash mismatches. Its integrity validator prints `PASS (UNCERTIFIED)` until all declared FG requirements exist.

## Blueprint API

- `Start Dataset Capture`
- `Get Dataset Capture Status`
- `Cancel Dataset Capture`

Implement `SRDatasetControllable` for systems requiring explicit dataset-time evaluation:

- `DatasetPrepare(RandomSeed, FixedDeltaSeconds)`
- `DatasetEvaluateFrame(FrameNumber, TimeSeconds)`
- `DatasetGetDeterministicState()` — return a canonical serialization of opaque render-affecting state
- `DatasetApplyDeterministicState(CanonicalState)` — restore that serialization and return success
- `DatasetRestore()`

Actors spawned during capture are discovered and prepared before their first evaluation. Set `bRequireControllableState=true` together with strict scene-control preflight to reject a selected frame before rendering when any controllable returns an empty state. Ordinary manifests store only Actor path, SHA-1 and UTF-8 byte count.

To replay opaque state, enable `bCacheControllableStatesForReplay` and choose exactly one of `controllableStateCacheOutputFile` or `controllableStateCacheInputFile`. Recording writes every logical frame, including frames skipped by `FrameStep`. Loading requires the exact world-relative Actor path/class set, calls `DatasetApplyDeterministicState` after Actor ticks and before render-data submission, then rejects any non-byte-exact state readback. Cache recording is non-resumable so a partial allocator/state journal cannot masquerade as complete.

The cache artifact intentionally contains the raw canonical strings. Treat it like project save-game/private gameplay data; omit the cache option when hashes are sufficient. [`job.controllable-state-cache-record-validation.json`](Config/job.controllable-state-cache-record-validation.json) and [`job.controllable-state-cache-replay-validation.json`](Config/job.controllable-state-cache-replay-validation.json) demonstrate the two-process workflow. Validate the pair with:

```powershell
python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\controllable_state_cache_replay_validation' `
  --compare '.\Saved\SRDataset\controllable_state_cache_record_validation' `
  --compare-mode state-cache
```

### Native Niagara CPU/GPU Sim Cache replay

Enable `bCacheNiagaraSimForReplay` and choose exactly one of `niagaraSimCacheOutputFile` or `niagaraSimCacheInputFile`. The recording process captures every logical frame, including frames skipped by image `frameStep`; the replay process loads the artifact before scene control, requires the same world-relative component paths and exact Niagara system assets, attaches each cache and verifies its requested age and cached CPU/GPU particle payload after Niagara finalization and before rendering.

The cache uses `AttributeCaptureMode=All`, immediate GPU dataset readback, no rebasing, interpolation or extrapolation, and no custom Data Interface storage. The schema-v1 bundle binds the exact engine version, world, capture policy, inclusive frame range, rational rate, seed, component/system identity, emitter simulation target and serialized payload SHA-1. Recording is Standard-only and non-resumable. The current loader caps each serialized component payload at 1 GiB and disables large-cache bulk-data serialization, so long production shots should be split into bounded jobs. Any Niagara Data Interface that reads external state must still be classified by strict preflight and supplied with a project adapter; do not infer that the cache serializes arbitrary external resources.

Treat the recording pass as acquisition and the resulting `.srncache` as the authoritative simulation state. UE 5.7 GPUComputeSim execution is not promised to regenerate a byte-identical payload when the live GPU simulation is recorded again: in the supplied fixture, repeated CPU payloads were byte-identical while repeated live GPU payload SHA-1 values differed and a small number of translucent pixels moved within narrow numeric bounds. Deterministic production therefore persists one reviewed artifact and generates dataset frames through `niagaraSimCacheInputFile`; the dedicated comparison proves that exact artifact was attached and replayed. Deleting it and recording a fresh GPU simulation creates a new source state, not a deterministic replay.

The supplied two-frame CPU/GPU fixture workflow is intentionally low resolution for fast regression:

```powershell
& '.\Plugins\DeterministicDatasetCaptureUE\Scripts\RunDatasetCapture.ps1' `
  -Map '/Game/FirstPerson/Lvl_FirstPerson' `
  -Job '.\Plugins\DeterministicDatasetCaptureUE\Config\job.niagara-sim-cache-record-validation.json' `
  -Project '.\YourProject.uproject'

& '.\Plugins\DeterministicDatasetCaptureUE\Scripts\RunDatasetCapture.ps1' `
  -Map '/Game/FirstPerson/Lvl_FirstPerson' `
  -Job '.\Plugins\DeterministicDatasetCaptureUE\Config\job.niagara-sim-cache-replay-validation.json' `
  -Project '.\YourProject.uproject'

python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\niagara_sim_cache_replay_validation' `
  --compare '.\Saved\SRDataset\niagara_sim_cache_record_validation' `
  --compare-mode niagara-cache
```

### Native Chaos rigid-body transform replay

Enable `bCacheChaosRigidBodyTransformsForReplay` and choose exactly one of `chaosRigidBodyCacheOutputFile` or `chaosRigidBodyCacheInputFile`. The supported UE 5.7 scope is registered, simulating, fixed-topology `UStaticMeshComponent` bodies. Recording uses the native Chaos static-mesh cache adapter and writes each serialized `UChaosCache`, exact component/Actor/class/static-mesh identity, initial pose/fixed scale and a sorted reference pose for every logical frame into one atomic schema-v1 `.srcache`.

Replay requires exact engine, world, policy, frame range, rational rate, seed and component topology. It keeps the native kinematic pre-solve path, random-accesses the cache at `(frameIndex + 1) * fixedDelta`, verifies native translation/rotation within `0.05 cm`/`0.05 degrees`, preserves fixed scale and applies the artifact's exact hash-validated pose before any dataset render submission. Native cache key compression is valid: logical-frame count, not raw transform-key count, defines completeness.

This is authoritative visible rigid transform replay, not a full Chaos solver snapshot. The artifact explicitly declares velocity, constraint and full solver serialization false. Forces, linear/angular velocity, contacts, sleep/island state, Geometry Collections and deformables require a future engine-specific adapter.

The supplied 18-frame two-body collision workflow is intentionally low resolution for fast regression:

```powershell
& '.\Plugins\DeterministicDatasetCaptureUE\Scripts\RunDatasetCapture.ps1' `
  -Map '/Game/FirstPerson/Lvl_FirstPerson' `
  -Job '.\Plugins\DeterministicDatasetCaptureUE\Config\job.chaos-rigid-body-cache-record-validation.json' `
  -Project '.\YourProject.uproject'

& '.\Plugins\DeterministicDatasetCaptureUE\Scripts\RunDatasetCapture.ps1' `
  -Map '/Game/FirstPerson/Lvl_FirstPerson' `
  -Job '.\Plugins\DeterministicDatasetCaptureUE\Config\job.chaos-rigid-body-cache-replay-validation.json' `
  -Project '.\YourProject.uproject'

python '.\Plugins\DeterministicDatasetCaptureUE\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\chaos_rigid_body_cache_replay_validation' `
  --compare '.\Saved\SRDataset\chaos_rigid_body_cache_record_validation' `
  --compare-mode chaos-cache
```

## LR and HR choices

`DownsampleFromHR` captures HR once and creates LR on the CPU with Box, Bilinear, Cubic Mitchell or Lanczos4. It is the strictest spatial-pair baseline but does not reproduce renderer-dependent Mip/LOD behavior.

`NativeRender` uses independent persistent HR and LR render views at the same logical state. It preserves resolution-dependent Mips, LOD, shadows and temporal behavior and is required by the temporal diagnostic path.

`color_hr_reference_scene_hdr` first renders at `HRResolution * ReferenceHRScale`, then downsamples to the fixed non-jittered HR grid. For example, a 1920x1080 HR target with scale 2 renders internally at 3840x2160 but still writes a 1920x1080 EXR.

## Verified release evidence

Version 0.16.0 adds native fixed-topology Chaos static-mesh rigid transform recording/replay. The checked UE 5.7 D3D12 fixture recorded two moving bodies over 18 logical frames into a 13,006-byte `.srcache`; the native tracks contained one particle track each and legally compressed their transform keys to 13 and 18. Replay loaded the exact artifact SHA-1, applied and verified both bodies on all 18 frames, observed 44 accumulated hit events, and kept maximum native error below `0.000023 cm` translation and `0.000046 degrees` rotation. Validator v21 passed `499/499` for recording, `499/499` for replay and `2360/2360` for the dedicated `chaos-cache` comparison. Depth was byte exact; HR/LR color was within its narrow numeric rendering contract but not universally byte exact. Semantic capture retained `583/583`, and native Niagara record/apply regression retained `833/833`. This closes authoritative visible translation/rotation/fixed-scale replay for fixed-topology simulated `UStaticMeshComponent` bodies, not velocities, constraints, contacts, sleep/island state, Geometry Collections, deformables or full Chaos solver restart.

Version 0.15.0 adds native fixed-topology Niagara Sim Cache recording/replay. The checked UE 5.7 D3D12 fixture recorded one CPU and one GPU emitter across two logical frames. Each frame exposed 1,024 cached particles in total, including 512 GPU particles; the final component payloads contained 2,048 particle-frame samples, including 1,024 GPU samples. The 287,461-byte artifact carried a manifest/provenance SHA-1, and the replay process required that exact hash while applying and verifying both components on both frames. Validator v20 passed `522/522` for recording, `522/522` for replay and `819/819` for the dedicated record/apply comparison. HR, depth, motion, masks, IDs, GBuffer and scene/cache metadata satisfied their gates; two LR scene-HDR files differed bytewise but passed the narrow numeric color contract. A separate repeated-live-record diagnostic showed byte-identical CPU payloads but different GPU payload SHA-1 values, so the certified workflow persists one authoritative cache instead of promising deterministic regeneration of live GPU simulation. Regression datasets retained `583/583` semantic, `694/694` dynamic-ID, `474/474` stable-ID and `293/293` controllable-state record/apply checks. This closes replay of one recorded native CPU/GPU particle state for the declared fixed-topology/no-custom-DI policy, not Chaos solver caching, arbitrary external Data Interface state or byte-identical re-recording of GPUComputeSim.

Version 0.14.0 adds portable opaque-state replay for gameplay and third-party VFX adapters. A validation Actor deliberately derives its render transform from a process-local private value; the recording process wrote two schema-v1 canonical states, while the replay process proved one pre-apply mismatch, one successful application and one byte-exact readback on each frame. Both standalone datasets passed validator-v19 `93/93`, and the dedicated record/apply comparison passed `277/277`, including identical per-frame cache SHA-1, restored scene-state SHA-1, HR/LR and depth. This closes adapter-owned state replay, not Chaos internal-state caching.

Version 0.13.0 adds monotonic dynamic-topology component identity. A three-frame runtime fixture registered a visible component, assigned final ID 61, then destroyed it without reusing the ID; the final schema-v2 mapping hash and per-frame active/new sets were identical in two clean UE processes. Validator v18 passed 694/694 standalone and 1099/1099 replay checks. Fixed-topology v0.13 capture remained at 474/474, and retained v0.8 data still passed 379/379.

Version 0.12.0 adds same-pixel deferred Main View GBuffer supervision and logical View State frame locking. The AfterDOF extraction now writes world normal, linear material Base Color, roughness/metallic/specular and an exact opaque-validity raster from the same input pixel used by HDR, depth, motion and IDs; temporal jobs reject Forward Shading because those attributes are unavailable under that renderer contract. Validator v17 checks every attribute alpha against `gbuffer_valid`, requires validity to equal `depth_valid`, enforces unit world normals and finite `[0,1]` material ranges, and records heatmaps for cross-process differences. UE compilation and the 1/1 job-validation automation passed. The stable-ID job passed 474/474 standalone checks and 735/735 two-process replay checks; the moving/VFX semantic fixture passed 583/583, strict controllable-state preflight passed 508/508, and the fixture-free `1920x1080`/`960x540` strict job passed 517/517. Both replay runs reported logical View State indices `0,1`; all GBuffer/depth validity pixels were exact, while sparse quantized attribute-boundary differences stayed below the independently enforced per-modality limits. Validator-v17 backward compatibility retained the v0.8 dataset at 379/379.

Version 0.11.0 extends `SRDatasetControllable` with audited opaque-state provenance. `DatasetGetDeterministicState()` lets gameplay, third-party VFX or procedural systems contribute a canonical state serialization to the scene hash. Strict jobs reject empty contributions before rendering; frame metadata stores only the state SHA-1 and byte count. The transient semantic fixture supplies a changing canonical state and the strict scene-control job passed 462/462 validator-v16 checks with one controllable Actor, one state record, zero missing records and distinct hashes across its two logical frames. The same binary's fixture-free `1920x1080`/`960x540` strict capture passed 469/469 with zero uncontrolled preflight records and a valid empty controllable-state set.

Version 0.10.0 adds a deterministic training-distribution profile to validator v15. It reports per-frame and aggregate motion p50/p95, valid/uncertain disocclusion, reactive/transparency coverage, linear-depth range, pre-exposure-normalized luminance percentiles, edge/high-frequency content, exposure change and camera translation/angular speed. Reset frames are excluded from temporal distributions, and recommendations identify missing motion, reveal, VFX and camera-rotation coverage without weakening the integrity gate. The checked UE 5.7 datasets produced:

- strict fixture-free 1080p/540p example: 467/467 checks and a passing profile-integrity gate; its diagnostic correctly classified the only non-reset sample as static and requested motion/reveal/VFX/rotation coverage;
- moving/VFX semantic fixture: 535/535 checks; the non-reset frame fell in the fast `motion p95 >= 10 px` bucket, valid rejected-history coverage was 25.18%, and reactive coverage was approximately 5%;
- two independently captured stable-ID datasets: 424/424 checks each; geometry-derived profile metrics were exact, while small color-profile differences remained bounded by the existing numeric color replay policy.

Version 0.9.0 adds reason-coded, validity-gated disocclusion/history rejection with explicit output aliases and independent cross-frame reconstruction. The checked UE 5.7 runs produced:

- stable-ID two-frame validation: validator v14 passed 423/423 checks; on the non-reset 256x144 LR frame it independently reconstructed all 36,864 decisions with zero mask, validity or reason mismatch, including 21 instance-identity changes and 7,708 static-depth occlusions;
- moving semantic fixture: 534/534 checks, including all 134 newly revealed pixels rejected with valid evidence, 624 stable-background pixels retained, and 305 dynamic same-instance pixels conservatively marked reject-invalid;
- a second clean stable-ID process: 662/662 exact-replay checks, including the new mask/valid/reason EXRs and the unchanged 60-entry mapping SHA-1;
- the directly runnable strict production example captured two real `1920x1080` HR / `960x540` LR frames without a validation fixture and passed 466/466 checks, with zero uncontrolled preflight records, 60 stable IDs and both disocclusion/stable-ID gates passing;
- validator v14 backward compatibility: the retained v0.8 stable-ID dataset still passed its original 376/376 required checks.

Version 0.8.0 adds fixed-topology component-unique instance IDs and a hashed semantic map. The checked UE 5.7 runs produced:

- 60 deterministic component records with a mapping SHA-1 of `AC6D1AFC22F85CBC194939683025C0B9C88B5E19` in two independent processes;
- two-frame single-run validation: 376/376 required checks, with every nonzero `object_id` value resolving through the map;
- clean-process replay comparison: 599/599 checks, including byte-exact `object_id` EXRs for both frames and exact mapping/frame metadata;
- an initial pre-warmup prototype correctly failed at zero samples when topology grew from 59 to 60; assignment was therefore moved after warmup/streaming, while later drift remains a hard failure.

Version 0.7.0 adds the same-stage Main View/native-LR SceneCapture pixel-domain gate. The checked UE 5.7 run produced:

- Development Editor UHT/C++ build: 11/11 actions passed; Unreal automation job validation passed 1/1;
- two real First Person samples with no extra LR render submission or simulation advance: validator v12 passed 439/439 required checks;
- exact unjittered projection and View-matrix agreement; SceneCapture jitter was zero while Main View used two distinct subpixel jitter phases;
- after independent pre-exposure normalization and metadata-defined alignment: 24.24–24.27 dB PSNR and 2.40%–2.54% normalized mean absolute error, with the recorded jitter sign outperforming the opposite sign in both frames.

Version 0.6.0 adds the hashed scene-control preflight and explicit static/camera-only/object-only/mixed-motion plus four-quadrant jitter gates. The checked UE 5.7 runs produced:

- report-only smoke capture: 56/56 validator-v11 checks, with five plugin-owned SceneCapture components classified as controlled and all remaining project classes listed explicitly;
- strict positive preflight: two frames, 408/408 checks, 4 audited ticking Actors, 5 audited ticking components, 8 subsystem-controlled components and zero unclassified records;
- strict negative preflight: rejected before frame zero, wrote the violation report and failed the runner with exit code 1;
- Static, Camera-only, Object-only and Mixed semantic motion captures: 398/398, 398/398, 405/405 and 405/405 validator-v10 checks before the preflight evidence was added;
- one complete eight-phase jitter cycle: 1538/1538 validator-v10 checks with both signs on both axes and all four sign quadrants.

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

The fixture process recorded 81 actors, 105 components, three skeletal components and 180 component-space bones in the formal run. Its pure-skinning gate measured the analytic endpoint displacement within 0.02 display pixels. Its WPO object had 100% native velocity coverage in both endpoint directions: expected `-17.23077/+17.23077` pixels and measured `-17.23032/+17.23033` pixels. The project AnimBP probe produced 17–18 revealed/occluded pixels per direction and rejected every valid revealed pixel. The project material probe showed a maximum mean-RGB logical-time change of `0.421563`, while matching the same logical frame across opposite traversal directions within the numeric color gate. Seven ticking actors lacked `SRDatasetControllable`; v0.6.0 moved this audit to a dedicated pre-warmup report and made zero unclassified records a configurable hard gate. Native fixed-topology CPU/GPU Niagara payload readback is covered by v0.15, visible fixed-topology Chaos rigid transforms by v0.16; project-authored WPO coverage remains outside certification.

Five color files were not byte-identical in the standard replay, but remained inside the numeric gate (HUD-less PSNR at least 61.6 dB in that run). Depth, motion, validity, masks and IDs were exact. Cross-GPU or cross-driver bit identity is not promised.

In the capture-order pair, all 30 non-color modality/frame pairs were byte-exact. The order-sensitive Lumen/color outputs remained within the numeric gate; the worst HUD-less comparison was 59.1 dB PSNR. This closes the fixture-level capture-order gate, while non-fixture scene coverage and Main View/reference pixel-domain equivalence remain open.

For the bidirectional FG fixture, forward and reverse endpoint depth, Object ID, motion-validity and logical-frame jitter grids were byte-exact. At logical frame 0 the reverse pass measured approximately `(+71.116, +0.001)` display pixels against an analytic `(+71.111, 0)` expectation. The two independently captured motion fields were not pixelwise negations. Full scene-state hashes still differed between forward and reverse traversal because seven hidden ticking actors remain outside the control interface; the assembler allows that mismatch only for the explicit semantic fixture. It is a production blocker, not a relaxed production rule.

See [`Docs/ARCHITECTURE.md`](Docs/ARCHITECTURE.md) for the full state/control contract and [`Docs/ROADMAP.md`](Docs/ROADMAP.md) for the remaining certification work.

## Contributing

Issues and focused pull requests are welcome. Include the Unreal version, RHI/GPU, capture JSON, validation report and relevant log excerpt.

## License

Released under the [MIT License](LICENSE).
