#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/Actor.h"
#include "SRDatasetViewExtension.h"
#include "SRDatasetTypes.h"
#include "SRDatasetCaptureRig.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

UCLASS(NotBlueprintable, Transient)
class ASRDatasetCaptureRig : public AActor
{
	GENERATED_BODY()

public:
	ASRDatasetCaptureRig();

	bool Configure(const FSRDatasetCaptureJob& Job, FString& OutError);
	void ApplyCameraView(const FMinimalViewInfo& View, bool bDisableMotionBlur, bool bLockExposure);
	void WarmupRenderState(const FSRDatasetCaptureJob& Job);
	const FMinimalViewInfo& GetLastCameraView() const { return LastCameraView; }
	const FSRDatasetTemporalFrameMetadata& GetLastTemporalMetadata() const { return LastTemporalMetadata; }
	const FSRDatasetTemporalFrameMetadata& GetLastNativeHRMetadata() const { return LastNativeHRMetadata; }
	const FSRDatasetTemporalFrameMetadata& GetLastReferenceHRMetadata() const { return LastReferenceHRMetadata; }
	const FSRDatasetTemporalFrameMetadata& GetLastHUDlessColorMetadata() const { return LastHUDlessColorMetadata; }
	FIntPoint GetLastHUDlessColorSize() const { return LastHUDlessColorSize; }
	bool CaptureFrame(
		const FSRDatasetCaptureJob& Job,
		int32 LogicalFrameNumber,
		int32 MotionPreviousLogicalFrameId,
		bool bHistoryReset,
		const FString& HRPath,
		const FString& LRPath,
		const FString& DepthPath,
		TMap<FString, FString>& OutHashes,
		FString& OutError);
	bool SaveTemporalCaptureResult(
		const FSRDatasetTemporalCaptureResult& Result,
		const FString& LRPath,
		int32 LogicalFrameNumber,
		int32 MotionPreviousLogicalFrameId,
		bool bHistoryReset,
		TMap<FString, FString>& OutHashes,
		FString& OutError);
	bool SaveNativeHDRColorResult(
		const FSRDatasetTemporalCaptureResult& Result,
		const FString& HRPath,
		TMap<FString, FString>& OutHashes,
		FString& OutError);
	bool SaveReferenceHDRColorResult(
		const FSRDatasetTemporalCaptureResult& Result,
		const FSRDatasetCaptureJob& Job,
		const FString& HRPath,
		TMap<FString, FString>& OutHashes,
		FString& OutError);
	bool SaveHUDlessColorResult(
		const FSRDatasetTemporalCaptureResult& Result,
		const FString& LRPath,
		TMap<FString, FString>& OutHashes,
		FString& OutError);

private:
	static bool SaveImageAtomic(const FString& Path, const TCHAR* Format, const FImage& Image, FString& OutHash, FString& OutError);
	static bool LoadScalarImage(const FString& Path, FIntPoint ExpectedSize, TArray<FLinearColor>& OutPixels);
	bool EnsurePreviousTemporalState(
		const FString& OutputRoot,
		int32 PreviousLogicalFrameId,
		FIntPoint ExpectedSize,
		FString& OutError);
	static FImageCore::EResizeImageFilter ToImageFilter(ESRDatasetResizeFilter Filter);
	void ApplyViewToCapture(USceneCaptureComponent2D* Capture, const FMinimalViewInfo& View, bool bDisableMotionBlur, bool bLockExposure);

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneCaptureComponent2D> HRCapture;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneCaptureComponent2D> LRCapture;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneCaptureComponent2D> ReferenceCapture;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneCaptureComponent2D> DepthCapture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> HRTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> LRTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ReferenceTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DepthTarget;

	FMinimalViewInfo LastCameraView;
	FSRDatasetTemporalFrameMetadata LastTemporalMetadata;
	FSRDatasetTemporalFrameMetadata LastNativeHRMetadata;
	FSRDatasetTemporalFrameMetadata LastReferenceHRMetadata;
	FSRDatasetTemporalFrameMetadata LastHUDlessColorMetadata;
	FIntPoint LastHUDlessColorSize = FIntPoint::ZeroValue;
	int32 PreviousTemporalLogicalFrameId = INDEX_NONE;
	FIntPoint PreviousTemporalSize = FIntPoint::ZeroValue;
	TArray<FLinearColor> PreviousTemporalDepth;
	TArray<FLinearColor> PreviousTemporalObjectId;
};
