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
- [x] Export experimental motion-reprojected history rejection/disocclusion mask, validity and reason codes, with independent reset/ID/static-depth reconstruction and conservative dynamic uncertainty.
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
- [x] Capture screen-space UI Color/Alpha independently and reject visible registered world-space `UWidgetComponent` residue in HUD-less color.
- [x] Validate a real project AnimBP through a shared portable pose cache in both endpoint directions.
- [x] Validate a real project-authored time-animated material at exact logical frames in both traversal directions.
- [x] Validate labeled project-skeletal reveal/occlusion evidence in both endpoint directions.
- [x] Validate Niagara and translucency fixture playback at endpoints and intermediate time.
- [ ] Validate a project-authored WPO asset with explicit current/previous endpoint motion; the checked project contains no suitable asset.
- [ ] Add per-surface/wider dynamic identity so same-component moving, WPO and animated-material disocclusion can be valid rather than conservatively rejected.
- [x] Add static/camera-only/object-only/mixed-motion and four-quadrant jitter-sign fixtures.
- [x] Add a hashed strict preflight scanner for every registered ticking Actor/component, loaded Niagara Data Interface and known time/per-instance/particle-random material input.
- [x] Let every `SRDatasetControllable` contribute a canonical opaque-state digest; strict jobs reject empty state before the selected render and include hashes in scene replay provenance.
- [ ] Add Niagara Sim Cache, Chaos cache and an actor-state recorder for non-Sequencer gameplay.

The current assembler writes `nr-fg-data-v1` only as `experimental_uncertified`. Certification stays false until every unchecked mandatory FG item above is implemented and validated.

## Phase 4 — dataset operations

- [ ] Multi-map/multi-sequence job queue and sharding across workers.
- [ ] Per-frame retry journal, disk-space estimation and graceful crash recovery.
- [ ] Dataset index export for common PyTorch/WebDataset layouts.
- [x] Deterministic per-frame/aggregate training-distribution profile for motion, disocclusion, reactive/transparency coverage, depth, luminance, frequency, exposure and camera speed.
- [ ] Turn selected profile thresholds into configurable production diversity gates; current recommendations are intentionally diagnostic.
- [ ] Automated quality gates for blank frames, invalid depth, exposure drift and HR/LR geometric alignment.

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

## Version 0.5.0 project replay, UI isolation and formal-resolution snapshot

- UE 5.7 Development Editor build passed on Windows/D3D12 with an AMD Radeon RX 7900 XTX.
- Fast semantic regression passed 461/461 validator-v9 checks with independent UI RGBA and zero visible registered `UWidgetComponent` residue.
- Formal two-frame capture produced real `1920x1080` HR/HUD-less/UI/main-depth files and `960x540` LR/temporal files; validation passed 431/431 checks.
- FG forward/reverse/intermediate captures passed 469/469, 469/469 and 247/247 checks.
- Real project AnimBP forward/reverse captures passed 385/385 checks in each direction and shared the exact same pose-cache artifact; 17-18 skeletal reveal/occlusion pixels per direction were validity-gated and rejected correctly.
- Real project animated-material forward/reverse captures passed 1279/1279 checks in each direction; the cross-direction logical-time/color comparison passed 1452/1452 checks.
- The self-contained assembled pair copied and revalidated all project evidence and passed 186/186 checks. Its sole remaining declared requirement is project-authored WPO endpoint-motion validation, so FG certification remains false.

## Version 0.6.0 motion isolation and scene-control preflight snapshot

- UE 5.7 Development Editor compilation passed after adding the scanner/report schema and validator-v11 contract.
- Static, Camera-only, Object-only and Mixed analytic motion captures passed 398/398, 398/398, 405/405 and 405/405 validator-v10 checks; the eight-phase jitter capture passed 1538/1538 with both signs on both axes and all four quadrants.
- Report-only smoke capture passed 56/56 validator-v11 checks and exposed every unclassified project Tick source without blocking migration.
- Strict First Person fixture captured two frames and passed 408/408 checks with zero unclassified records; its report contains 4 exact audited Actor classes, 5 audited ticking component instances and 8 subsystem-controlled components.
- The negative strict fixture stopped at zero samples, retained its violation report/failed manifest, and the runner returned exit code 1 based on manifest state.
- This closes the scanner/evidence gate, not the adapter gap: generalized actor-state recording, Niagara Sim Cache, Chaos cache and project-authored WPO validation remain open.

## Version 0.7.0 Main View / SceneCapture pixel-domain snapshot

- UE 5.7 Development Editor UHT/C++ compilation passed 11/11 actions; the automation job-validation suite passed 1/1.
- The existing native-LR render now optionally yields `color_lr_scene_capture_hdr`; this adds an RDG readback but no additional render submission or simulation/history advance.
- A locked Static First Person fixture captured two real frames and passed validator-v12 439/439 required checks.
- Main View and SceneCapture unjittered projection/View matrices matched exactly. After independent pre-exposure normalization and recorded-jitter alignment, the frames measured 24.24–24.27 dB PSNR and 2.40%–2.54% normalized mean absolute error; both selected alignment signs beat the opposite sign.
- This closes the Main View/native-LR SceneCapture same-stage gate. Main View/reference-HR equality, generalized production disocclusion, instance-unique IDs and project-authored WPO validation remain open.

## Version 0.8.0 fixed-topology stable instance-ID snapshot

- ID assignment is deferred until renderer warmup and the streaming barrier finish; later component-set or stencil-label drift aborts before the affected frame is committed.
- The First Person validation map contains 60 component-unique IDs plus background, with ID→component/Actor/class records and independently reconstructed SHA-1 `AC6D1AFC22F85CBC194939683025C0B9C88B5E19`.
- The single run passed validator-v13 376/376 checks; a second clean process retained the exact mapping hash and passed 599/599 replay checks, including byte-exact `object_id` EXRs.
- Prior Custom Depth/stencil state is restored on completion or failure. More than 255 instances, dynamic topology and same-component self-occlusion remain explicit hard limits rather than hidden collisions.

## Version 0.9.0 conservative disocclusion-v2 snapshot

- Adds exact `disocclusion_mask`, `disocclusion_valid` and `disocclusion_reason` aliases while preserving established history-rejection names.
- Nine integer reasons separate reset/input/OOB/identity/static-depth/dynamic-uncertain decisions. Dynamic same-instance and unlabeled motion are reject-invalid, never silently accepted.
- Validator v14 independently reconstructs all 36,864 non-reset pixels in the stable-ID test with zero differences; the run passed 423/423 checks and its second clean process passed 662/662 exact-replay checks.
- The moving semantic fixture passed 534/534 checks, including 134/134 newly revealed pixels rejected-valid and 305 dynamic same-instance pixels reject-invalid.
- The fixture-free strict First Person production example captured two `1920x1080` HR / `960x540` LR frames and passed 466/466 checks with zero uncontrolled preflight records and 60 stable IDs.
- Production-valid dynamic same-instance self-occlusion, dynamic topology and wider IDs remain open.

## Version 0.10.0 training-distribution profile snapshot

- Validator v15 records per-frame and aggregate motion p50/p95 buckets, disocclusion valid/uncertain ratios, reactive/transparency coverage, linear-depth range, pre-exposure-normalized luminance percentiles, edge/high-frequency content, exposure change and camera speed.
- Reset frames are excluded from temporal distributions. Profile integrity is a required gate; diversity recommendations remain diagnostic so a two-frame validation job is not mistaken for a balanced training corpus.
- The strict 1080p/540p dataset passed 467/467 and correctly reported its static/no-VFX coverage gaps. The moving/VFX semantic fixture passed 535/535, classified its non-reset sample as fast motion, measured 25.18% valid rejected history and about 5% reactive coverage.
- Independent stable-ID captures passed 424/424 each; geometry-derived profile values were exact, while color-profile drift stayed within the existing color replay tolerance.

## Version 0.11.0 controllable opaque-state snapshot

- `DatasetGetDeterministicState()` adds a canonical integration-owned state serialization to `SRDatasetControllable`; raw state is never written, only Actor path, SHA-1 and UTF-8 byte count.
- `bRequireControllableState` requires strict preflight and rejects an empty contribution before rendering a selected frame.
- The semantic fixture emits deterministic frame-varying state. The strict scene-control capture passed validator-v16 462/462 with one controllable, one digest record, zero missing records and different hashes at the two logical frames.
- The fixture-free strict `1920x1080`/`960x540` capture passed 469/469 with zero uncontrolled preflight records and a valid empty controllable-state set.
- This closes adapter-provided opaque-state provenance, not automatic Niagara Sim Cache, Chaos cache or arbitrary gameplay serialization.
