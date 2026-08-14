#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/Actor.h"
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
	void ApplyCameraView(const FMinimalViewInfo& View, bool bDisableMotionBlur);
	const FMinimalViewInfo& GetLastCameraView() const { return LastCameraView; }
	bool CaptureFrame(
		const FSRDatasetCaptureJob& Job,
		const FString& HRPath,
		const FString& LRPath,
		const FString& DepthPath,
		TMap<FString, FString>& OutHashes,
		FString& OutError);

private:
	static bool SaveImageAtomic(const FString& Path, const TCHAR* Format, const FImage& Image, FString& OutHash, FString& OutError);
	static FImageCore::EResizeImageFilter ToImageFilter(ESRDatasetResizeFilter Filter);
	void ApplyViewToCapture(USceneCaptureComponent2D* Capture, const FMinimalViewInfo& View, bool bDisableMotionBlur);

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneCaptureComponent2D> HRCapture;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneCaptureComponent2D> LRCapture;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneCaptureComponent2D> DepthCapture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> HRTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> LRTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DepthTarget;

	FMinimalViewInfo LastCameraView;
};
