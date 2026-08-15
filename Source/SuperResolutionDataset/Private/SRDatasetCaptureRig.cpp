#include "SRDatasetCaptureRig.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Slate/SGameLayerManager.h"
#include "Slate/WidgetRenderer.h"

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
	RendererPrimeCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("RendererPrimeCapture"));
	RendererPrimeCapture->SetupAttachment(SceneRoot);

	for (USceneCaptureComponent2D* Capture : {
		HRCapture.Get(), LRCapture.Get(), ReferenceCapture.Get(), DepthCapture.Get(), RendererPrimeCapture.Get() })
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
	RendererPrimeCapture->CaptureSource = SCS_FinalColorLDR;
}

bool ASRDatasetCaptureRig::Configure(const FSRDatasetCaptureJob& Job, FString& OutError)
{
	PreviousTemporalLogicalFrameId = INDEX_NONE;
	PreviousTemporalSize = FIntPoint::ZeroValue;
	PreviousTemporalDepth.Reset();
	PreviousTemporalObjectId.Reset();

	HRTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("SRDatasetHRTarget"));
	LRTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("SRDatasetLRTarget"));
	DepthTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("SRDatasetDepthTarget"));
	RendererPrimeTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("SRDatasetRendererPrimeTarget"));
	if (!HRTarget || !LRTarget || !DepthTarget || !RendererPrimeTarget)
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

	// Endpoint replay suppresses uncaptured player Main Views so their temporal
	// history does not advance. A tiny off-screen view keeps renderer resources
	// such as newly spawned Niagara payloads resident without paying production
	// capture resolution on every intermediate simulation frame.
	constexpr int32 PrimeLongEdge = 64;
	const bool bLandscape = Job.LRResolution.X >= Job.LRResolution.Y;
	const int32 PrimeWidth = bLandscape
		? PrimeLongEdge
		: FMath::Max(1, FMath::RoundToInt(
			static_cast<float>(PrimeLongEdge) * Job.LRResolution.X / Job.LRResolution.Y));
	const int32 PrimeHeight = bLandscape
		? FMath::Max(1, FMath::RoundToInt(
			static_cast<float>(PrimeLongEdge) * Job.LRResolution.Y / Job.LRResolution.X))
		: PrimeLongEdge;
	RendererPrimeTarget->ClearColor = FLinearColor::Black;
	RendererPrimeTarget->InitCustomFormat(PrimeWidth, PrimeHeight, PF_B8G8R8A8, false);
	RendererPrimeTarget->UpdateResourceImmediate(true);
	RendererPrimeCapture->TextureTarget = RendererPrimeTarget;

	if (Job.bCaptureUIColorAlpha)
	{
		// FWidgetRenderer owns a dedicated Slate renderer, but the target must
		// remain referenced by the rig across frames. The default (no explicit
		// gamma pass) path writes display-encoded Slate color into an sRGB BGRA8
		// target while preserving the transparent clear and coverage alpha.
		UIColorAlphaTarget = FWidgetRenderer::CreateTargetFor(
			FVector2D(Job.HRResolution), TF_Nearest, false);
		if (!UIColorAlphaTarget)
		{
			OutError = TEXT("Failed to allocate the independent Slate UI render target.");
			return false;
		}
		UIColorAlphaTarget->ClearColor = FLinearColor::Transparent;
	}

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
	ApplyViewToCapture(RendererPrimeCapture, View, bDisableMotionBlur, bLockExposure);
}

void ASRDatasetCaptureRig::PrimeRendererState(const TArray<UPrimitiveComponent*>& Components)
{
	RendererPrimeCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	RendererPrimeCapture->ClearShowOnlyComponents();
	for (UPrimitiveComponent* Component : Components)
	{
		if (IsValid(Component) && Component->IsRegistered())
		{
			RendererPrimeCapture->ShowOnlyComponent(Component);
		}
	}
	RendererPrimeCapture->CaptureScene();
}

void ASRDatasetCaptureRig::WarmupRenderState(const FSRDatasetCaptureJob& Job)
{
	// Initialize each modality's persistent SceneCapture view state without
	// writing a training sample or advancing world simulation.
	const auto WarmHighResolution = [this, &Job]()
	{
		HRCapture->CaptureScene();
		if (Job.bCaptureReferenceHR)
		{
			ReferenceCapture->CaptureScene();
		}
	};
	const auto WarmLowResolution = [this, &Job]()
	{
		if (Job.LRMode == ESRDatasetLRMode::NativeRender)
		{
			LRCapture->CaptureScene();
		}
		if (Job.bCaptureDepth)
		{
			DepthCapture->CaptureScene();
		}
	};
	if (Job.AuxiliaryCaptureOrder == ESRDatasetAuxiliaryCaptureOrder::LowResolutionFirst)
	{
		WarmLowResolution();
		WarmHighResolution();
	}
	else
	{
		WarmHighResolution();
		WarmLowResolution();
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
	const int32 LogicalFrameNumber,
	const int32 MotionPreviousLogicalFrameId,
	const bool bHistoryReset,
	const FString& HRPath,
	const FString& LRPath,
	const FString& DepthPath,
	TMap<FString, FString>& OutHashes,
	FString& OutError)
{
	LastTemporalMetadata = FSRDatasetTemporalFrameMetadata();
	LastNativeHRMetadata = FSRDatasetTemporalFrameMetadata();
	LastSceneCaptureLRMetadata = FSRDatasetTemporalFrameMetadata();
	LastReferenceHRMetadata = FSRDatasetTemporalFrameMetadata();
	LastHUDlessColorMetadata = FSRDatasetTemporalFrameMetadata();
	LastHUDlessColorSize = FIntPoint::ZeroValue;
	LastUIColorAlphaSize = FIntPoint::ZeroValue;
	LastUINonzeroAlphaPixelCount = 0;
	LastUIFractionalAlphaPixelCount = 0;
	LastUIMinAlpha = 0.0f;
	LastUIMaxAlpha = 0.0f;
	TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> ViewExtension;
	if (Job.bCaptureTemporalDiagnostics)
	{
		ViewExtension = GetSRDatasetViewExtension();
		if (!ViewExtension)
		{
			OutError = TEXT("The SRDataset RDG view extension is not available for isolated capture.");
			return false;
		}
	}

	FImage HRImage;
	FImage LRImage;
	const auto CaptureHighResolution = [&]() -> bool
	{
		if (Job.bCaptureTemporalDiagnostics &&
			!ViewExtension->RequestCapture(Job.HRResolution, Job.HRResolution, false, OutError))
		{
			return false;
		}
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
		return true;
	};
	const auto CaptureReference = [&]() -> bool
	{
		if (!Job.bCaptureReferenceHR)
		{
			return true;
		}
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
		return true;
	};
	const auto CaptureLowResolution = [&]() -> bool
	{
		if (Job.LRMode != ESRDatasetLRMode::NativeRender)
		{
			return true;
		}
		const bool bExtractSceneCaptureDiagnostics = Job.bCaptureTemporalDiagnostics &&
			(!Job.bCaptureMainViewTemporalDiagnostics || Job.bCaptureSceneCaptureLRComparison);
		if (bExtractSceneCaptureDiagnostics)
		{
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

		if (bExtractSceneCaptureDiagnostics)
		{
			FSRDatasetTemporalCaptureResult TemporalResult;
			if (!ViewExtension->WaitAndTakeCapture(TemporalResult, OutError))
			{
				return false;
			}
			if (Job.bCaptureMainViewTemporalDiagnostics)
			{
				if (!SaveSceneCaptureLRHDRColorResult(TemporalResult, LRPath, OutHashes, OutError))
				{
					return false;
				}
			}
			else if (!SaveTemporalCaptureResult(
				TemporalResult,
				LRPath,
				LogicalFrameNumber,
				MotionPreviousLogicalFrameId,
				bHistoryReset,
				OutHashes,
				OutError))
			{
				return false;
			}
		}
		return true;
	};
	const auto CaptureDepth = [&]() -> bool
	{
		if (!Job.bCaptureDepth)
		{
			return true;
		}
		FImage DepthImage;
		DepthCapture->CaptureScene();
		if (!FImageUtils::GetRenderTargetImage(DepthTarget, DepthImage))
		{
			OutError = TEXT("Failed to read the depth render target.");
			return false;
		}
		FString DepthHash;
		if (!SaveImageAtomic(DepthPath, TEXT("exr"), DepthImage, DepthHash, OutError))
		{
			return false;
		}
		OutHashes.Add(TEXT("depth"), DepthHash);
		return true;
	};

	if (Job.AuxiliaryCaptureOrder == ESRDatasetAuxiliaryCaptureOrder::LowResolutionFirst)
	{
		if (!CaptureLowResolution() || !CaptureDepth() ||
			!CaptureHighResolution() || !CaptureReference())
		{
			return false;
		}
	}
	else if (!CaptureHighResolution() || !CaptureReference() ||
		!CaptureLowResolution() || !CaptureDepth())
	{
		return false;
	}

	if (Job.LRMode == ESRDatasetLRMode::DownsampleFromHR)
	{
		LRImage.Init(Job.LRResolution.X, Job.LRResolution.Y, 1, HRImage.Format, HRImage.GammaSpace);
		FImageCore::ResizeImage(HRImage, LRImage, ToImageFilter(Job.ResizeFilter));
	}

	FString Hash;
	if (!SaveImageAtomic(HRPath, TEXT("png"), HRImage, Hash, OutError))
	{
		return false;
	}
	OutHashes.Add(TEXT("hr"), Hash);
	if (!SaveImageAtomic(LRPath, TEXT("png"), LRImage, Hash, OutError))
	{
		return false;
	}
	OutHashes.Add(TEXT("lr"), Hash);

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

bool ASRDatasetCaptureRig::SaveSceneCaptureLRHDRColorResult(
	const FSRDatasetTemporalCaptureResult& Result,
	const FString& LRPath,
	TMap<FString, FString>& OutHashes,
	FString& OutError)
{
	const int32 PixelCount = Result.Size.X * Result.Size.Y;
	if (PixelCount <= 0 || Result.SceneColor.Num() != PixelCount || !Result.Metadata.bValid)
	{
		OutError = TEXT("RDG SceneCapture LR comparison color has inconsistent dimensions or metadata.");
		return false;
	}

	FImage Image;
	Image.Init(Result.Size.X, Result.Size.Y, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	FMemory::Memcpy(Image.RawData.GetData(), Result.SceneColor.GetData(), Result.SceneColor.Num() * sizeof(FLinearColor));
	const FString OutputRoot = FPaths::GetPath(FPaths::GetPath(LRPath));
	const FString FrameName = FPaths::ChangeExtension(FPaths::GetCleanFilename(LRPath), TEXT("exr"));
	const FString Path = FPaths::Combine(OutputRoot, TEXT("color_lr_scene_capture_hdr"), FrameName);
	FString Hash;
	if (!SaveImageAtomic(Path, TEXT("exr"), Image, Hash, OutError))
	{
		return false;
	}
	OutHashes.Add(TEXT("color_lr_scene_capture_hdr"), Hash);
	LastSceneCaptureLRMetadata = Result.Metadata;
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

bool ASRDatasetCaptureRig::CaptureUIColorAlpha(
	const FSRDatasetCaptureJob& Job,
	const FString& OutputPath,
	TMap<FString, FString>& OutHashes,
	FString& OutError)
{
	if (!Job.bCaptureUIColorAlpha)
	{
		return true;
	}
	if (!UIColorAlphaTarget || !GetWorld())
	{
		OutError = TEXT("The independent Slate UI render target is unavailable.");
		return false;
	}

	UGameViewportClient* ViewportClient = GetWorld()->GetGameViewport();
	const TSharedPtr<IGameLayerManager> LayerManager = ViewportClient
		? ViewportClient->GetGameLayerManager()
		: nullptr;
	if (!LayerManager)
	{
		OutError = TEXT("UI Color/Alpha capture requires a game viewport layer manager.");
		return false;
	}
	// UGameEngine creates SGameLayerManager as the concrete implementation. By
	// painting that child rather than the enclosing SViewport we include
	// screen-space game layers while excluding the scene/backbuffer itself.
	const TSharedPtr<SGameLayerManager> GameLayerWidget =
		StaticCastSharedPtr<SGameLayerManager>(LayerManager);
	if (!GameLayerWidget)
	{
		OutError = TEXT("The game viewport uses an unsupported layer-manager implementation.");
		return false;
	}

	FWidgetRenderer WidgetRenderer(false, true);
	WidgetRenderer.SetApplyColorDeficiencyCorrection(false);
	WidgetRenderer.DrawWidget(
		UIColorAlphaTarget,
		StaticCastSharedRef<SWidget>(GameLayerWidget.ToSharedRef()),
		FVector2D(Job.HRResolution),
		static_cast<float>(Job.GetFixedDeltaSeconds()),
		false);
	FlushRenderingCommands();

	FImage Image;
	if (!FImageUtils::GetRenderTargetImage(UIColorAlphaTarget, Image) ||
		Image.SizeX != Job.HRResolution.X || Image.SizeY != Job.HRResolution.Y)
	{
		OutError = TEXT("Could not read the independent Slate UI Color/Alpha target.");
		return false;
	}
	FImage LinearImage;
	Image.CopyTo(LinearImage, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	const TArrayView64<const FLinearColor> Pixels = LinearImage.AsRGBA32F();
	LastUIMinAlpha = 1.0f;
	LastUIMaxAlpha = 0.0f;
	for (const FLinearColor& Pixel : Pixels)
	{
		const float Alpha = FMath::Clamp(Pixel.A, 0.0f, 1.0f);
		LastUIMinAlpha = FMath::Min(LastUIMinAlpha, Alpha);
		LastUIMaxAlpha = FMath::Max(LastUIMaxAlpha, Alpha);
		LastUINonzeroAlphaPixelCount += Alpha > (0.5f / 255.0f) ? 1 : 0;
		LastUIFractionalAlphaPixelCount +=
			Alpha > (0.5f / 255.0f) && Alpha < (254.5f / 255.0f) ? 1 : 0;
	}
	if (Pixels.IsEmpty())
	{
		LastUIMinAlpha = 0.0f;
	}
	LastUIColorAlphaSize = Job.HRResolution;

	FString Hash;
	if (!SaveImageAtomic(OutputPath, TEXT("png"), Image, Hash, OutError))
	{
		return false;
	}
	OutHashes.Add(TEXT("ui_color_alpha"), Hash);
	return true;
}

bool ASRDatasetCaptureRig::SaveTemporalCaptureResult(
	const FSRDatasetTemporalCaptureResult& Result,
	const FString& LRPath,
	const int32 LogicalFrameNumber,
	const int32 MotionPreviousLogicalFrameId,
	const bool bHistoryReset,
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
	const auto MakeScalarPixelsImage = [&Result](const TArray<FLinearColor>& Pixels)
	{
		FImage Image;
		Image.Init(Result.Size.X, Result.Size.Y, 1, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
		FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FLinearColor));
		return Image;
	};

	if (!bHistoryReset && !EnsurePreviousTemporalState(
		OutputRoot,
		MotionPreviousLogicalFrameId,
		Result.Size,
		OutError))
	{
		return false;
	}

	TArray<FLinearColor> HistoryRejection;
	TArray<FLinearColor> HistoryRejectionValid;
	TArray<FLinearColor> HistoryRejectionReason;
	HistoryRejection.SetNumUninitialized(PixelCount);
	HistoryRejectionValid.SetNumUninitialized(PixelCount);
	HistoryRejectionReason.SetNumUninitialized(PixelCount);
	const float DisplayToRenderScale = FMath::IsFinite(Result.Metadata.ResolutionFraction)
		? Result.Metadata.ResolutionFraction
		: 0.0f;
	for (int32 Y = 0; Y < Result.Size.Y; ++Y)
	{
		for (int32 X = 0; X < Result.Size.X; ++X)
		{
			const int32 Index = Y * Result.Size.X + X;
			float Reject = 1.0f;
			float Valid = 0.0f;
			float Reason = 2.0f; // Invalid current inputs.
			if (bHistoryReset)
			{
				// A reset has no reusable history by definition; rejecting every
				// pixel is exact and therefore valid rather than an approximation.
				Valid = 1.0f;
				Reason = 1.0f;
			}
			else
			{
				const FLinearColor& CurrentDepth = Result.Depth[Index];
				const FLinearColor& CurrentMotion = Result.MotionFull[Index];
				const bool bCurrentInputsValid = CurrentDepth.B >= 0.5f &&
					CurrentMotion.A >= 0.5f && DisplayToRenderScale > 0.0f &&
					FMath::IsFinite(CurrentMotion.R) && FMath::IsFinite(CurrentMotion.G);
				if (bCurrentInputsValid)
				{
					const int32 PreviousX = FMath::RoundToInt(
						X + CurrentMotion.R * DisplayToRenderScale);
					const int32 PreviousY = FMath::RoundToInt(
						Y + CurrentMotion.G * DisplayToRenderScale);
					if (PreviousX < 0 || PreviousY < 0 ||
						PreviousX >= Result.Size.X || PreviousY >= Result.Size.Y)
					{
						// Reprojection outside the previous render rect is a definitive
						// history rejection.
						Valid = 1.0f;
						Reason = 3.0f;
					}
					else
					{
						const int32 PreviousIndex = PreviousY * Result.Size.X + PreviousX;
						const int32 CurrentId = FMath::RoundToInt(Result.ObjectId[Index].R);
						const int32 PreviousId = FMath::RoundToInt(PreviousTemporalObjectId[PreviousIndex].R);
						const bool bAnyLabeled = CurrentId != 0 || PreviousId != 0;
						const bool bSameLabeledInstance = CurrentId != 0 && CurrentId == PreviousId;
						const bool bNativeVelocityCovered = CurrentMotion.B >= 0.5f;
						if (bAnyLabeled && !bSameLabeledInstance)
						{
							// Different component identities at the motion-reprojected pixel
							// are definitive cross-instance occlusion/disocclusion evidence.
							Reject = 1.0f;
							Valid = 1.0f;
							Reason = 4.0f;
						}
						else if (bNativeVelocityCovered)
						{
							// Component identity cannot prove surface identity inside a moving
							// rigid or deforming component. Reject conservatively and expose
							// the uncertainty instead of emitting over-trusted supervision.
							Reject = 1.0f;
							Valid = 0.0f;
							Reason = bSameLabeledInstance ? 6.0f : 7.0f;
						}
						else
						{
							const float PreviousDeviceZ = PreviousTemporalDepth[PreviousIndex].R;
							const float ReprojectedDeviceZ = CurrentDepth.A;
							if (PreviousDeviceZ > 0.0f && ReprojectedDeviceZ > 0.0f &&
								FMath::IsFinite(PreviousDeviceZ) && FMath::IsFinite(ReprojectedDeviceZ))
							{
								// Reversed-Z: a larger previous depth value is closer. If the
								// previous visible surface was closer than the reprojected current
								// surface, current history was occluded and must be rejected.
								const float Tolerance = FMath::Max(
									1.0e-5f,
									FMath::Abs(ReprojectedDeviceZ) * 2.0e-3f);
								const bool bOccluded = PreviousDeviceZ > ReprojectedDeviceZ + Tolerance;
								Reject = bOccluded ? 1.0f : 0.0f;
								Valid = 1.0f;
								Reason = bOccluded ? 5.0f : 0.0f;
							}
							else
							{
								Reject = 1.0f;
								Valid = 0.0f;
								Reason = 8.0f;
							}
						}
					}
				}
			}
			HistoryRejection[Index] = FLinearColor(Reject, Reject, Reject, 1.0f);
			HistoryRejectionValid[Index] = FLinearColor(Valid, Valid, Valid, 1.0f);
			HistoryRejectionReason[Index] = FLinearColor(Reason, Reason, Reason, 1.0f);
		}
	}

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
	Outputs.Add({ TEXT("depth_previous_reprojected_device"), MakeScalarImage(Result.Depth, 3) });
	Outputs.Add({ TEXT("history_rejection_mask"), MakeScalarPixelsImage(HistoryRejection) });
	Outputs.Add({ TEXT("history_rejection_valid"), MakeScalarPixelsImage(HistoryRejectionValid) });
	Outputs.Add({ TEXT("history_rejection_reason"), MakeScalarPixelsImage(HistoryRejectionReason) });
	// Explicit disocclusion aliases make the supervision discoverable without
	// changing the established history-rejection modality names.
	Outputs.Add({ TEXT("disocclusion_mask"), MakeScalarPixelsImage(HistoryRejection) });
	Outputs.Add({ TEXT("disocclusion_valid"), MakeScalarPixelsImage(HistoryRejectionValid) });
	Outputs.Add({ TEXT("disocclusion_reason"), MakeScalarPixelsImage(HistoryRejectionReason) });
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
	PreviousTemporalLogicalFrameId = LogicalFrameNumber;
	PreviousTemporalSize = Result.Size;
	PreviousTemporalDepth.SetNumUninitialized(PixelCount);
	PreviousTemporalObjectId.SetNumUninitialized(PixelCount);
	for (int32 Index = 0; Index < PixelCount; ++Index)
	{
		const float DeviceZ = Result.Depth[Index].G;
		const float ObjectId = Result.ObjectId[Index].R;
		PreviousTemporalDepth[Index] = FLinearColor(DeviceZ, DeviceZ, DeviceZ, 1.0f);
		PreviousTemporalObjectId[Index] = FLinearColor(ObjectId, ObjectId, ObjectId, 1.0f);
	}
	LastTemporalMetadata = Result.Metadata;
	return true;
}

bool ASRDatasetCaptureRig::LoadScalarImage(
	const FString& Path,
	const FIntPoint ExpectedSize,
	TArray<FLinearColor>& OutPixels)
{
	FImage Image;
	if (!FImageUtils::LoadImage(*Path, Image) ||
		Image.SizeX != ExpectedSize.X || Image.SizeY != ExpectedSize.Y || Image.NumSlices != 1)
	{
		return false;
	}
	Image.ChangeFormat(ERawImageFormat::RGBA32F, EGammaSpace::Linear);
	const int32 PixelCount = ExpectedSize.X * ExpectedSize.Y;
	if (Image.RawData.Num() != PixelCount * static_cast<int64>(sizeof(FLinearColor)))
	{
		return false;
	}
	OutPixels.SetNumUninitialized(PixelCount);
	FMemory::Memcpy(OutPixels.GetData(), Image.RawData.GetData(), Image.RawData.Num());
	return true;
}

bool ASRDatasetCaptureRig::EnsurePreviousTemporalState(
	const FString& OutputRoot,
	const int32 PreviousLogicalFrameId,
	const FIntPoint ExpectedSize,
	FString& OutError)
{
	const int32 ExpectedPixelCount = ExpectedSize.X * ExpectedSize.Y;
	if (PreviousTemporalLogicalFrameId == PreviousLogicalFrameId &&
		PreviousTemporalSize == ExpectedSize &&
		PreviousTemporalDepth.Num() == ExpectedPixelCount &&
		PreviousTemporalObjectId.Num() == ExpectedPixelCount)
	{
		return true;
	}

	const FString FrameName = FString::Printf(TEXT("frame_%06d.exr"), PreviousLogicalFrameId);
	const FString DepthPath = FPaths::Combine(OutputRoot, TEXT("depth_device_raw"), FrameName);
	const FString ObjectIdPath = FPaths::Combine(OutputRoot, TEXT("object_id"), FrameName);
	if (!LoadScalarImage(DepthPath, ExpectedSize, PreviousTemporalDepth) ||
		!LoadScalarImage(ObjectIdPath, ExpectedSize, PreviousTemporalObjectId))
	{
		PreviousTemporalLogicalFrameId = INDEX_NONE;
		PreviousTemporalSize = FIntPoint::ZeroValue;
		PreviousTemporalDepth.Reset();
		PreviousTemporalObjectId.Reset();
		OutError = FString::Printf(
			TEXT("Could not load previous temporal depth/Object ID for history rejection: frame %d."),
			PreviousLogicalFrameId);
		return false;
	}
	PreviousTemporalLogicalFrameId = PreviousLogicalFrameId;
	PreviousTemporalSize = ExpectedSize;
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
