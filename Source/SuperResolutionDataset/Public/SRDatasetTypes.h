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
	FrameGenerationIntermediate
};

/**
 * A serializable capture job. The same struct is accepted by Blueprint and by
 * the -SRDatasetJob=<json-file> command-line entry point.
 */
USTRUCT(BlueprintType)
struct SUPERRESOLUTIONDATASET_API FSRDatasetCaptureJob
{
	GENERATED_BODY()

	/** Versioned semantic contract. v0.1.x only certifies spatial-sr-data-v1. */
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

	/** Uses the active Sequencer camera first, then this tag, then player camera 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FName CameraActorTag;

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bCaptureDepth = true;

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
	 * Render an isolated, non-jittered spatial supersample and downsample it to
	 * the HR output grid as a distinct linear-HDR reference target.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bCaptureTemporalDiagnostics"))
	bool bCaptureReferenceHR = false;

	/** Per-axis reference render scale. 2 means 4x as many source pixels. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (ClampMin = "2", ClampMax = "4", EditCondition = "bCaptureReferenceHR"))
	int32 ReferenceHRScale = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bCaptureReferenceHR"))
	ESRDatasetResizeFilter ReferenceResizeFilter = ESRDatasetResizeFilter::Lanczos4;

	/** Capture display-resolution Main View color after tonemap but before Slate/UI composition. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (EditCondition = "bCaptureMainViewTemporalDiagnostics"))
	bool bCaptureMainViewHUDlessColor = false;

	/** Relative paths resolve beneath the project directory. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FString OutputDirectory = TEXT("Saved/SRDataset/default");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bResume = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bWriteManifestEveryFrame = true;

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
	 * This covers rigid/component motion; skeletal bone/WPO endpoint motion is
	 * still uncertified and must not be inferred from this option alone.
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
