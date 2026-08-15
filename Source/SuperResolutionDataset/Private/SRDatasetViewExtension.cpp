#include "SRDatasetViewExtension.h"

#include "GlobalShader.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "SystemTextures.h"

namespace
{
	TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> GSRDatasetViewExtension;
	TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> GSRDatasetTonemapViewExtension;

	class FSRDatasetExtractCS final : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSRDatasetExtractCS);
		SHADER_USE_PARAMETER_STRUCT(FSRDatasetExtractCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSceneColor)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputVelocity)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputDepth)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSeparateTranslucency)
			SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, InputCustomStencil)
			SHADER_PARAMETER_SAMPLER(SamplerState, InputSeparateTranslucencySampler)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSceneColor)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutVelocityRaw)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutMotionFull)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutDepth)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutTranslucency)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutObjectId)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutMetadata)
			SHADER_PARAMETER(FIntPoint, InputViewRectMin)
			SHADER_PARAMETER(FIntPoint, OutputSize)
			SHADER_PARAMETER(FVector2f, DisplaySize)
			SHADER_PARAMETER(FIntPoint, SeparateTranslucencyViewRectMin)
			SHADER_PARAMETER(FIntPoint, SeparateTranslucencyViewRectSize)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FSRDatasetExtractCS, "/Plugin/SuperResolutionDataset/Private/SRDatasetExtract.usf", "MainCS", SF_Compute);

	class FSRDatasetCopyColorCS final : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FSRDatasetCopyColorCS);
		SHADER_USE_PARAMETER_STRUCT(FSRDatasetCopyColorCS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
			SHADER_PARAMETER_RDG_TEXTURE(Texture2D, InputSceneColor)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSceneColor)
			SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutMetadata)
			SHADER_PARAMETER(FIntPoint, InputViewRectMin)
			SHADER_PARAMETER(FIntPoint, OutputSize)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FSRDatasetCopyColorCS, "/Plugin/SuperResolutionDataset/Private/SRDatasetExtract.usf", "CopyColorCS", SF_Compute);
}

FSRDatasetViewExtension::FSRDatasetViewExtension(
	const FAutoRegister& AutoRegister,
	const ESRDatasetViewCaptureStage InCaptureStage)
	: FSceneViewExtensionBase(AutoRegister)
	, CaptureStage(InCaptureStage)
{
}

void FSRDatasetViewExtension::SetDeterministicViewTime(
	const double CurrentTimeSeconds,
	const float DeltaTimeSeconds)
{
	FScopeLock Lock(&StateMutex);
	bDeterministicViewTimeEnabled = true;
	DeterministicCurrentTimeSeconds = CurrentTimeSeconds;
	DeterministicDeltaTimeSeconds = DeltaTimeSeconds;
}

void FSRDatasetViewExtension::ClearDeterministicViewTime()
{
	FScopeLock Lock(&StateMutex);
	bDeterministicViewTimeEnabled = false;
	DeterministicCurrentTimeSeconds = 0.0;
	DeterministicDeltaTimeSeconds = 0.0f;
}

void FSRDatasetViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	FScopeLock Lock(&StateMutex);
	if (bDeterministicViewTimeEnabled)
	{
		// Renderer view-state reset detection uses real time, while material Game
		// Time needs to follow logical frame direction. A stable non-zero origin
		// preserves view history and freezes Ignore-Pause/Real-Time expressions.
		constexpr double DeterministicRealTimeOriginSeconds = 86400.0;
		InViewFamily.Time = FGameTime::CreateDilated(
			DeterministicRealTimeOriginSeconds,
			0.0f,
			DeterministicCurrentTimeSeconds,
			DeterministicDeltaTimeSeconds);
	}
}

bool FSRDatasetViewExtension::RequestCapture(
	const FIntPoint ExpectedSize,
	const FIntPoint DisplaySize,
	const bool bMainViewOnly,
	FString& OutError)
{
	if (CaptureStage != ESRDatasetViewCaptureStage::AfterDOFTemporal)
	{
		OutError = TEXT("Temporal capture was requested from the tonemap-only view extension.");
		return false;
	}
	FScopeLock Lock(&StateMutex);
	if (bRequestPending || bRequestConsumed || PendingReadbacks)
	{
		OutError = TEXT("A temporal diagnostic render capture is already pending.");
		return false;
	}
	if (ExpectedSize.X <= 0 || ExpectedSize.Y <= 0 || DisplaySize.X <= 0 || DisplaySize.Y <= 0)
	{
		OutError = TEXT("Temporal diagnostic capture sizes must be positive.");
		return false;
	}

	RequestedSize = ExpectedSize;
	RequestedDisplaySize = DisplaySize;
	bRequestedMainViewOnly = bMainViewOnly;
	bRequestPending = true;
	OutError.Reset();
	return true;
}

bool FSRDatasetViewExtension::RequestTonemappedColorCapture(
	const FIntPoint ExpectedSize,
	const bool bMainViewOnly,
	FString& OutError)
{
	if (CaptureStage != ESRDatasetViewCaptureStage::AfterTonemapColor)
	{
		OutError = TEXT("Tonemapped color was requested from the temporal view extension.");
		return false;
	}
	FScopeLock Lock(&StateMutex);
	if (bRequestPending || bRequestConsumed || PendingReadbacks)
	{
		OutError = TEXT("A tonemapped color capture is already pending.");
		return false;
	}
	if (ExpectedSize.X <= 0 || ExpectedSize.Y <= 0)
	{
		OutError = TEXT("Tonemapped color capture size must be positive.");
		return false;
	}

	RequestedSize = ExpectedSize;
	RequestedDisplaySize = ExpectedSize;
	bRequestedMainViewOnly = bMainViewOnly;
	bRequestPending = true;
	OutError.Reset();
	return true;
}

void FSRDatasetViewExtension::SubscribeToPostProcessingPass(
	const EPostProcessingPass Pass,
	const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks,
	const bool bIsPassEnabled)
{
	const EPostProcessingPass RequestedPass = CaptureStage == ESRDatasetViewCaptureStage::AfterDOFTemporal
		? EPostProcessingPass::AfterDOF
		: EPostProcessingPass::Tonemap;
	if (Pass != RequestedPass)
	{
		return;
	}

	FScopeLock Lock(&StateMutex);
	if (bRequestPending && !bRequestConsumed && InView.bIsSceneCapture != bRequestedMainViewOnly)
	{
		InOutPassCallbacks.Add(CaptureStage == ESRDatasetViewCaptureStage::AfterDOFTemporal
			? FPostProcessingPassDelegate::CreateRaw(this, &FSRDatasetViewExtension::CaptureAfterDOF_RenderThread)
			: FPostProcessingPassDelegate::CreateRaw(this, &FSRDatasetViewExtension::CaptureAfterTonemap_RenderThread));
	}
}

FScreenPassTexture FSRDatasetViewExtension::CaptureAfterTonemap_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor(Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	FIntPoint OutputSize;
	{
		FScopeLock Lock(&StateMutex);
		if (!bRequestPending || bRequestConsumed || View.bIsSceneCapture == bRequestedMainViewOnly ||
			!SceneColor.IsValid() || SceneColor.ViewRect.Size() != RequestedSize)
		{
			return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		}
		bRequestPending = false;
		bRequestConsumed = true;
		OutputSize = RequestedSize;
	}

	const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
		OutputSize,
		PF_A32B32G32R32F,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);
	const FRDGTextureDesc MetadataDesc = FRDGTextureDesc::Create2D(
		FIntPoint(4, 21),
		PF_A32B32G32R32F,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef SceneColorOutput = GraphBuilder.CreateTexture(OutputDesc, TEXT("SRDataset.TonemappedHUDlessColor"));
	FRDGTextureRef MetadataOutput = GraphBuilder.CreateTexture(MetadataDesc, TEXT("SRDataset.TonemappedMetadata"));

	FSRDatasetCopyColorCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSRDatasetCopyColorCS::FParameters>();
	Parameters->View = View.ViewUniformBuffer;
	Parameters->InputSceneColor = SceneColor.Texture;
	Parameters->OutSceneColor = GraphBuilder.CreateUAV(SceneColorOutput);
	Parameters->OutMetadata = GraphBuilder.CreateUAV(MetadataOutput);
	Parameters->InputViewRectMin = SceneColor.ViewRect.Min;
	Parameters->OutputSize = OutputSize;
	TShaderMapRef<FSRDatasetCopyColorCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("SRDataset Extract Tonemapped HUD-less Color"),
		ComputeShader,
		Parameters,
		FComputeShaderUtils::GetGroupCount(
			FIntPoint(FMath::Max(OutputSize.X, 4), FMath::Max(OutputSize.Y, 21)),
			FIntPoint(8, 8)));

	TUniquePtr<FPendingReadbacks> NewReadbacks = MakeUnique<FPendingReadbacks>();
	NewReadbacks->Size = OutputSize;
	NewReadbacks->bColorOnly = true;
	NewReadbacks->SceneColor = MakeUnique<FRHIGPUTextureReadback>(TEXT("SRDatasetTonemappedHUDlessColor"));
	NewReadbacks->Metadata = MakeUnique<FRHIGPUTextureReadback>(TEXT("SRDatasetTonemappedMetadata"));
	AddEnqueueCopyPass(GraphBuilder, NewReadbacks->SceneColor.Get(), SceneColorOutput);
	AddEnqueueCopyPass(GraphBuilder, NewReadbacks->Metadata.Get(), MetadataOutput);
	{
		FScopeLock Lock(&StateMutex);
		PendingReadbacks = MoveTemp(NewReadbacks);
	}
	return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
}

FScreenPassTexture FSRDatasetViewExtension::CaptureAfterDOF_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor(Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	FIntPoint OutputSize;
	FIntPoint DisplaySize;
	{
		FScopeLock Lock(&StateMutex);
		if (!bRequestPending || bRequestConsumed || View.bIsSceneCapture == bRequestedMainViewOnly ||
			!SceneColor.IsValid() || SceneColor.ViewRect.Size() != RequestedSize)
		{
			return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
		}
		bRequestPending = false;
		bRequestConsumed = true;
		OutputSize = RequestedSize;
		DisplaySize = RequestedDisplaySize;
	}

	if (!Inputs.SceneTextures.SceneTextures)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	const FSceneTextureUniformParameters& SceneTextures = *Inputs.SceneTextures.SceneTextures->GetContents();
	if (!SceneTextures.GBufferVelocityTexture || !SceneTextures.SceneDepthTexture)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
		OutputSize,
		PF_A32B32G32R32F,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);
	const FRDGTextureDesc MetadataDesc = FRDGTextureDesc::Create2D(
		FIntPoint(4, 21),
		PF_A32B32G32R32F,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef SceneColorOutput = GraphBuilder.CreateTexture(OutputDesc, TEXT("SRDataset.SceneColorAfterDOF"));
	FRDGTextureRef VelocityRawOutput = GraphBuilder.CreateTexture(OutputDesc, TEXT("SRDataset.VelocityRaw"));
	FRDGTextureRef MotionFullOutput = GraphBuilder.CreateTexture(OutputDesc, TEXT("SRDataset.MotionFull"));
	FRDGTextureRef DepthOutput = GraphBuilder.CreateTexture(OutputDesc, TEXT("SRDataset.Depth"));
	FRDGTextureRef TranslucencyOutput = GraphBuilder.CreateTexture(OutputDesc, TEXT("SRDataset.Translucency"));
	FRDGTextureRef ObjectIdOutput = GraphBuilder.CreateTexture(OutputDesc, TEXT("SRDataset.ObjectId"));
	FRDGTextureRef MetadataOutput = GraphBuilder.CreateTexture(MetadataDesc, TEXT("SRDataset.Metadata"));

	FSRDatasetExtractCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSRDatasetExtractCS::FParameters>();
	Parameters->View = View.ViewUniformBuffer;
	Parameters->InputSceneColor = SceneColor.Texture;
	Parameters->InputVelocity = SceneTextures.GBufferVelocityTexture;
	Parameters->InputDepth = SceneTextures.SceneDepthTexture;
	Parameters->InputCustomStencil = SceneTextures.CustomStencilTexture;
	const FScreenPassTexture SeparateTranslucency(Inputs.GetInput(EPostProcessMaterialInput::SeparateTranslucency));
	const bool bHasSeparateTranslucency = SeparateTranslucency.IsValid();
	Parameters->InputSeparateTranslucency = bHasSeparateTranslucency
		? SeparateTranslucency.Texture
		: GSystemTextures.GetBlackAlphaOneDummy(GraphBuilder);
	Parameters->InputSeparateTranslucencySampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	Parameters->OutSceneColor = GraphBuilder.CreateUAV(SceneColorOutput);
	Parameters->OutVelocityRaw = GraphBuilder.CreateUAV(VelocityRawOutput);
	Parameters->OutMotionFull = GraphBuilder.CreateUAV(MotionFullOutput);
	Parameters->OutDepth = GraphBuilder.CreateUAV(DepthOutput);
	Parameters->OutTranslucency = GraphBuilder.CreateUAV(TranslucencyOutput);
	Parameters->OutObjectId = GraphBuilder.CreateUAV(ObjectIdOutput);
	Parameters->OutMetadata = GraphBuilder.CreateUAV(MetadataOutput);
	Parameters->InputViewRectMin = SceneColor.ViewRect.Min;
	Parameters->OutputSize = OutputSize;
	Parameters->DisplaySize = FVector2f(DisplaySize);
	Parameters->SeparateTranslucencyViewRectMin = bHasSeparateTranslucency
		? SeparateTranslucency.ViewRect.Min
		: FIntPoint::ZeroValue;
	Parameters->SeparateTranslucencyViewRectSize = bHasSeparateTranslucency
		? SeparateTranslucency.ViewRect.Size()
		: FIntPoint(1, 1);

	TShaderMapRef<FSRDatasetExtractCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("SRDataset Extract Temporal Inputs"),
		ComputeShader,
		Parameters,
		FComputeShaderUtils::GetGroupCount(
			FIntPoint(FMath::Max(OutputSize.X, 4), FMath::Max(OutputSize.Y, 21)),
			FIntPoint(8, 8)));

	TUniquePtr<FPendingReadbacks> NewReadbacks = MakeUnique<FPendingReadbacks>();
	NewReadbacks->Size = OutputSize;
	NewReadbacks->bColorOnly = false;
	NewReadbacks->SceneColor = MakeUnique<FRHIGPUTextureReadback>(TEXT("SRDatasetSceneColor"));
	NewReadbacks->VelocityRaw = MakeUnique<FRHIGPUTextureReadback>(TEXT("SRDatasetVelocityRaw"));
	NewReadbacks->MotionFull = MakeUnique<FRHIGPUTextureReadback>(TEXT("SRDatasetMotionFull"));
	NewReadbacks->Depth = MakeUnique<FRHIGPUTextureReadback>(TEXT("SRDatasetDepth"));
	NewReadbacks->Translucency = MakeUnique<FRHIGPUTextureReadback>(TEXT("SRDatasetTranslucency"));
	NewReadbacks->ObjectId = MakeUnique<FRHIGPUTextureReadback>(TEXT("SRDatasetObjectId"));
	NewReadbacks->Metadata = MakeUnique<FRHIGPUTextureReadback>(TEXT("SRDatasetMetadata"));

	AddEnqueueCopyPass(GraphBuilder, NewReadbacks->SceneColor.Get(), SceneColorOutput);
	AddEnqueueCopyPass(GraphBuilder, NewReadbacks->VelocityRaw.Get(), VelocityRawOutput);
	AddEnqueueCopyPass(GraphBuilder, NewReadbacks->MotionFull.Get(), MotionFullOutput);
	AddEnqueueCopyPass(GraphBuilder, NewReadbacks->Depth.Get(), DepthOutput);
	AddEnqueueCopyPass(GraphBuilder, NewReadbacks->Translucency.Get(), TranslucencyOutput);
	AddEnqueueCopyPass(GraphBuilder, NewReadbacks->ObjectId.Get(), ObjectIdOutput);
	AddEnqueueCopyPass(GraphBuilder, NewReadbacks->Metadata.Get(), MetadataOutput);

	{
		FScopeLock Lock(&StateMutex);
		PendingReadbacks = MoveTemp(NewReadbacks);
	}
	return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
}

bool FSRDatasetViewExtension::WaitAndTakeCapture(FSRDatasetTemporalCaptureResult& OutResult, FString& OutError)
{
	FlushRenderingCommands();

	TSharedRef<FSRDatasetTemporalCaptureResult, ESPMode::ThreadSafe> Result = MakeShared<FSRDatasetTemporalCaptureResult, ESPMode::ThreadSafe>();
	TSharedRef<FString, ESPMode::ThreadSafe> RenderError = MakeShared<FString, ESPMode::ThreadSafe>();
	ENQUEUE_RENDER_COMMAND(SRDatasetReadTemporalInputs)(
		[this, Result, RenderError](FRHICommandListImmediate& RHICmdList)
		{
			TUniquePtr<FPendingReadbacks> Readbacks;
			{
				FScopeLock Lock(&StateMutex);
				Readbacks = MoveTemp(PendingReadbacks);
			}
			if (!Readbacks)
			{
				*RenderError = TEXT("The AfterDOF render hook did not produce temporal diagnostic buffers.");
				return;
			}

			RHICmdList.BlockUntilGPUIdle();
			Result->Size = Readbacks->Size;
			ReadFloatTexture(*Readbacks->SceneColor, Readbacks->Size, Result->SceneColor);
			if (!Readbacks->bColorOnly)
			{
				ReadFloatTexture(*Readbacks->VelocityRaw, Readbacks->Size, Result->VelocityRaw);
				ReadFloatTexture(*Readbacks->MotionFull, Readbacks->Size, Result->MotionFull);
				ReadFloatTexture(*Readbacks->Depth, Readbacks->Size, Result->Depth);
				ReadFloatTexture(*Readbacks->Translucency, Readbacks->Size, Result->Translucency);
				ReadFloatTexture(*Readbacks->ObjectId, Readbacks->Size, Result->ObjectId);
			}
			TArray<FLinearColor> MetadataPixels;
			ReadFloatTexture(*Readbacks->Metadata, FIntPoint(4, 21), MetadataPixels);
			DecodeMetadata(MetadataPixels, Result->Metadata);
		});
	FlushRenderingCommands();

	{
		FScopeLock Lock(&StateMutex);
		bRequestPending = false;
		bRequestConsumed = false;
		RequestedSize = FIntPoint::ZeroValue;
		RequestedDisplaySize = FIntPoint::ZeroValue;
		bRequestedMainViewOnly = false;
	}

	if (!RenderError->IsEmpty())
	{
		OutError = *RenderError;
		return false;
	}
	OutResult = MoveTemp(*Result);
	OutError.Reset();
	return true;
}

void FSRDatasetViewExtension::CancelCapture()
{
	// GPU readbacks are render-thread resources. Drain queued work before
	// releasing them so cancellation and world teardown cannot leave a stale
	// request that poisons the next job in the same editor process.
	FlushRenderingCommands();
	FScopeLock Lock(&StateMutex);
	PendingReadbacks.Reset();
	bRequestPending = false;
	bRequestConsumed = false;
	RequestedSize = FIntPoint::ZeroValue;
	RequestedDisplaySize = FIntPoint::ZeroValue;
	bRequestedMainViewOnly = false;
}

void FSRDatasetViewExtension::ReadFloatTexture(
	FRHIGPUTextureReadback& Readback,
	const FIntPoint Size,
	TArray<FLinearColor>& OutPixels)
{
	int32 RowPitchPixels = 0;
	int32 BufferHeight = 0;
	const FLinearColor* Source = static_cast<const FLinearColor*>(Readback.Lock(RowPitchPixels, &BufferHeight));
	check(Source && RowPitchPixels >= Size.X && BufferHeight >= Size.Y);
	OutPixels.SetNumUninitialized(Size.X * Size.Y);
	for (int32 Y = 0; Y < Size.Y; ++Y)
	{
		FMemory::Memcpy(OutPixels.GetData() + Y * Size.X, Source + Y * RowPitchPixels, Size.X * sizeof(FLinearColor));
	}
	Readback.Unlock();
}

void FSRDatasetViewExtension::DecodeMetadata(
	const TArray<FLinearColor>& Pixels,
	FSRDatasetTemporalFrameMetadata& OutMetadata)
{
	if (Pixels.Num() != 84)
	{
		return;
	}
	const auto MatrixAt = [&Pixels](const int32 Row)
	{
		FMatrix44f Matrix;
		for (int32 MatrixRow = 0; MatrixRow < 4; ++MatrixRow)
		{
			const FLinearColor& Value = Pixels[Row * 4 + MatrixRow];
			Matrix.M[MatrixRow][0] = Value.R;
			Matrix.M[MatrixRow][1] = Value.G;
			Matrix.M[MatrixRow][2] = Value.B;
			Matrix.M[MatrixRow][3] = Value.A;
		}
		return Matrix;
	};

	OutMetadata.ViewToClipJittered = MatrixAt(0);
	OutMetadata.ViewToClipUnjittered = MatrixAt(1);
	OutMetadata.PreviousViewToClip = MatrixAt(2);
	OutMetadata.ClipToPreviousClipUnjittered = MatrixAt(3);
	OutMetadata.ClipToPreviousClipJittered = MatrixAt(4);
	OutMetadata.TranslatedWorldToViewCurrent = MatrixAt(6);
	OutMetadata.ViewToTranslatedWorldCurrent = MatrixAt(7);
	OutMetadata.TranslatedWorldToClipCurrentJittered = MatrixAt(8);
	OutMetadata.ClipToTranslatedWorldCurrentJittered = MatrixAt(9);
	OutMetadata.TranslatedWorldToViewPrevious = MatrixAt(10);
	OutMetadata.ViewToTranslatedWorldPrevious = MatrixAt(11);
	OutMetadata.TranslatedWorldToClipPreviousJittered = MatrixAt(12);
	OutMetadata.PreviousViewToClipUnjittered = MatrixAt(13);
	OutMetadata.TranslatedWorldToClipCurrentUnjittered = MatrixAt(14);
	OutMetadata.TranslatedWorldToClipPreviousUnjittered = MatrixAt(15);
	const FLinearColor& Meta0 = Pixels[5 * 4 + 0];
	const FLinearColor& Meta1 = Pixels[5 * 4 + 1];
	const FLinearColor& Meta2 = Pixels[5 * 4 + 2];
	OutMetadata.PreExposure = Meta0.R;
	OutMetadata.OneOverPreExposure = Meta0.G;
	OutMetadata.JitterCurrentNDC = FVector2f(Meta0.B, Meta0.A);
	OutMetadata.JitterPreviousNDC = FVector2f(Meta1.R, Meta1.G);
	OutMetadata.DeltaTimeSeconds = Meta1.B;
	OutMetadata.GameTimeSeconds = Meta1.A;
	OutMetadata.NearPlane = Meta2.R;
	OutMetadata.OrthoFarPlane = Meta2.G;
	OutMetadata.ViewSize = FIntPoint(FMath::RoundToInt(Meta2.B), FMath::RoundToInt(Meta2.A));
	const auto Vector3At = [&Pixels](const int32 Row, const int32 Column)
	{
		const FLinearColor& Value = Pixels[Row * 4 + Column];
		return FVector3f(Value.R, Value.G, Value.B);
	};
	OutMetadata.WorldViewOriginHighCurrent = Vector3At(16, 0);
	OutMetadata.WorldViewOriginLowCurrent = Vector3At(16, 1);
	OutMetadata.PreViewTranslationHighCurrent = Vector3At(16, 2);
	OutMetadata.PreViewTranslationLowCurrent = Vector3At(16, 3);
	OutMetadata.WorldViewOriginHighPrevious = Vector3At(17, 0);
	OutMetadata.WorldViewOriginLowPrevious = Vector3At(17, 1);
	OutMetadata.PreViewTranslationHighPrevious = Vector3At(17, 2);
	OutMetadata.PreViewTranslationLowPrevious = Vector3At(17, 3);
	const FLinearColor& ViewRect = Pixels[18 * 4 + 0];
	const FLinearColor& Buffer = Pixels[18 * 4 + 1];
	const FLinearColor& Resolution = Pixels[18 * 4 + 2];
	const FLinearColor& Frame = Pixels[19 * 4 + 0];
	OutMetadata.ViewRectMin = FIntPoint(FMath::RoundToInt(ViewRect.R), FMath::RoundToInt(ViewRect.G));
	OutMetadata.BufferSize = FIntPoint(FMath::RoundToInt(Buffer.R), FMath::RoundToInt(Buffer.G));
	OutMetadata.ResolutionFraction = Resolution.R;
	OutMetadata.InvResolutionFraction = Resolution.G;
	OutMetadata.MaterialTextureMipBias = Pixels[20 * 4 + 0].R;
	OutMetadata.RenderFrameNumber = static_cast<uint32>(FMath::Max(0, FMath::RoundToInt(Frame.R)));
	OutMetadata.StateFrameIndex = static_cast<uint32>(FMath::Max(0, FMath::RoundToInt(Frame.G)));
	OutMetadata.StateFrameIndexMod8 = static_cast<uint32>(FMath::Max(0, FMath::RoundToInt(Frame.B)));
	OutMetadata.bValid = true;
}

TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> GetSRDatasetViewExtension()
{
	return GSRDatasetViewExtension;
}

void SetSRDatasetViewExtension(TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> InExtension)
{
	GSRDatasetViewExtension = MoveTemp(InExtension);
}

TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> GetSRDatasetTonemapViewExtension()
{
	return GSRDatasetTonemapViewExtension;
}

void SetSRDatasetTonemapViewExtension(TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> InExtension)
{
	GSRDatasetTonemapViewExtension = MoveTemp(InExtension);
}
