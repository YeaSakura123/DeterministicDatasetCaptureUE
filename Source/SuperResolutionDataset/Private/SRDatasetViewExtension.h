#pragma once

#include "CoreMinimal.h"
#include "RHIGPUReadback.h"
#include "SceneViewExtension.h"

enum class ESRDatasetViewCaptureStage : uint8
{
	AfterDOFTemporal,
	AfterTonemapColor
};

struct FSRDatasetTemporalFrameMetadata
{
	bool bValid = false;
	bool bCameraCut = false;
	float PreExposure = 1.0f;
	float OneOverPreExposure = 1.0f;
	FVector2f JitterCurrentNDC = FVector2f::ZeroVector;
	FVector2f JitterPreviousNDC = FVector2f::ZeroVector;
	float DeltaTimeSeconds = 0.0f;
	float GameTimeSeconds = 0.0f;
	float NearPlane = 0.0f;
	float OrthoFarPlane = 0.0f;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
	FMatrix44f ViewToClipJittered = FMatrix44f::Identity;
	FMatrix44f ViewToClipUnjittered = FMatrix44f::Identity;
	FMatrix44f PreviousViewToClip = FMatrix44f::Identity;
	FMatrix44f ClipToPreviousClipUnjittered = FMatrix44f::Identity;
	FMatrix44f ClipToPreviousClipJittered = FMatrix44f::Identity;
	FMatrix44f TranslatedWorldToViewCurrent = FMatrix44f::Identity;
	FMatrix44f ViewToTranslatedWorldCurrent = FMatrix44f::Identity;
	FMatrix44f TranslatedWorldToClipCurrentJittered = FMatrix44f::Identity;
	FMatrix44f ClipToTranslatedWorldCurrentJittered = FMatrix44f::Identity;
	FMatrix44f TranslatedWorldToViewPrevious = FMatrix44f::Identity;
	FMatrix44f ViewToTranslatedWorldPrevious = FMatrix44f::Identity;
	FMatrix44f TranslatedWorldToClipPreviousJittered = FMatrix44f::Identity;
	FMatrix44f PreviousViewToClipUnjittered = FMatrix44f::Identity;
	FMatrix44f TranslatedWorldToClipCurrentUnjittered = FMatrix44f::Identity;
	FMatrix44f TranslatedWorldToClipPreviousUnjittered = FMatrix44f::Identity;
	FVector3f WorldViewOriginHighCurrent = FVector3f::ZeroVector;
	FVector3f WorldViewOriginLowCurrent = FVector3f::ZeroVector;
	FVector3f PreViewTranslationHighCurrent = FVector3f::ZeroVector;
	FVector3f PreViewTranslationLowCurrent = FVector3f::ZeroVector;
	FVector3f WorldViewOriginHighPrevious = FVector3f::ZeroVector;
	FVector3f WorldViewOriginLowPrevious = FVector3f::ZeroVector;
	FVector3f PreViewTranslationHighPrevious = FVector3f::ZeroVector;
	FVector3f PreViewTranslationLowPrevious = FVector3f::ZeroVector;
	FIntPoint ViewRectMin = FIntPoint::ZeroValue;
	FIntPoint BufferSize = FIntPoint::ZeroValue;
	float ResolutionFraction = 1.0f;
	float InvResolutionFraction = 1.0f;
	float MaterialTextureMipBias = 0.0f;
	uint32 RenderFrameNumber = 0;
	uint32 StateFrameIndex = 0;
	uint32 StateFrameIndexMod8 = 0;
};

struct FSRDatasetTemporalCaptureResult
{
	FIntPoint Size = FIntPoint::ZeroValue;
	FIntPoint DisplaySize = FIntPoint::ZeroValue;
	TArray<FLinearColor> SceneColor;
	TArray<FLinearColor> VelocityRaw;
	TArray<FLinearColor> MotionFull;
	TArray<FLinearColor> Depth;
	TArray<FLinearColor> Translucency;
	TArray<FLinearColor> ObjectId;
	TArray<FLinearColor> WorldNormal;
	TArray<FLinearColor> BaseColor;
	TArray<FLinearColor> MaterialProperties;
	FSRDatasetTemporalFrameMetadata Metadata;
};

class FSRDatasetViewExtension final : public FSceneViewExtensionBase
{
public:
	FSRDatasetViewExtension(const FAutoRegister& AutoRegister, ESRDatasetViewCaptureStage InCaptureStage);

	bool RequestCapture(FIntPoint ExpectedSize, FIntPoint DisplaySize, bool bMainViewOnly, FString& OutError, bool bColorOnly = false);
	bool RequestTonemappedColorCapture(FIntPoint ExpectedSize, bool bMainViewOnly, FString& OutError);
	bool WaitAndTakeCapture(FSRDatasetTemporalCaptureResult& OutResult, FString& OutError);
	void CancelCapture();
	void SetDeterministicViewTime(double CurrentTimeSeconds, float DeltaTimeSeconds);
	void ClearDeterministicViewTime();
	void SetDeterministicViewFrameIndex(uint32 FrameIndex);
	void ClearDeterministicViewFrameIndex();

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;

	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass Pass,
		const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;

private:
	struct FPendingReadbacks
	{
		FIntPoint Size = FIntPoint::ZeroValue;
		FIntPoint DisplaySize = FIntPoint::ZeroValue;
		bool bCameraCut = false;
		bool bColorOnly = false;
		TUniquePtr<FRHIGPUTextureReadback> SceneColor;
		TUniquePtr<FRHIGPUTextureReadback> VelocityRaw;
		TUniquePtr<FRHIGPUTextureReadback> MotionFull;
		TUniquePtr<FRHIGPUTextureReadback> Depth;
		TUniquePtr<FRHIGPUTextureReadback> Translucency;
		TUniquePtr<FRHIGPUTextureReadback> ObjectId;
		TUniquePtr<FRHIGPUTextureReadback> WorldNormal;
		TUniquePtr<FRHIGPUTextureReadback> BaseColor;
		TUniquePtr<FRHIGPUTextureReadback> MaterialProperties;
		TUniquePtr<FRHIGPUTextureReadback> Metadata;
	};

	FScreenPassTexture CaptureAfterDOF_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
	FScreenPassTexture CaptureColorOnly_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);
	static void ReadFloatTexture(FRHIGPUTextureReadback& Readback, FIntPoint Size, TArray<FLinearColor>& OutPixels);
	static void DecodeMetadata(const TArray<FLinearColor>& Pixels, FSRDatasetTemporalFrameMetadata& OutMetadata);

	FCriticalSection StateMutex;
	ESRDatasetViewCaptureStage CaptureStage;
	bool bRequestPending = false;
	bool bRequestConsumed = false;
	FIntPoint RequestedSize = FIntPoint::ZeroValue;
	FIntPoint RequestedDisplaySize = FIntPoint::ZeroValue;
	bool bRequestedMainViewOnly = false;
	bool bRequestedColorOnly = false;
	bool bDeterministicViewTimeEnabled = false;
	double DeterministicCurrentTimeSeconds = 0.0;
	float DeterministicDeltaTimeSeconds = 0.0f;
	bool bDeterministicViewFrameIndexEnabled = false;
	uint32 DeterministicViewFrameIndex = 0;
	TUniquePtr<FPendingReadbacks> PendingReadbacks;
};

TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> GetSRDatasetViewExtension();
void SetSRDatasetViewExtension(TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> InExtension);
TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> GetSRDatasetTonemapViewExtension();
void SetSRDatasetTonemapViewExtension(TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> InExtension);
