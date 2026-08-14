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

/**
 * A serializable capture job. The same struct is accepted by Blueprint and by
 * the -SRDatasetJob=<json-file> command-line entry point.
 */
USTRUCT(BlueprintType)
struct SUPERRESOLUTIONDATASET_API FSRDatasetCaptureJob
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	FString JobName = TEXT("default");

	/** Optional Level Sequence. Autonomous gameplay is still advanced at the fixed frame rate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
	FSoftObjectPath Sequence;

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
