#include "SRDatasetCaptureRig.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"

ASRDatasetCaptureRig::ASRDatasetCaptureRig()
{
	PrimaryActorTick.bCanEverTick = false;
	SetActorEnableCollision(false);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	HRCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("HRCapture"));
	HRCapture->SetupAttachment(SceneRoot);
	LRCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("LRCapture"));
	LRCapture->SetupAttachment(SceneRoot);
	DepthCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("DepthCapture"));
	DepthCapture->SetupAttachment(SceneRoot);

	for (USceneCaptureComponent2D* Capture : { HRCapture.Get(), LRCapture.Get(), DepthCapture.Get() })
	{
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = true;
		Capture->CompositeMode = SCCM_Overwrite;
	}

	HRCapture->CaptureSource = SCS_FinalColorLDR;
	LRCapture->CaptureSource = SCS_FinalColorLDR;
	DepthCapture->CaptureSource = SCS_SceneDepth;
}

bool ASRDatasetCaptureRig::Configure(const FSRDatasetCaptureJob& Job, FString& OutError)
{
	HRTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("SRDatasetHRTarget"));
	LRTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("SRDatasetLRTarget"));
	DepthTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("SRDatasetDepthTarget"));
	if (!HRTarget || !LRTarget || !DepthTarget)
	{
		OutError = TEXT("Failed to allocate render targets.");
		return false;
	}

	HRTarget->ClearColor = FLinearColor::Black;
	HRTarget->InitCustomFormat(Job.HRResolution.X, Job.HRResolution.Y, PF_B8G8R8A8, false);
	HRTarget->UpdateResourceImmediate(true);
	HRCapture->TextureTarget = HRTarget;

	LRTarget->ClearColor = FLinearColor::Black;
	LRTarget->InitCustomFormat(Job.LRResolution.X, Job.LRResolution.Y, PF_B8G8R8A8, false);
	LRTarget->UpdateResourceImmediate(true);
	LRCapture->TextureTarget = LRTarget;

	DepthTarget->ClearColor = FLinearColor::Black;
	DepthTarget->InitCustomFormat(Job.HRResolution.X, Job.HRResolution.Y, PF_A32B32G32R32F, true);
	DepthTarget->UpdateResourceImmediate(true);
	DepthCapture->TextureTarget = DepthTarget;

	OutError.Reset();
	return true;
}

void ASRDatasetCaptureRig::ApplyCameraView(const FMinimalViewInfo& View, const bool bDisableMotionBlur)
{
	LastCameraView = View;
	SetActorLocationAndRotation(View.Location, View.Rotation);
	ApplyViewToCapture(HRCapture, View, bDisableMotionBlur);
	ApplyViewToCapture(LRCapture, View, bDisableMotionBlur);
	ApplyViewToCapture(DepthCapture, View, bDisableMotionBlur);
}

void ASRDatasetCaptureRig::ApplyViewToCapture(USceneCaptureComponent2D* Capture, const FMinimalViewInfo& View, const bool bDisableMotionBlur)
{
	Capture->ProjectionType = View.ProjectionMode;
	Capture->FOVAngle = View.FOV;
	Capture->OrthoWidth = View.OrthoWidth;
	Capture->PostProcessSettings = View.PostProcessSettings;
	Capture->PostProcessBlendWeight = View.PostProcessBlendWeight;
	Capture->bUseCustomProjectionMatrix = false;
	if (bDisableMotionBlur)
	{
		Capture->PostProcessSettings.bOverride_MotionBlurAmount = true;
		Capture->PostProcessSettings.MotionBlurAmount = 0.0f;
		Capture->PostProcessSettings.bOverride_MotionBlurMax = true;
		Capture->PostProcessSettings.MotionBlurMax = 0.0f;
	}
}

bool ASRDatasetCaptureRig::CaptureFrame(
	const FSRDatasetCaptureJob& Job,
	const FString& HRPath,
	const FString& LRPath,
	const FString& DepthPath,
	TMap<FString, FString>& OutHashes,
	FString& OutError)
{
	FImage HRImage;
	HRCapture->CaptureScene();
	if (!FImageUtils::GetRenderTargetImage(HRTarget, HRImage))
	{
		OutError = TEXT("Failed to read the HR render target.");
		return false;
	}

	FString Hash;
	if (!SaveImageAtomic(HRPath, TEXT("png"), HRImage, Hash, OutError))
	{
		return false;
	}
	OutHashes.Add(TEXT("hr"), Hash);

	FImage LRImage;
	if (Job.LRMode == ESRDatasetLRMode::DownsampleFromHR)
	{
		LRImage.Init(Job.LRResolution.X, Job.LRResolution.Y, 1, HRImage.Format, HRImage.GammaSpace);
		FImageCore::ResizeImage(HRImage, LRImage, ToImageFilter(Job.ResizeFilter));
	}
	else
	{
		LRCapture->CaptureScene();
		if (!FImageUtils::GetRenderTargetImage(LRTarget, LRImage))
		{
			OutError = TEXT("Failed to read the LR render target.");
			return false;
		}
	}

	if (!SaveImageAtomic(LRPath, TEXT("png"), LRImage, Hash, OutError))
	{
		return false;
	}
	OutHashes.Add(TEXT("lr"), Hash);

	if (Job.bCaptureDepth)
	{
		FImage DepthImage;
		DepthCapture->CaptureScene();
		if (!FImageUtils::GetRenderTargetImage(DepthTarget, DepthImage))
		{
			OutError = TEXT("Failed to read the depth render target.");
			return false;
		}
		if (!SaveImageAtomic(DepthPath, TEXT("exr"), DepthImage, Hash, OutError))
		{
			return false;
		}
		OutHashes.Add(TEXT("depth"), Hash);
	}

	return true;
}

bool ASRDatasetCaptureRig::SaveImageAtomic(
	const FString& Path,
	const TCHAR* Format,
	const FImage& Image,
	FString& OutHash,
	FString& OutError)
{
	TArray64<uint8> Encoded;
	if (!FImageUtils::CompressImage(Encoded, Format, Image, 0))
	{
		OutError = FString::Printf(TEXT("Failed to encode image: %s"), *Path);
		return false;
	}

	const FString TempPath = Path + TEXT(".part");
	if (!FFileHelper::SaveArrayToFile(Encoded, *TempPath))
	{
		OutError = FString::Printf(TEXT("Failed to write image: %s"), *TempPath);
		return false;
	}
	if (!IFileManager::Get().Move(*Path, *TempPath, true, true, false, true))
	{
		IFileManager::Get().Delete(*TempPath, false, true, true);
		OutError = FString::Printf(TEXT("Failed to finalize image: %s"), *Path);
		return false;
	}

	OutHash = FSHA1::HashBuffer(Encoded.GetData(), Encoded.Num()).ToString();
	return true;
}

FImageCore::EResizeImageFilter ASRDatasetCaptureRig::ToImageFilter(const ESRDatasetResizeFilter Filter)
{
	switch (Filter)
	{
	case ESRDatasetResizeFilter::Box:
		return FImageCore::EResizeImageFilter::Box;
	case ESRDatasetResizeFilter::Bilinear:
		return FImageCore::EResizeImageFilter::Bilinear;
	case ESRDatasetResizeFilter::Lanczos4:
		return FImageCore::EResizeImageFilter::Lanczos4;
	case ESRDatasetResizeFilter::CubicMitchell:
	default:
		return FImageCore::EResizeImageFilter::CubicMitchell;
	}
}
