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
- [ ] Complete pixel-domain Main View/native/reference comparison.
- [x] Prove auxiliary capture-order invariance with actual reversed submissions and a paired-process hard gate.
- [x] Record the GPU View Uniform Mip bias, fixed LOD/scalability CVar profile, streaming barrier and resident-texture state hash.
- [x] Export experimental motion-reprojected history rejection plus a per-pixel validity mask, with reset/ID/static-depth semantic tests.
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
- [x] Per-frame sorted scene-state hash covering Actor/components, skeletal bones, Niagara controls and controllable/ticking-actor audit lists.
- [x] Capture `motion_0_to_1` in an independent reverse-endpoint process; never infer it by negation.
- [x] Validate controlled pure-skinning endpoint motion by injecting double-buffered component-space bones in both directions.
- [x] Validate explicit `PreviousFrameSwitch` WPO endpoint motion in forward/reverse/midpoint processes and the assembled pair.
- [ ] Capture UI Color/Alpha independently and validate zero UI residue in HUD-less color.
- [ ] Validate non-fixture AnimBP, project WPO/animated materials, Niagara and translucency endpoint motion.
- [ ] Certify disocclusion/visibility for unlabeled moving, skeletal, WPO and animated-material geometry and capture both endpoint directions.
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

## Version 0.2.1 streaming/Mip validation snapshot

- UE 5.7 Development Editor build and job-validation automation: passed.
- Two-process semantic replay: 494/494 required checks.
- Streaming barrier: 0 requests after wait; 81 loaded textures; 0 pending textures in the validation scene.
- Main View at 50% render fraction: GPU View Uniform bias `-1.296875`, matching the independent formula using offset `-0.3` and quantization `1024`.
- Native/reference full-resolution SceneCapture views: GPU View Uniform bias `0`.
- FG endpoint/intermediate replays: 367/367 and 192/192 required checks; assembled pair: 90/90 integrity checks, still intentionally uncertified.

## Version 0.3.0 history-rejection validation snapshot

- UE 5.7 Development Editor build and job-validation automation: passed.
- Two-process semantic replay: 542/542 required checks; new rejection/validity EXRs were byte-exact.
- Known reveal geometry: 174/174 revealed pixels rejected with valid evidence; 734 stable background pixels retained.
- FG endpoint/intermediate replays: 403/403 and 209/209 required checks.
- Assembled FG pair: 102/102 integrity checks, still intentionally uncertified.

## Version 0.3.1 scene-state provenance snapshot

- Two-process semantic replay: 569/569 required checks.
- Captured scope: 81 actors, 99 components, two skeletal components and 178 component-space bones.
- Scene hashes changed between logical frames and matched exactly across processes.
- Seven ticking actors without `SRDatasetControllable` are listed by path/class for preflight audit.

## Version 0.3.2 capture-order invariance snapshot

- UE 5.7 Development Editor build passed; automation job validation passed 1/1 with zero test warnings/errors.
- Actual order A: `HR -> Reference -> LR -> Depth -> Main View`.
- Actual order B: `LR -> Depth -> HR -> Reference -> Main View`.
- Standalone order-A dataset: 406/406 required checks.
- Paired order comparison: 574/574 required checks.
- All 30 non-color modality/frame pairs were byte-exact; worst HUD-less color was 59.1 dB PSNR.
- FG endpoint/intermediate regressions passed 410/410 and 213/213 checks; the assembled pair passed 103/103 integrity checks and remains intentionally uncertified.
- This closes the deterministic semantic-fixture gate, not yet the required non-fixture production-scene coverage gate.

## Version 0.4.0 independent bidirectional-motion snapshot

- Capture-order regression passed 408/408 standalone and 578/578 paired checks after the jitter-lock provenance fields were added.
- Forward endpoint, reverse endpoint and isolated midpoint processes passed 415/415, 415/415 and 216/216 required checks.
- FG jobs lock temporal jitter phase to logical frame IDs; matching endpoint depth, Object ID, motion-validity and actual jitter grids were exact across traversal directions.
- Reverse frame 0 motion measured approximately `(+71.116, +0.001)` display pixels against the fixture's analytic `(+71.111, 0)` expectation.
- The assembler copies independently captured `motion_1_to_0`, `motion_0_to_1` and direction-specific history-rejection/validity buffers; the self-contained pair passed 127/127 integrity checks.
- Full scene-state hashes did not match between forward and reverse fixture traversal because hidden ticking actors remain uncontrolled. The exception is fixture-only; production assembly still rejects it.
- UI RGBA, skeletal/WPO/animated-material endpoint validation and production-certified bidirectional visibility remain open, so FG certification remains false.

## Version 0.4.1 skeletal and WPO endpoint snapshot

- UE 5.7 Development Editor build passed.
- Forward endpoint, reverse endpoint and isolated midpoint processes passed 428/428, 428/428 and 224/224 required checks.
- The subsystem caches every eligible double-buffered skinned component's component-space bones and supplies them as previous endpoint state; the controlled pure-skinning fixture matched analytic motion within 0.02 display pixels in both directions.
- The plugin now ships a generated validation material using explicit current/previous world offsets through UE's `PreviousFrameSwitch`, and temporal capture forces/records `r.Velocity.EnableVertexDeformation=1`.
- WPO native Velocity coverage was 100% in both directions. Expected `-17.23077/+17.23077` display pixels measured `-17.23032/+17.23033`.
- The self-contained FG pair re-measured both WPO fields and passed 131/131 integrity checks. Opposite auxiliary capture-order comparison passed 599/599 checks.
- UI RGBA, non-fixture AnimBP/project WPO/animated-material coverage and production-certified bidirectional visibility remain open, so FG certification remains false.
