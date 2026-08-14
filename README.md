# Deterministic Dataset Capture for Unreal Engine

[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.7-0E1128?logo=unrealengine)](https://www.unrealengine.com/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-blue)](#requirements)

A deterministic Unreal Engine plugin for synchronized HR, LR, scene-depth, animation, particle, and VFX dataset capture. It is designed for super-resolution training data today and extensible to additional computer-vision modalities later.

用于同步采集 HR、LR、场景深度、人物动画、粒子和 VFX 数据的确定性 Unreal Engine 插件。当前主要面向超分辨率训练集，也可扩展到更多计算机视觉模态。

## Why this plugin?

A useful training pair must describe the same scene state. Taking separate screenshots at different ticks produces mismatched character poses, particle states, camera transforms, physics, and temporal effects.

This plugin owns the capture clock and performs every modality capture after one evaluated world tick, before the world advances again:

```text
fixed engine tick
  -> evaluate Sequencer at absolute dataset time
  -> seed and seek Niagara to absolute age
  -> evaluate custom deterministic drivers
  -> run animation, physics, and component ticks
  -> resolve the active camera
  -> capture HR
  -> derive or render LR without ticking
  -> capture float SceneDepth without ticking
  -> atomically write files and manifest hashes
```

## Features

- Fixed rational frame rate and deterministic global random seed.
- Continuous Level Sequence evaluation for transforms, skeletal animation, camera cuts, events, and Sequencer-driven VFX.
- Niagara Desired Age control, stable seed offsets, forced solo execution, and temporary deterministic/fixed-step system configuration.
- Chaos enhanced determinism through `p.Chaos.Solver.Deterministic`.
- `SRDatasetControllable` Blueprint/C++ interface for custom gameplay and third-party VFX.
- Synchronized HR PNG, LR PNG, and floating-point SceneDepth EXR.
- LR generated from HR with Box, Bilinear, Cubic Mitchell, or Lanczos4 filters.
- Optional native low-resolution render for engine-specific LOD/TAA degradation.
- JSON jobs, command-line automation, PIE console commands, and Blueprint API.
- Atomic image/manifest writes, frame-level resume, camera calibration metadata, and SHA-1 content hashes.
- Safe map guard and automatic process exit for unattended workers.

## Requirements

- Unreal Engine 5.7.
- Windows and a rendering-capable GPU for the included PowerShell workflow.
- A C++ Unreal project. Blueprint projects can add an empty C++ class once to enable plugin compilation.

The source is intentionally kept as a Runtime module and does not depend on the editor UI.

## Installation

1. Copy this repository to `<YourProject>/Plugins/DeterministicDatasetCaptureUE`.
2. Regenerate project files or reopen the `.uproject`.
3. Enable **Super Resolution Dataset** in **Edit → Plugins** if Unreal does not enable it automatically.
4. Build the project for your target configuration.

The plugin descriptor remains named `SuperResolutionDataset.uplugin`, so existing projects can upgrade without changing module references.

## Quick start

Copy [`Config/job.example.json`](Config/job.example.json) and customize it. `expectedMap` prevents accidentally capturing the wrong level. A Level Sequence is optional:

```json
{
  "jobName": "city_train_001",
  "sequence": "/Game/Cinematics/LS_CityTrain.LS_CityTrain",
  "expectedMap": "/Game/Maps/City",
  "startFrame": 0,
  "endFrame": 299,
  "captureFrameRateNumerator": 30,
  "captureFrameRateDenominator": 1,
  "hRResolution": { "x": 3840, "y": 2160 },
  "lRResolution": { "x": 960, "y": 540 },
  "lRMode": "DownsampleFromHR",
  "bCaptureDepth": true,
  "outputDirectory": "Saved/SRDataset/city_train_001",
  "randomSeed": 1337,
  "bAutoQuit": true
}
```

Run an unattended offscreen capture from the project directory:

```powershell
& '.\Plugins\DeterministicDatasetCaptureUE\Scripts\RunDatasetCapture.ps1' `
  -Map '/Game/Maps/City' `
  -Job '.\Plugins\DeterministicDatasetCaptureUE\Config\city_train.json' `
	-Project '.\YourProject.uproject'
```

The script reads `EngineAssociation` from the `.uproject` and resolves registered Unreal installations automatically. Pass `-Editor` explicitly for source builds or unregistered engine installations.

## PIE console commands

```text
SRDataset.Start D:/jobs/city_train.json
SRDataset.Status
SRDataset.Cancel
```

`bAutoQuit` is honored only by unattended workers. A capture started from an interactive PIE console completes without closing the Unreal Editor.

## Blueprint API

- `Start Dataset Capture`
- `Get Dataset Capture Status`
- `Cancel Dataset Capture`

Implement the `SRDatasetControllable` interface for systems that require explicit dataset-time evaluation:

- `DatasetPrepare(RandomSeed, FixedDeltaSeconds)`
- `DatasetEvaluateFrame(FrameNumber, TimeSeconds)`
- `DatasetRestore()`

Actors spawned while capture is running are detected and prepared before their first evaluation.

## Output layout

```text
Saved/SRDataset/city_train_001/
├── hr/
│   └── frame_000000.png
├── lr/
│   └── frame_000000.png
├── depth/
│   └── frame_000000.exr
└── manifest.json
```

Depth uses Unreal's `SCS_SceneDepth`, stored in the R channel of a floating-point OpenEXR image. Values are linear distances in Unreal centimeters. The manifest records:

- normalized job configuration and Unreal Engine version;
- frame index and rational capture time;
- camera location, rotation, projection, FOV, aspect ratio, and focal length in pixels;
- relative modality paths and encoded-file SHA-1 hashes;
- capture status, resume state, and determinism contract.

## LR generation modes

### `DownsampleFromHR` — recommended for paired SR

The plugin captures HR once and generates LR on the CPU. This gives the strictest pixel correspondence and guarantees that LR generation does not advance or re-render the world.

### `NativeRender`

HR and LR use independent Scene Capture components and view histories, but both render before the next world tick. This preserves resolution-dependent LOD, material, and temporal behavior. It models engine-native degradation but is not mathematically equivalent to downsampling the HR image.

## Determinism contract

“Absolute control” means that capture time belongs to the plugin and every participating simulation is either controlled by a built-in adapter or explicitly implements the control interface.

Built-in coverage includes Sequencer, normal fixed-step actor/animation ticks, Niagara, Chaos, legacy seeded Cascade particles, camera selection, and all capture modalities. Live inputs, nondeterministic third-party systems, network traffic, audio-driven state, and custom asynchronous work cannot be silently certified. Route those systems through Sequencer, a native cache, or `SRDatasetControllable`.

For certification, run the same job twice under the same engine build, RHI, GPU/driver, content revision, and console-variable profile, then compare sorted `(frame, modality, sha1)` tuples from both manifests. Cross-GPU bit-identical floating-point rendering is not guaranteed by Unreal Engine.

See [`Docs/ARCHITECTURE.md`](Docs/ARCHITECTURE.md) for the complete control matrix and [`Docs/ROADMAP.md`](Docs/ROADMAP.md) for planned Movie Render Graph, normals, motion vectors, segmentation, replay certification, and distributed job support.

## Validation

The initial implementation was validated with:

- UE 5.7 Development Editor compilation and UnrealHeaderTool warnings-as-errors;
- an Unreal automation test for job validation;
- real D3D12 offscreen HR/LR/Depth capture on the First Person sample map;
- repeated-run byte comparison with identical SHA-256 output for all three modalities.

Use [`Config/job.smoke.json`](Config/job.smoke.json) as a minimal integration-test job after updating its map for your project.

## Contributing

Issues and focused pull requests are welcome. Please include the Unreal version, RHI, capture job, relevant log excerpt, and whether the issue reproduces with `DownsampleFromHR`.

## License

Released under the [MIT License](LICENSE).
