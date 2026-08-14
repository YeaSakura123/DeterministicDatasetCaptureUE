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

This phase is certified only as `spatial-sr-data-v1`. Its FinalColorLDR PNG and SceneCapture depth outputs are not certified temporal-upscaler or frame-generation inputs.

## Phase 2 — production rendering (in progress)

- [x] UE 5.7 AfterDOF RDG extraction from native-LR SceneCapture and the real player Main View.
- [x] Decoded raw Velocity/coverage and depth-completed current-to-previous Motion in display pixels.
- [x] Raw reversed-Z device Depth, linear view Depth in meters and validity masks.
- [x] Current/previous jitter, exposure and complete jittered/unjittered camera matrices.
- [x] AfterDOF translucency, transparency/reactive masks and Custom Stencil Object ID.
- [x] Isolated non-jittered native HR and 2x-4x supersampled/downsampled reference HR.
- [x] After-tonemap, pre-Slate/UI display-resolution Main View color.
- [x] Logical-frame/render-submission IDs and per-frame engine/GPU/CVar/content/shader provenance.
- [ ] Complete pixel-domain Main View/native/reference comparison and capture-order invariance.
- [ ] Record effective Mip bias, fixed LOD/scalability state and a streaming-state hash.
- [ ] Export production disocclusion, instance ID, normals, albedo and material/semantic IDs.
- [ ] Add Movie Render Graph for temporal/spatial samples, path tracing and tiled ultra-high-resolution output.
- [ ] Add configurable camera calibration export and pluggable degradation chains.

## Phase 3 — replay certification (in progress)

- [x] Automated two-run hash/numeric comparison with color-difference heatmaps.
- [x] Known-distance 1/10/100 m depth and reversed-Z world/view reconstruction fixture.
- [x] Rigid object-motion direction/magnitude, reset, coverage and known occlusion/disocclusion tests.
- [x] Separate FG endpoint and real midpoint processes so `Sτ` cannot replace endpoint history.
- [x] Full-interval `motion_1_to_0` for rigid scene components with explicit scope metadata.
- [x] Atomic FG assembler plus an independent self-contained integrity validator.
- [ ] Capture `motion_0_to_1` independently; never infer it by negation.
- [ ] Capture UI Color/Alpha independently and validate zero UI residue in HUD-less color.
- [ ] Validate endpoint motion for skeletal bones, WPO, animated materials, Niagara and translucency.
- [ ] Produce production disocclusion/visibility masks from both endpoints.
- [ ] Add static/camera-only/mixed-motion and four-direction jitter-sign fixtures.
- [ ] Add a preflight scanner for every tickable actor, Niagara data interface and nondeterministic material input.
- [ ] Add Niagara Sim Cache, Chaos cache and an actor-state recorder for non-Sequencer gameplay.

The current assembler writes `nr-fg-data-v1` only as `experimental_uncertified`. Certification stays false until every unchecked mandatory FG item above is implemented and validated.

## Phase 4 — dataset operations

- Multi-map/multi-sequence job queue and sharding across workers.
- Per-frame retry journal, disk-space estimation and graceful crash recovery.
- Dataset index export for common PyTorch/WebDataset layouts.
- Automated quality gates for blank frames, invalid depth, exposure drift and HR/LR geometric alignment.

## Version 0.2.0 validation snapshot

- UE 5.7 Development Editor build: passed.
- Unreal job-validation automation: 1/1 passed, zero test warnings/errors.
- Standard semantic replay: 458/458 required checks.
- FG endpoint replay: 462/462 required checks.
- Isolated midpoint replay: 240/240 required checks.
- Assembled FG pair: 89/89 integrity checks, intentionally not certified.
