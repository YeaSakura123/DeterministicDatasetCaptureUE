#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "SRDatasetTypes.generated.h"

UENUM(BlueprintType)
enum class ESRDatasetLRMode : uint8
{
	DownsampleFromHR UMETA(DisplayName = "Downsample HR (paired ground truth)"),
	NativeRender UMETA(DisplayName = "Native low-resolution render")
};

UENUM(BlueprintType)
enum class ESRDatasetResizeFilter : uint8
{
	Box,
	Bilinear,
	CubicMitchell,
	Lanczos4
};

UENUM(BlueprintType)
enum class ESRDatasetCaptureState : uint8
{
	Idle,
	WarmingUp,
	Capturing,
	Completed,
	Failed,
	Cancelled
};

UENUM(BlueprintType)
enum class ESRDatasetReplayPass : uint8
{
	Standard,
	FrameGenerationEndpoints,
	FrameGenerationReverseEndpoints,
	FrameGenerationIntermediate
};

/**
 * Order of the isolated SceneCapture submissions around a logical sample.
 * Running the same job once per order is the capture-order invariance gate.
 */
UENUM(BlueprintType)
enum class ESRDatasetAuxiliaryCaptureOrder : uint8
{
	HighResolutionFirst,
	LowResolutionFirst
};

/**
 * Analytic motion contract used by the semantic fixture. LegacyCameraRelative
 * preserves the original camera-following chart; every other mode anchors the
 * chart in world space so camera and object motion can be isolated explicitly.
 */
UENUM(BlueprintType)
enum class ESRDatasetSemanticMotionScenario : uint8
{
	LegacyCameraRelative,
	Static,
	CameraOnly,
	ObjectOnly,
	Mixed
};

/**
 * A serializable capture job. The same struct is accepted by Blueprint and by
 * the -SRDatasetJob=<json-file> command-line entry point.
 */
USTRUCT(BlueprintType)
struct SUPERRESOLUTIONDATASET_API FSRDatasetCaptureJob
{
	GENERATED_BODY()

	/** spatial-sr-data-v1, or strict nr-sr-data-v2 followed by offline dataset admission. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	FString ContractVersion = TEXT("spatial-sr-data-v1");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	FString JobName = TEXT("default");

	/** Optional Level Sequence. Autonomous gameplay is still advanced at the fixed frame rate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	FSoftObjectPath Sequence;

	/** Explicit replay role; FG endpoint and intermediate passes are assembled separately. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	ESRDatasetReplayPass ReplayPass = ESRDatasetReplayPass::Standard;

	/** Optional package name guard, for example /Game/Maps/MyMap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	FString ExpectedMap;

	/** Uses the active Sequencer camera first, then this tag, then player camera 0 unless the deterministic override is enabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FName CameraActorTag;

	/** Spawn and lock the player Main View to an explicit camera for cross-process replay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool bUseDeterministicCameraTransform = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (EditCondition = "bUseDeterministicCameraTransform"))
	FVector DeterministicCameraLocationCm = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (EditCondition = "bUseDeterministicCameraTransform"))
	FRotator DeterministicCameraRotationDegrees = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (EditCondition = "bUseDeterministicCameraTransform", ClampMin = "5.0", ClampMax = "170.0"))
	float DeterministicCameraFOVDegrees = 90.0f;

	/** World-space translation added per logical frame relative to StartFrame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (EditCondition = "bUseDeterministicCameraTransform"))
	FVector DeterministicCameraTranslationPerLogicalFrameCm = FVector::ZeroVector;

	/** Inclusive source-frame interval in CaptureFrameRate time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frames", meta = (ClampMin = "0"))
	int32 StartFrame = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frames", meta = (ClampMin = "0"))
	int32 EndFrame = 239;

	/** Simulates every frame but only writes every Nth frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frames", meta = (ClampMin = "1"))
	int32 FrameStep = 1;

	/** Captured phase in [0, FrameStep); 0 preserves the original behavior. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frames", meta = (ClampMin = "0"))
	int32 CaptureFrameOffset = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frames", meta = (ClampMin = "1"))
	int32 CaptureFrameRateNumerator = 30;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frames", meta = (ClampMin = "1"))
	int32 CaptureFrameRateDenominator = 1;

	/** Deterministic settling ticks before StartFrame; these are not written. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Frames", meta = (ClampMin = "0"))
	int32 WarmupFrames = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FIntPoint HRResolution = FIntPoint(3840, 2160);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FIntPoint LRResolution = FIntPoint(960, 540);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	ESRDatasetLRMode LRMode = ESRDatasetLRMode::DownsampleFromHR;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	ESRDatasetResizeFilter ResizeFilter = ESRDatasetResizeFilter::CubicMitchell;

	/**
	 * Controls the actual isolated SceneCapture submission order. LowResolutionFirst
	 * is intended for the paired capture-order validation and requires NativeRender.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation")
	ESRDatasetAuxiliaryCaptureOrder AuxiliaryCaptureOrder = ESRDatasetAuxiliaryCaptureOrder::HighResolutionFirst;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bCaptureDepth = true;

	/**
	 * Temporarily assign deterministic, component-unique Custom Stencil IDs.
	 * ID 0 remains background. The encoding is uint8 and therefore rejects more
	 * than 255 identities instead of allowing collisions.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bAssignStableInstanceIds = false;

	/**
	 * Permit renderable components to appear or disappear after initial ID
	 * assignment. New component paths receive monotonically allocated IDs that
	 * are never reused; surviving or path-stable respawned components retain the
	 * same ID. The final mapping is propagated to every frame's metadata.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bAssignStableInstanceIds"))
	bool bAllowDynamicInstanceIdTopology = false;

	/**
	 * Experimental UE RDG extraction from the native LR view. Writes HDR Color,
	 * raw/full motion, device/linear depth and validity diagnostics. These files
	 * are not temporal-training certified until the validation suite passes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bCaptureTemporalDiagnostics = false;

	/** Capture diagnostics from the real player Main View instead of the LR SceneCapture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bCaptureTemporalDiagnostics"))
	bool bCaptureMainViewTemporalDiagnostics = false;

	/**
	 * Also extract the existing native-LR SceneCapture at the same linear-HDR
	 * AfterDOF stage. This is an isolated comparison input; it does not submit an
	 * additional render or replace the real Main View temporal modalities.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bCaptureMainViewTemporalDiagnostics"))
	bool bCaptureSceneCaptureLRComparison = false;

	/**
	 * Require the validator's jitter-aligned Main View versus SceneCapture LR
	 * pixel-domain gate. Restricted to the locked static semantic fixture so the
	 * comparison measures capture-path differences rather than scene motion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bCaptureSceneCaptureLRComparison"))
	bool bValidateMainViewSceneCapturePixelDomain = false;

	/**
	 * Render an isolated, non-jittered spatial supersample and downsample it to
	 * the HR output grid as a distinct linear-HDR reference target.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bCaptureTemporalDiagnostics"))
	bool bCaptureReferenceHR = false;

	/** Per-axis reference render scale. 2 means 4x as many source pixels. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (ClampMin = "2", ClampMax = "4", EditCondition = "bCaptureReferenceHR"))
	int32 ReferenceHRScale = 2;

	/** Fixed-time jitter samples on the isolated reference view: 1 (legacy), 16, 32 or 64. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bCaptureReferenceHR"))
	int32 ReferenceTemporalSamples = 1;

	/** Optional source-resolution EXRs for independently auditing reference accumulation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bCaptureReferenceHR"))
	bool bSaveReferenceSubsamples = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bCaptureReferenceHR"))
	ESRDatasetResizeFilter ReferenceResizeFilter = ESRDatasetResizeFilter::Lanczos4;

	/** Capture display-resolution Main View color after tonemap but before Slate/UI composition. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bCaptureMainViewTemporalDiagnostics"))
	bool bCaptureMainViewHUDlessColor = false;

	/**
	 * Capture the screen-space game UI layer independently on a transparent
	 * display-resolution target. RGB is premultiplied by straight coverage alpha;
	 * the scene/backbuffer is never used to infer alpha.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bCaptureMainViewHUDlessColor"))
	bool bCaptureUIColorAlpha = false;

	/** Relative paths resolve beneath the project directory. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FString OutputDirectory = TEXT("Saved/SRDataset/default");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bResume = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bWriteManifestEveryFrame = true;

	/** Compress/write owned CPU images on workers; only completed frames enter the manifest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bAsyncImageWrites = true;

	/** Bounds queued raw CPU image copies; backpressure never waits on the render thread. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (ClampMin = "16", ClampMax = "8192"))
	int32 MaxPendingImageWriteMB = 512;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	int32 RandomSeed = 1337;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bControlNiagara = true;

	/** Temporarily forces Niagara systems to deterministic, fixed-step mode in memory. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism", meta = (EditCondition = "bControlNiagara"))
	bool bForceNiagaraDeterminism = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bEnableChaosDeterminism = true;

	/** Removes view-dependent motion blur from all generated modalities. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bDisableMotionBlur = true;

	/** Disables eye adaptation so HR/LR/Main View use a stable exposure state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bLockExposure = true;

	/** Disables parallel/async render paths whose completion order can perturb offline captures. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bForceSynchronousRendering = true;

	/**
	 * Override every dataset SceneViewFamily's game time and signed delta from
	 * logical current/previous frame IDs, while freezing real time at a stable
	 * non-zero origin. This makes Time-driven materials and WPO independent of
	 * wall-clock time and replay direction without resetting temporal view state.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bLockMaterialTimeToLogicalFrame = true;

	/**
	 * Scan the prepared world before warmup and write scene_control_preflight.json.
	 * The report inventories ticking Actors/components, Niagara Data Interfaces,
	 * and material expressions whose values can vary independently of scene state.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Scene Control")
	bool bRunSceneControlPreflight = true;

	/** Fail the job when the scene-control preflight finds an unclassified source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Scene Control", meta = (EditCondition = "bRunSceneControlPreflight"))
	bool bRequireSceneControlPreflight = false;

	/**
	 * Require every SRDatasetControllable Actor to return a non-empty canonical
	 * state serialization before each selected capture. This closes the gap
	 * between receiving logical time and proving opaque internal state.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Scene Control")
	bool bRequireControllableState = false;

	/**
	 * Audited ticking Actor class paths. Rules are case-sensitive exact class
	 * paths; a single trailing '*' enables a prefix match.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Scene Control", meta = (EditCondition = "bRunSceneControlPreflight"))
	TArray<FString> SceneControlAllowedTickingActorClassPaths;

	/** Audited ticking component class paths; the same exact/trailing-'*' rule syntax applies. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Scene Control", meta = (EditCondition = "bRunSceneControlPreflight"))
	TArray<FString> SceneControlAllowedTickingComponentClassPaths;

	/** Audited Niagara Data Interface class paths; the same exact/trailing-'*' rule syntax applies. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Scene Control", meta = (EditCondition = "bRunSceneControlPreflight"))
	TArray<FString> SceneControlAllowedNiagaraDataInterfaceClassPaths;

	/** Audited material-expression class paths; the same exact/trailing-'*' rule syntax applies. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Scene Control", meta = (EditCondition = "bRunSceneControlPreflight"))
	TArray<FString> SceneControlAllowedMaterialExpressionClassPaths;

	/**
	 * Fail before and during capture if any visible registered UWidgetComponent
	 * exists. Screen-space Slate/UMG is captured separately; world/component UI
	 * must not silently remain in the HUD-less scene target.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bRejectVisibleWidgetComponents = true;

	/**
	 * Bake every registered skeletal component pose during a forward warmup
	 * prepass and replay those component-space bones by logical frame. This is
	 * the deterministic adapter for otherwise stateful AnimBP/gameplay poses.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bCacheSkeletalAnimationPosesForReplay = false;

	/** Optional shared pose-cache artifact to load before replay (project-relative or absolute). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism", meta = (EditCondition = "bCacheSkeletalAnimationPosesForReplay"))
	FString SkeletalPoseCacheInputFile;

	/** Optional shared pose-cache artifact written after the forward warmup bake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism", meta = (EditCondition = "bCacheSkeletalAnimationPosesForReplay"))
	FString SkeletalPoseCacheOutputFile;

	/**
	 * Capture or restore the canonical private state of every
	 * SRDatasetControllable Actor on every logical frame. The artifact contains
	 * the raw state strings, not only their hashes, and must therefore be handled
	 * with the same privacy policy as project gameplay saves.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|State Cache")
	bool bCacheControllableStatesForReplay = false;

	/** Portable state-cache artifact loaded and applied after Actor ticks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|State Cache", meta = (EditCondition = "bCacheControllableStatesForReplay"))
	FString ControllableStateCacheInputFile;

	/** Portable state-cache artifact written after a complete Standard replay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|State Cache", meta = (EditCondition = "bCacheControllableStatesForReplay"))
	FString ControllableStateCacheOutputFile;

	/**
	 * Record or replay every controlled Niagara component through UE's native
	 * full-attribute Sim Cache. The binary artifact contains particle/system
	 * buffers, including GPU readback data, and must be treated as project data.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Niagara Sim Cache")
	bool bCacheNiagaraSimForReplay = false;

	/** Native Niagara Sim Cache bundle loaded before deterministic replay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Niagara Sim Cache", meta = (EditCondition = "bCacheNiagaraSimForReplay"))
	FString NiagaraSimCacheInputFile;

	/** Native Niagara Sim Cache bundle written after a complete Standard replay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Niagara Sim Cache", meta = (EditCondition = "bCacheNiagaraSimForReplay"))
	FString NiagaraSimCacheOutputFile;

	/**
	 * Record or replay fixed-topology simulated UStaticMeshComponents through
	 * UE's native Chaos Cache adapter. The cache is authoritative for visible
	 * rigid-body transforms; velocity, constraints and full solver internals are
	 * deliberately outside this versioned contract.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Chaos Rigid Body Cache")
	bool bCacheChaosRigidBodyTransformsForReplay = false;

	/** Native Chaos rigid-body cache bundle loaded before deterministic replay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Chaos Rigid Body Cache", meta = (EditCondition = "bCacheChaosRigidBodyTransformsForReplay"))
	FString ChaosRigidBodyCacheInputFile;

	/** Native Chaos rigid-body cache bundle written after a complete Standard replay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism|Chaos Rigid Body Cache", meta = (EditCondition = "bCacheChaosRigidBodyTransformsForReplay"))
	FString ChaosRigidBodyCacheOutputFile;

	/**
	 * Bind the non-shipping Temporal AA/TSR jitter phase to logical frame ID.
	 * Required by FG replay so forward, reverse and intermediate processes sample
	 * the same logical frame on the same raster grid.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bLockTemporalJitterToLogicalFrame = false;

	/** Validation phase modulus. Eight is valid for UE TSR's base sequence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism", meta = (ClampMin = "1", ClampMax = "8", EditCondition = "bLockTemporalJitterToLogicalFrame"))
	int32 TemporalJitterSequenceLength = 8;

	/** Per-clip phase offset, normalized modulo TemporalJitterSequenceLength. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism", meta = (EditCondition = "bLockTemporalJitterToLogicalFrame"))
	int32 TemporalJitterPhaseOffset = 0;

	/** Block once after render warmup until currently requested engine streaming resources settle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism")
	bool bBlockOnStreamingBeforeCapture = true;

	/** Maximum streaming-barrier duration; a timeout fails the job instead of silently capturing partial residency. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Determinism", meta = (ClampMin = "0.1", EditCondition = "bBlockOnStreamingBeforeCapture"))
	float StreamingWaitSeconds = 60.0f;

	/**
	 * Experimental endpoint capture: do not submit a player Main View on
	 * simulated frames that are not selected by FrameStep/CaptureFrameOffset.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation")
	bool bSuppressMainViewOnUncapturedFrames = false;

	/**
	 * Override component previous transforms with the last captured endpoint.
	 * Forward endpoints use t0 as the prior transform at t1; reverse endpoints
	 * use t1 as the prior transform at t0. This covers rigid/component motion;
	 * skeletal bone/WPO endpoint motion is still uncertified.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bSuppressMainViewOnUncapturedFrames"))
	bool bUseLastCapturedEndpointTransforms = false;

	/**
	 * Replaces visible level geometry with a deterministic, camera-relative test
	 * chart containing moving, known-depth and translucent primitives. This is a
	 * validation fixture, not a production dataset option.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation")
	bool bEnableSemanticValidationFixture = false;

	/** Selects the analytic camera/object motion combination exercised by the semantic fixture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bEnableSemanticValidationFixture"))
	ESRDatasetSemanticMotionScenario SemanticMotionScenario = ESRDatasetSemanticMotionScenario::LegacyCameraRelative;

	/** Require a full logical jitter cycle with both signs on both axes and all four sign quadrants. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bEnableSemanticValidationFixture"))
	bool bValidateTemporalJitterSignCoverage = false;

	/** Spawn a renderable component on frame 1 and remove it on frame 2 to prove dynamic ID allocation/release. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bAllowDynamicInstanceIdTopology"))
	bool bValidateDynamicInstanceIdTopology = false;

	/**
	 * Spawn a private-state render probe whose process-local state differs before
	 * cache application, proving that replay restores rather than merely observes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bCacheControllableStatesForReplay"))
	bool bValidateControllableStateCache = false;

	/** Spawn CPU and GPU Niagara probes and require native Sim Cache record/apply evidence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bCacheNiagaraSimForReplay"))
	bool bValidateNiagaraSimCache = false;

	/** Spawn two colliding rigid bodies and require native Chaos Cache record/apply evidence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bCacheChaosRigidBodyTransformsForReplay"))
	bool bValidateChaosRigidBodyCache = false;

	/** Assign temporary Custom Stencil IDs to non-fixture skeletal components and require their production-scene pose/motion gate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bCacheSkeletalAnimationPosesForReplay"))
	bool bValidateNonFixtureSkeletalAnimation = false;

	/** Optional project-authored Actor/AnimBP probe spawned by the non-fixture skeletal gate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bValidateNonFixtureSkeletalAnimation"))
	FSoftClassPath NonFixtureSkeletalValidationActorClass;

	/** Apply a project-authored animated surface material to a labeled transient receiver and require logical-time replay evidence. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation")
	bool bValidateProjectAnimatedMaterial = false;

	/** Project-authored material or material instance used by the animated-material validation receiver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation", meta = (EditCondition = "bValidateProjectAnimatedMaterial"))
	FSoftObjectPath ProjectAnimatedMaterialValidationMaterial;

	/** Exit Unreal automatically after a command-line job completes or fails. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Automation")
	bool bAutoQuit = false;

	bool Validate(FString& OutError) const;
	double GetFixedDeltaSeconds() const;
};

USTRUCT(BlueprintType)
struct SUPERRESOLUTIONDATASET_API FSRDatasetCaptureStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Dataset")
	ESRDatasetCaptureState State = ESRDatasetCaptureState::Idle;

	UPROPERTY(BlueprintReadOnly, Category = "Dataset")
	int32 CurrentFrame = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Dataset")
	int32 CapturedSamples = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Dataset")
	int32 SkippedSamples = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Dataset")
	FString OutputDirectory;

	UPROPERTY(BlueprintReadOnly, Category = "Dataset")
	FString LastError;
};
