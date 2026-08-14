#include "SRDatasetCaptureRig.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
	ReferenceCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("ReferenceCapture"));
	ReferenceCapture->SetupAttachment(SceneRoot);
	DepthCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("DepthCapture"));
	DepthCapture->SetupAttachment(SceneRoot);

	for (USceneCaptureComponent2D* Capture : {
		HRCapture.Get(), LRCapture.Get(), ReferenceCapture.Get(), DepthCapture.Get() })
	{
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = true;
		Capture->CompositeMode = SCCM_Overwrite;
	}

	HRCapture->CaptureSource = SCS_FinalColorLDR;
	LRCapture->CaptureSource = SCS_FinalColorLDR;
	ReferenceCapture->CaptureSource = SCS_FinalColorLDR;
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

	if (Job.bCaptureReferenceHR)
	{
		ReferenceTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("SRDatasetReferenceTarget"));
		if (!ReferenceTarget)
		{
			OutError = TEXT("Failed to allocate the reference render target.");
			return false;
		}
		ReferenceTarget->ClearColor = FLinearColor::Black;
		ReferenceTarget->InitCustomFormat(
			Job.HRResolution.X * Job.ReferenceHRScale,
			Job.HRResolution.Y * Job.ReferenceHRScale,
			PF_B8G8R8A8,
			false);
		ReferenceTarget->UpdateResourceImmediate(true);
		ReferenceCapture->TextureTarget = ReferenceTarget;
	}

	DepthTarget->ClearColor = FLinearColor::Black;
	DepthTarget->InitCustomFormat(Job.HRResolution.X, Job.HRResolution.Y, PF_A32B32G32R32F, true);
	DepthTarget->UpdateResourceImmediate(true);
	DepthCapture->TextureTarget = DepthTarget;

	OutError.Reset();
	return true;
}

void ASRDatasetCaptureRig::ApplyCameraView(
	const FMinimalViewInfo& View,
	const bool bDisableMotionBlur,
	const bool bLockExposure)
{
	LastCameraView = View;
	SetActorLocationAndRotation(View.Location, View.Rotation);
	ApplyViewToCapture(HRCapture, View, bDisableMotionBlur, bLockExposure);
	ApplyViewToCapture(LRCapture, View, bDisableMotionBlur, bLockExposure);
	ApplyViewToCapture(ReferenceCapture, View, bDisableMotionBlur, bLockExposure);
	ApplyViewToCapture(DepthCapture, View, bDisableMotionBlur, bLockExposure);
}

void ASRDatasetCaptureRig::WarmupRenderState(const FSRDatasetCaptureJob& Job)
{
	// Initialize each modality's persistent SceneCapture view state without
	// writing a training sample or advancing world simulation.
	HRCapture->CaptureScene();
	if (Job.LRMode == ESRDatasetLRMode::NativeRender)
	{
		LRCapture->CaptureScene();
	}
	if (Job.bCaptureReferenceHR)
	{
		ReferenceCapture->CaptureScene();
	}
	if (Job.bCaptureDepth)
	{
		DepthCapture->CaptureScene();
	}
}

void ASRDatasetCaptureRig::ApplyViewToCapture(
	USceneCaptureComponent2D* Capture,
	const FMinimalViewInfo& View,
	const bool bDisableMotionBlur,
	const bool bLockExposure)
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
	if (bLockExposure)
	{
		Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
		Capture->PostProcessSettings.AutoExposureMethod = AEM_Manual;
		Capture->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		Capture->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure = false;
		Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
		Capture->PostProcessSettings.AutoExposureBias = 0.0f;
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
	LastTemporalMetadata = FSRDatasetTemporalFrameMetadata();
	LastNativeHRMetadata = FSRDatasetTemporalFrameMetadata();
	LastReferenceHRMetadata = FSRDatasetTemporalFrameMetadata();
	LastHUDlessColorMetadata = FSRDatasetTemporalFrameMetadata();
	LastHUDlessColorSize = FIntPoint::ZeroValue;
	TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> ViewExtension;
	if (Job.bCaptureTemporalDiagnostics)
	{
		ViewExtension = GetSRDatasetViewExtension();
		if (!ViewExtension)
		{
			OutError = TEXT("The SRDataset RDG view extension is not available for native HR capture.");
			return false;
		}
		if (!ViewExtension->RequestCapture(Job.HRResolution, Job.HRResolution, false, OutError))
		{
			return false;
		}
	}

	FImage HRImage;
	HRCapture->CaptureScene();
	if (!FImageUtils::GetRenderTargetImage(HRTarget, HRImage))
	{
		OutError = TEXT("Failed to read the HR render target.");
		return false;
	}
	if (Job.bCaptureTemporalDiagnostics)
	{
		FSRDatasetTemporalCaptureResult NativeHRResult;
		if (!ViewExtension->WaitAndTakeCapture(NativeHRResult, OutError) ||
			!SaveNativeHDRColorResult(NativeHRResult, HRPath, OutHashes, OutError))
		{
			return false;
		}
	}
	if (Job.bCaptureReferenceHR)
	{
		const FIntPoint ReferenceSize(
			Job.HRResolution.X * Job.ReferenceHRScale,
			Job.HRResolution.Y * Job.ReferenceHRScale);
		if (!ViewExtension->RequestCapture(ReferenceSize, Job.HRResolution, false, OutError))
		{
			return false;
		}
		ReferenceCapture->CaptureScene();
		FSRDatasetTemporalCaptureResult ReferenceResult;
		if (!ViewExtension->WaitAndTakeCapture(ReferenceResult, OutError) ||
			!SaveReferenceHDRColorResult(ReferenceResult, Job, HRPath, OutHashes, OutError))
		{
			return false;
		}
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
		if (Job.bCaptureTemporalDiagnostics && !Job.bCaptureMainViewTemporalDiagnostics)
		{
			ViewExtension = GetSRDatasetViewExtension();
			if (!ViewExtension)
			{
				OutError = TEXT("The SRDataset RDG view extension is not available.");
				return false;
			}
			if (!ViewExtension->RequestCapture(Job.LRResolution, Job.HRResolution, false, OutError))
			{
				return false;
			}
		}

		LRCapture->CaptureScene();
		if (!FImageUtils::GetRenderTargetImage(LRTarget, LRImage))
		{
			OutError = TEXT("Failed to read the LR render target.");
			return false;
		}

		if (Job.bCaptureTemporalDiagnostics && !Job.bCaptureMainViewTemporalDiagnostics)
		{
			FSRDatasetTemporalCaptureResult TemporalResult;
			if (!ViewExtension->WaitAndTakeCapture(TemporalResult, OutError) ||
				!SaveTemporalCaptureResult(TemporalResult, LRPath, OutHashes, OutError))
			{
				return false;
			}
			LastTemporalMetadata = TemporalResult.Metadata;
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

bool ASRDatasetCaptureRig::SaveNativeHDRColorResult(
	const FSRDatasetTemporalCaptureResult& Result,
	const FString& HRPath,
	TMap<FString, FString>& OutHashes,
	FString& OutError)
{
	const int32 PixelCount = Result.Size.X * Result.Size.Y;
	if (PixelCount <= 0 || Result.SceneColor.Num() != PixelCount)
	{
		OutError = TEXT("RDG native HR color buffer has inconsistent dimensions.");
		return false;
	}

	FImage Image;
	Image.Init(Result.Size.X, Result.Size.Y, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	FMemory::Memcpy(Image.RawData.GetData(), Result.SceneColor.GetData(), Result.SceneColor.Num() * sizeof(FLinearColor));
	const FString OutputRoot = FPaths::GetPath(FPaths::GetPath(HRPath));
	const FString FrameName = FPaths::ChangeExtension(FPaths::GetCleanFilename(HRPath), TEXT("exr"));
	const FString Path = FPaths::Combine(OutputRoot, TEXT("color_hr_native_scene_hdr"), FrameName);
	FString Hash;
	if (!SaveImageAtomic(Path, TEXT("exr"), Image, Hash, OutError))
	{
		return false;
	}
	OutHashes.Add(TEXT("color_hr_native_scene_hdr"), Hash);
	LastNativeHRMetadata = Result.Metadata;
	return true;
}

bool ASRDatasetCaptureRig::SaveReferenceHDRColorResult(
	const FSRDatasetTemporalCaptureResult& Result,
	const FSRDatasetCaptureJob& Job,
	const FString& HRPath,
	TMap<FString, FString>& OutHashes,
	FString& OutError)
{
	const FIntPoint ExpectedSize(
		Job.HRResolution.X * Job.ReferenceHRScale,
		Job.HRResolution.Y * Job.ReferenceHRScale);
	const int32 PixelCount = Result.Size.X * Result.Size.Y;
	if (Result.Size != ExpectedSize || PixelCount <= 0 || Result.SceneColor.Num() != PixelCount)
	{
		OutError = TEXT("RDG reference HR color buffer has inconsistent dimensions.");
		return false;
	}

	FImage Supersampled;
	Supersampled.Init(Result.Size.X, Result.Size.Y, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	FMemory::Memcpy(
		Supersampled.RawData.GetData(),
		Result.SceneColor.GetData(),
		Result.SceneColor.Num() * sizeof(FLinearColor));
	FImage Reference;
	Reference.Init(
		Job.HRResolution.X,
		Job.HRResolution.Y,
		1,
		ERawImageFormat::RGBA32F,
		EGammaSpace::Linear);
	FImageCore::ResizeImage(Supersampled, Reference, ToImageFilter(Job.ReferenceResizeFilter));

	const FString OutputRoot = FPaths::GetPath(FPaths::GetPath(HRPath));
	const FString FrameName = FPaths::ChangeExtension(FPaths::GetCleanFilename(HRPath), TEXT("exr"));
	const FString Path = FPaths::Combine(OutputRoot, TEXT("color_hr_reference_scene_hdr"), FrameName);
	FString Hash;
	if (!SaveImageAtomic(Path, TEXT("exr"), Reference, Hash, OutError))
	{
		return false;
	}
	OutHashes.Add(TEXT("color_hr_reference_scene_hdr"), Hash);
	LastReferenceHRMetadata = Result.Metadata;
	return true;
}

bool ASRDatasetCaptureRig::SaveHUDlessColorResult(
	const FSRDatasetTemporalCaptureResult& Result,
	const FString& LRPath,
	TMap<FString, FString>& OutHashes,
	FString& OutError)
{
	const int32 PixelCount = Result.Size.X * Result.Size.Y;
	if (PixelCount <= 0 || Result.SceneColor.Num() != PixelCount || !Result.Metadata.bValid)
	{
		OutError = TEXT("RDG HUD-less tonemapped color buffer has inconsistent dimensions or metadata.");
		return false;
	}
	FImage Image;
	Image.Init(Result.Size.X, Result.Size.Y, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	FMemory::Memcpy(Image.RawData.GetData(), Result.SceneColor.GetData(), Result.SceneColor.Num() * sizeof(FLinearColor));
	const FString OutputRoot = FPaths::GetPath(FPaths::GetPath(LRPath));
	const FString FrameName = FPaths::ChangeExtension(FPaths::GetCleanFilename(LRPath), TEXT("exr"));
	const FString Path = FPaths::Combine(OutputRoot, TEXT("color_main_view_hudless_after_tonemap"), FrameName);
	FString Hash;
	if (!SaveImageAtomic(Path, TEXT("exr"), Image, Hash, OutError))
	{
		return false;
	}
	OutHashes.Add(TEXT("color_main_view_hudless_after_tonemap"), Hash);
	LastHUDlessColorMetadata = Result.Metadata;
	LastHUDlessColorSize = Result.Size;
	return true;
}

bool ASRDatasetCaptureRig::SaveTemporalCaptureResult(
	const FSRDatasetTemporalCaptureResult& Result,
	const FString& LRPath,
	TMap<FString, FString>& OutHashes,
	FString& OutError)
{
	const int32 PixelCount = Result.Size.X * Result.Size.Y;
	if (PixelCount <= 0 || Result.SceneColor.Num() != PixelCount || Result.VelocityRaw.Num() != PixelCount ||
		Result.MotionFull.Num() != PixelCount || Result.Depth.Num() != PixelCount ||
		Result.Translucency.Num() != PixelCount || Result.ObjectId.Num() != PixelCount ||
		!Result.Metadata.bValid)
	{
		OutError = TEXT("RDG temporal diagnostic buffers have inconsistent dimensions or missing metadata.");
		return false;
	}

	const FString OutputRoot = FPaths::GetPath(FPaths::GetPath(LRPath));
	const FString FrameName = FPaths::ChangeExtension(FPaths::GetCleanFilename(LRPath), TEXT("exr"));
	const auto MakePath = [&OutputRoot, &FrameName](const TCHAR* Modality)
	{
		return FPaths::Combine(OutputRoot, Modality, FrameName);
	};
	const auto MakeImage = [&Result](const TArray<FLinearColor>& Pixels)
	{
		FImage Image;
		Image.Init(Result.Size.X, Result.Size.Y, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FLinearColor));
		return Image;
	};
	const auto MakeScalarImage = [&Result](const TArray<FLinearColor>& Source, const int32 Channel)
	{
		TArray<FLinearColor> Pixels;
		Pixels.SetNumUninitialized(Source.Num());
		for (int32 Index = 0; Index < Source.Num(); ++Index)
		{
			const float Value = Channel == 0 ? Source[Index].R : Channel == 1 ? Source[Index].G : Channel == 2 ? Source[Index].B : Source[Index].A;
			Pixels[Index] = FLinearColor(Value, Value, Value, 1.0f);
		}
		FImage Image;
		Image.Init(Result.Size.X, Result.Size.Y, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FLinearColor));
		return Image;
	};
	const auto MakeTransparencyMask = [&Result]()
	{
		TArray<FLinearColor> Pixels;
		Pixels.SetNumUninitialized(Result.Translucency.Num());
		for (int32 Index = 0; Index < Result.Translucency.Num(); ++Index)
		{
			const float Coverage = FMath::Clamp(1.0f - Result.Translucency[Index].A, 0.0f, 1.0f);
			Pixels[Index] = FLinearColor(Coverage, Coverage, Coverage, 1.0f);
		}
		FImage Image;
		Image.Init(Result.Size.X, Result.Size.Y, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FLinearColor));
		return Image;
	};

	struct FOutput
	{
		const TCHAR* Name;
		FImage Image;
	};
	TArray<FOutput> Outputs;
	Outputs.Add({ TEXT("color_lr_scene_hdr"), MakeImage(Result.SceneColor) });
	Outputs.Add({ TEXT("velocity_raw"), MakeImage(Result.VelocityRaw) });
	Outputs.Add({ TEXT("velocity_coverage"), MakeScalarImage(Result.VelocityRaw, 2) });
	Outputs.Add({ TEXT("motion_full_current_to_previous"), MakeImage(Result.MotionFull) });
	Outputs.Add({ TEXT("motion_valid"), MakeScalarImage(Result.MotionFull, 3) });
	Outputs.Add({ TEXT("depth_device_raw"), MakeScalarImage(Result.Depth, 1) });
	Outputs.Add({ TEXT("depth_view_linear_meters"), MakeScalarImage(Result.Depth, 0) });
	Outputs.Add({ TEXT("depth_valid"), MakeScalarImage(Result.Depth, 2) });
	Outputs.Add({ TEXT("translucency_after_dof_raw"), MakeImage(Result.Translucency) });
	Outputs.Add({ TEXT("transparency_mask"), MakeTransparencyMask() });
	// First conservative implementation: post-DOF translucency coverage is
	// always reactive. Opaque animated-material reactivity remains explicitly
	// excluded in metadata until a dedicated material/stencil signal is added.
	Outputs.Add({ TEXT("reactive_mask"), MakeTransparencyMask() });
	Outputs.Add({ TEXT("object_id"), MakeScalarImage(Result.ObjectId, 0) });

	for (const FOutput& Output : Outputs)
	{
		FString Hash;
		if (!SaveImageAtomic(MakePath(Output.Name), TEXT("exr"), Output.Image, Hash, OutError))
		{
			return false;
		}
		OutHashes.Add(Output.Name, Hash);
	}
	LastTemporalMetadata = Result.Metadata;
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
