# Delivery roadmap

## Phase 1 — implemented baseline

- Runtime plugin isolated from the game module.
- Fixed-step world simulation and deterministic random seed.
- Continuous Level Sequence frame evaluation, including camera cuts and event traversal.
- Niagara Desired Age, seed, fixed-step and lifecycle restoration.
- Chaos enhanced determinism switch.
- Extensible Blueprint/C++ deterministic-control interface.
- Same-state HR, derived/native LR, float depth capture.
- JSON job, command-line automation, PIE console and Blueprint API.
- Atomic file writes, resumable frames, per-frame camera metadata and content hashes.

## Phase 2 — production rendering

- Add Movie Render Graph backend for temporal/spatial samples, path tracing and tiled ultra-high-resolution output.
- Add normals, albedo, motion vectors, optical flow, stencil/object instance IDs and semantic segmentation.
- Add configurable camera intrinsics/extrinsics and OpenCV-compatible calibration export.
- Add pluggable degradation chains: blur kernels, sensor noise, JPEG, chromatic aberration and exposure variants.

## Phase 3 — replay certification

- Add a preflight scanner that emits pass/fail findings for every tickable actor, Niagara system, live data interface and nondeterministic material input.
- Add Niagara Sim Cache and Chaos cache adapters for long VFX/destruction shots.
- Add an actor-state recorder for gameplay that cannot be authored in Sequencer.
- Add automated two-run hash comparison and mismatch localization by modality/component.

## Phase 4 — dataset operations

- Multi-map/multi-sequence job queue and sharding across workers.
- Per-frame retry journal, disk-space estimation and graceful crash recovery.
- Dataset index export for common PyTorch/WebDataset layouts.
- Automated quality gates for blank frames, invalid depth, exposure drift and HR/LR geometric alignment.
