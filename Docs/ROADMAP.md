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
- [x] Export deferred Main View world normal, linear Base Color, roughness, metallic, specular and exact GBuffer validity at the same pixel as temporal inputs.
- [x] Add monotonic uint8 component IDs for controlled dynamic spawn/remove topology with per-frame active/new sets and a schema-v2 first-seen mapping.
- [ ] Upgrade conservative dynamic disocclusion and uint8 component identity to production per-surface/wide identity; add material and semantic IDs.
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
- [x] Add deterministic dynamic component topology within the uint8 ceiling; IDs are never reused and path-stable respawns retain identity.
- [ ] Add per-surface/wider identity so same-component moving, WPO and animated-material disocclusion can be valid rather than conservatively rejected.
- [x] Add static/camera-only/object-only/mixed-motion and four-quadrant jitter-sign fixtures.
- [x] Add a hashed strict preflight scanner for every registered ticking Actor/component, loaded Niagara Data Interface and known time/per-instance/particle-random material input.
- [x] Let every `SRDatasetControllable` contribute a canonical opaque-state digest; strict jobs reject empty state before the selected render and include hashes in scene replay provenance.
- [x] Add a portable logical-frame state recorder/replayer for adapter-owned `SRDatasetControllable` gameplay and third-party VFX state, with post-tick apply plus byte-exact readback.
- [x] Add native fixed-topology Niagara CPU/GPU Sim Cache recording/replay with all-attribute capture, immediate GPU readback, exact topology/payload hashes and a dedicated two-process validator gate.
- [x] Add native fixed-topology Chaos `UStaticMeshComponent` rigid transform record/replay, exact logical-frame random access and a real colliding two-body fixture.
- [ ] Extend Chaos capture beyond visible rigid transforms to velocities, constraints, contacts/sleep/island state, Geometry Collections and full solver restart.

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
- This closes the scanner/evidence gate; version 0.14 later closes adapter-owned actor-state recording, while automatic Niagara Sim Cache, Chaos cache and project-authored WPO validation remain open.

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

## Version 0.12.0 same-pixel GBuffer and logical View-frame snapshot

- The AfterDOF Main View extraction writes world normal, linear material Base Color, roughness/metallic/specular and GBuffer validity from the exact same deferred input pixel used by HDR, motion, depth and identity. Forward Shading is rejected for temporal jobs.
- Logical jitter locking now uses UE's sequence-rendering `FSceneView::OverrideFrameIndexValue` and `OverrideOutputFrameIndexValue`; the GPU metadata proved indices `0,1` for logical frames `0,1` in two fresh processes instead of inheriting the 18/19 warmup submission count.
- Validator v17 requires GBuffer alpha/validity to match `depth_valid`, unit world normals, zero invalid samples and finite `[0,1]` Base Color/material attributes. UE compilation and 1/1 automation passed; stable-ID capture passed 474/474 standalone checks.
- A second clean process passed 735/735 replay checks. Depth/ID/masks/GBuffer validity remained exact; known sparse deferred quantization/raster boundary changes were bounded independently per attribute and produced heatmaps whenever encoded hashes differed.
- The moving/VFX semantic fixture passed 583/583, strict controllable-state preflight passed 508/508, the fixture-free `1920x1080`/`960x540` strict job passed 517/517, and validator-v17 retained v0.8 compatibility at 379/379.
- Dynamic per-surface identity, material/semantic IDs, Main View/reference-HR pixel-domain proof and automatic Niagara/Chaos/gameplay caches remain open; temporal certification is still disabled.

## Version 0.13.0 dynamic component-topology snapshot

- `bAllowDynamicInstanceIdTopology` keeps initial path-sorted IDs, assigns new component paths monotonically in discovery-frame/path order, never reuses removed IDs and rejects resume until an allocator journal exists.
- Schema-v2 `instance_id_map.json` adds `firstSeenLogicalFrame`, dynamic assignment policy and a final canonical SHA-1. Map growth retroactively updates every in-memory frame's global mapping reference, while frame-local active/new ID arrays remain immutable evidence.
- A real transient `UStaticMeshComponent` appeared on frame 1 and was destroyed on frame 2. The map grew from 60 to 61; active counts were `60 -> 61 -> 60`, new IDs were `[] -> [61] -> []`, and ID 61 appeared in the raster only while active.
- Validator v18 passed 694/694 standalone and 1099/1099 two-process replay checks. Fixed-topology regression remained 474/474 and retained v0.8 data passed 379/379.
- The carrier remains uint8 and component-scoped. More than 255 lifetime identities and same-component per-surface self-occlusion remain explicit open requirements.

## Version 0.14.0 controllable state-cache snapshot

- `DatasetApplyDeterministicState()` complements the existing canonical state getter. Cache recording stores every logical frame in a schema-v1 JSON artifact with raw payload, world-relative Actor/class identity, per-payload SHA-1/UTF-8 size and per-frame aggregate SHA-1.
- Replay applies state after Actor ticks and before render-data submission, rejects Actor topology/class drift or an unimplemented/failed apply callback, then requires byte-exact state readback. Recording is deliberately Standard-only and non-resumable.
- A validation Actor uses a process-local private value to move a visible cube. Both replay frames proved that state differed before apply, then matched the recorded payload and scene hash after apply.
- Validator v19 passed 93/93 for the record dataset, 93/93 for the replay dataset and 277/277 for the dedicated cross-process `state-cache` comparison, including HR/LR/depth.
- Ordinary manifests still contain only state hashes/byte counts. The explicit cache artifact contains raw project-private state and must follow save-data privacy policy. At this release the native Niagara GPU Sim Cache and Chaos solver caches remained open; v0.15 closes the declared Niagara path.

## Version 0.15.0 native Niagara CPU/GPU Sim Cache snapshot

- `bCacheNiagaraSimForReplay` records every logical frame into one `UNiagaraSimCache` per active component. The path uses all-attribute capture, immediate `FNiagaraDataSetReadback` for GPU emitters, no rebase/interpolation/extrapolation, no custom Data Interface storage and fixed component/system/emitter topology.
- The atomic schema-v1 binary binds exact engine, world, policy, frame range, rational rate and seed. Each sorted component record carries system identity, CPU/GPU emitter counts, cumulative particle samples, serialized payload SHA-1 and a bounded payload; replay rejects header, topology, asset, hash, frame-count or age drift before rendering.
- A generated UE 5.7 validation asset provides a real GPUComputeSim emitter alongside the CPU fixture. The two-frame recording captured 1,024 particles per frame, including 512 GPU particles; replay applied and verified both components on both frames from the same 287,461-byte artifact.
- Validator v20 passed 522/522 recording, 522/522 replay and 819/819 dedicated `niagara-cache` comparison checks. Existing semantic, dynamic-ID, fixed-ID and controllable-state gates retained 583/583, 694/694, 474/474 and 293/293 checks respectively.
- Repeating the live recording kept the CPU payload byte-identical but changed the GPU payload SHA-1 and a small set of translucent pixels within numeric bounds. The supported deterministic workflow therefore preserves one reviewed artifact and replays it; byte-identical GPUComputeSim re-recording is not claimed.
- This closes replay of one native fixed-topology CPU/GPU Niagara state for the exact no-custom-DI policy. External/live Data Interface resources, arbitrary topology, long-cache bulk serialization, byte-identical GPU re-simulation and Chaos solver state remain open.

## Version 0.16.0 native Chaos rigid-body transform snapshot

- `bCacheChaosRigidBodyTransformsForReplay` records each supported, simulating, fixed-topology `UStaticMeshComponent` through UE 5.7's native Chaos static-mesh adapter and adds an exact per-logical-frame reference-pose channel to the atomic schema-v1 `.srcache`.
- Replay requires exact engine/world/policy/rate/seed and Actor/component/class/static-mesh topology, random-accesses the native cache at the requested logical frame, verifies translation/rotation within `0.05 cm`/`0.05 degrees`, preserves fixed scale and applies the hash-validated reference pose before dataset render submission.
- The 18-frame validation fixture exercised two independently moving rigid bodies and 44 accumulated hit events. Both standalone datasets passed 499/499 validator-v21 checks; the dedicated `chaos-cache` comparison passed 2360/2360 with two components applied and verified on every frame.
- Depth was byte exact across record/replay. HR/LR color satisfied the numeric rendering contract but was not universally byte exact, which remains an explicit renderer-boundary observation rather than a relaxed Chaos state gate.
- This closes authoritative visible rigid transform replay for the declared native static-mesh/fixed-topology scope. Velocity, constraint/contact/sleep/island state, Geometry Collections, deformables and full solver restart remain open.
