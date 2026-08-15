#include "SRDatasetTypes.h"

bool FSRDatasetCaptureJob::Validate(FString& OutError) const
{
	if (!ContractVersion.Equals(TEXT("spatial-sr-data-v1"), ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(
			TEXT("ContractVersion '%s' is not certified for direct capture. Use spatial-sr-data-v1; enable the experimental Main View diagnostics under that contract, and assemble validated FG replay passes offline without fabricating missing nr-fg-data-v1 buffers."),
			*ContractVersion);
		return false;
	}
	if (JobName.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("JobName cannot be empty.");
		return false;
	}
	if (bRequireSceneControlPreflight && !bRunSceneControlPreflight)
	{
		OutError = TEXT("bRequireSceneControlPreflight requires bRunSceneControlPreflight=true.");
		return false;
	}
	if (bRequireControllableState && !bRequireSceneControlPreflight)
	{
		OutError = TEXT("bRequireControllableState requires bRequireSceneControlPreflight=true.");
		return false;
	}
	const auto ValidateSceneControlRules = [&OutError](
		const TArray<FString>& Rules,
		const TCHAR* FieldName)
	{
		TSet<FString> UniqueRules;
		for (const FString& RawRule : Rules)
		{
			const FString Rule = RawRule.TrimStartAndEnd();
			const int32 FirstWildcard = Rule.Find(TEXT("*"), ESearchCase::CaseSensitive);
			const FString ClassPrefix = FirstWildcard == INDEX_NONE ? Rule : Rule.Left(FirstWildcard);
			const int32 LastClassSeparator = ClassPrefix.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (Rule.IsEmpty() || !Rule.StartsWith(TEXT("/")) ||
				(FirstWildcard != INDEX_NONE && FirstWildcard != Rule.Len() - 1) ||
				LastClassSeparator == INDEX_NONE || LastClassSeparator >= ClassPrefix.Len() - 1)
			{
				OutError = FString::Printf(
					TEXT("%s contains invalid or over-broad rule '%s'. Use an absolute module/asset class path and, if needed, one trailing '*' after a non-empty class prefix."),
					FieldName,
					*RawRule);
				return false;
			}
			if (UniqueRules.Contains(Rule))
			{
				OutError = FString::Printf(
					TEXT("%s contains duplicate rule '%s'."),
					FieldName,
					*Rule);
				return false;
			}
			UniqueRules.Add(Rule);
		}
		return true;
	};
	if (!ValidateSceneControlRules(
			SceneControlAllowedTickingActorClassPaths,
			TEXT("SceneControlAllowedTickingActorClassPaths")) ||
		!ValidateSceneControlRules(
			SceneControlAllowedTickingComponentClassPaths,
			TEXT("SceneControlAllowedTickingComponentClassPaths")) ||
		!ValidateSceneControlRules(
			SceneControlAllowedNiagaraDataInterfaceClassPaths,
			TEXT("SceneControlAllowedNiagaraDataInterfaceClassPaths")) ||
		!ValidateSceneControlRules(
			SceneControlAllowedMaterialExpressionClassPaths,
			TEXT("SceneControlAllowedMaterialExpressionClassPaths")))
	{
		return false;
	}
	if (StartFrame < 0 || EndFrame < StartFrame)
	{
		OutError = TEXT("Frame range must satisfy 0 <= StartFrame <= EndFrame.");
		return false;
	}
	if (FrameStep <= 0)
	{
		OutError = TEXT("FrameStep must be greater than zero.");
		return false;
	}
	if (CaptureFrameOffset < 0 || CaptureFrameOffset >= FrameStep)
	{
		OutError = TEXT("CaptureFrameOffset must satisfy 0 <= offset < FrameStep.");
		return false;
	}
	if (CaptureFrameRateNumerator <= 0 || CaptureFrameRateDenominator <= 0)
	{
		OutError = TEXT("Capture frame-rate numerator and denominator must be greater than zero.");
		return false;
	}
	if (WarmupFrames < 0)
	{
		OutError = TEXT("WarmupFrames cannot be negative.");
		return false;
	}
	if (bUseDeterministicCameraTransform &&
		(DeterministicCameraLocationCm.ContainsNaN() ||
		 DeterministicCameraRotationDegrees.ContainsNaN() ||
		 DeterministicCameraTranslationPerLogicalFrameCm.ContainsNaN() ||
		 !FMath::IsFinite(DeterministicCameraFOVDegrees) ||
		 DeterministicCameraFOVDegrees < 5.0f ||
		 DeterministicCameraFOVDegrees > 170.0f))
	{
		OutError = TEXT("Deterministic camera transform/trajectory must be finite and its FOV must be in [5, 170] degrees.");
		return false;
	}
	if (bLockTemporalJitterToLogicalFrame &&
		(TemporalJitterSequenceLength < 1 || TemporalJitterSequenceLength > 8))
	{
		OutError = TEXT("TemporalJitterSequenceLength must be in [1, 8] when logical-frame jitter locking is enabled.");
		return false;
	}
	if (bBlockOnStreamingBeforeCapture &&
		(!FMath::IsFinite(StreamingWaitSeconds) || StreamingWaitSeconds <= 0.0f || StreamingWaitSeconds > 3600.0f))
	{
		OutError = TEXT("StreamingWaitSeconds must be finite and in (0, 3600] when the streaming barrier is enabled.");
		return false;
	}
	if (HRResolution.X <= 0 || HRResolution.Y <= 0 || LRResolution.X <= 0 || LRResolution.Y <= 0)
	{
		OutError = TEXT("HR and LR resolutions must be positive.");
		return false;
	}
	if (LRResolution.X > HRResolution.X || LRResolution.Y > HRResolution.Y)
	{
		OutError = TEXT("LRResolution cannot exceed HRResolution.");
		return false;
	}
	if (bCaptureTemporalDiagnostics && LRMode != ESRDatasetLRMode::NativeRender)
	{
		OutError = TEXT("bCaptureTemporalDiagnostics requires LRMode=NativeRender so Velocity, Depth and HDR Color come from the real LR view.");
		return false;
	}
	if (bAssignStableInstanceIds && !bCaptureTemporalDiagnostics)
	{
		OutError = TEXT("bAssignStableInstanceIds requires temporal diagnostics so the labeled object_id raster is captured.");
		return false;
	}
	if (bAssignStableInstanceIds &&
		(bEnableSemanticValidationFixture || bValidateNonFixtureSkeletalAnimation ||
		 bValidateProjectAnimatedMaterial))
	{
		OutError = TEXT("Stable production instance IDs cannot be combined with semantic or project-probe validation fixtures that reserve Custom Stencil values.");
		return false;
	}
	if (AuxiliaryCaptureOrder == ESRDatasetAuxiliaryCaptureOrder::LowResolutionFirst &&
		LRMode != ESRDatasetLRMode::NativeRender)
	{
		OutError = TEXT("LowResolutionFirst requires LRMode=NativeRender because a derived LR image has no independent render submission to reorder.");
		return false;
	}
	if (bCaptureMainViewTemporalDiagnostics && !bCaptureTemporalDiagnostics)
	{
		OutError = TEXT("bCaptureMainViewTemporalDiagnostics requires bCaptureTemporalDiagnostics=true.");
		return false;
	}
	if (bCaptureSceneCaptureLRComparison &&
		(!bCaptureTemporalDiagnostics || !bCaptureMainViewTemporalDiagnostics ||
		 LRMode != ESRDatasetLRMode::NativeRender))
	{
		OutError = TEXT("bCaptureSceneCaptureLRComparison requires temporal diagnostics from the real Main View and LRMode=NativeRender.");
		return false;
	}
	if (bValidateMainViewSceneCapturePixelDomain && !bCaptureSceneCaptureLRComparison)
	{
		OutError = TEXT("bValidateMainViewSceneCapturePixelDomain requires bCaptureSceneCaptureLRComparison=true.");
		return false;
	}
	if (bValidateMainViewSceneCapturePixelDomain &&
		(!bEnableSemanticValidationFixture ||
		 SemanticMotionScenario != ESRDatasetSemanticMotionScenario::Static ||
		 !bUseDeterministicCameraTransform || !bLockExposure ||
		 !bDisableMotionBlur || !bLockTemporalJitterToLogicalFrame))
	{
		OutError = TEXT("The Main View/SceneCapture pixel-domain gate requires the Static semantic fixture, a deterministic camera, locked exposure, disabled motion blur and logical-frame jitter locking.");
		return false;
	}
	if (bCaptureReferenceHR && !bCaptureTemporalDiagnostics)
	{
		OutError = TEXT("bCaptureReferenceHR requires bCaptureTemporalDiagnostics=true for linear HDR extraction.");
		return false;
	}
	if (bCaptureMainViewHUDlessColor && !bCaptureMainViewTemporalDiagnostics)
	{
		OutError = TEXT("bCaptureMainViewHUDlessColor requires real Main View temporal diagnostics.");
		return false;
	}
	if (bCaptureUIColorAlpha && !bCaptureMainViewHUDlessColor)
	{
		OutError = TEXT("bCaptureUIColorAlpha requires bCaptureMainViewHUDlessColor=true so scene color and UI are available as separate layers.");
		return false;
	}
	if (bSuppressMainViewOnUncapturedFrames &&
		(!bCaptureMainViewTemporalDiagnostics || FrameStep <= 1))
	{
		OutError = TEXT("Suppressing uncaptured Main Views requires Main View diagnostics and FrameStep > 1.");
		return false;
	}
	if (bSuppressMainViewOnUncapturedFrames &&
		(EndFrame - StartFrame) % FrameStep != CaptureFrameOffset)
	{
		OutError = TEXT("A suppressed-render endpoint job must end on a captured frame.");
		return false;
	}
	if (bUseLastCapturedEndpointTransforms && !bSuppressMainViewOnUncapturedFrames)
	{
		OutError = TEXT("Endpoint previous-transform overrides require suppressed uncaptured Main Views.");
		return false;
	}
	if (bUseLastCapturedEndpointTransforms &&
		ReplayPass != ESRDatasetReplayPass::FrameGenerationEndpoints &&
		ReplayPass != ESRDatasetReplayPass::FrameGenerationReverseEndpoints)
	{
		OutError = TEXT("Endpoint previous-transform overrides require a forward or reverse endpoint replay pass.");
		return false;
	}
	if (bCaptureReferenceHR && (ReferenceHRScale < 2 || ReferenceHRScale > 4))
	{
		OutError = TEXT("ReferenceHRScale must be between 2 and 4.");
		return false;
	}
	if (bCaptureReferenceHR &&
		(static_cast<int64>(HRResolution.X) * ReferenceHRScale > 16384 ||
		 static_cast<int64>(HRResolution.Y) * ReferenceHRScale > 16384))
	{
		OutError = TEXT("The supersampled reference render cannot exceed 16384 pixels on either axis.");
		return false;
	}
	if (bCaptureMainViewTemporalDiagnostics && !CameraActorTag.IsNone())
	{
		OutError = TEXT("Main View temporal capture cannot use CameraActorTag; drive the player/Sequencer camera used by the real Main View.");
		return false;
	}
	if (bCaptureMainViewTemporalDiagnostics &&
		static_cast<int64>(HRResolution.X) * LRResolution.Y != static_cast<int64>(HRResolution.Y) * LRResolution.X)
	{
		OutError = TEXT("Main View temporal capture requires matching HR/LR aspect ratios.");
		return false;
	}
	if (bEnableSemanticValidationFixture && !bCaptureMainViewTemporalDiagnostics)
	{
		OutError = TEXT("bEnableSemanticValidationFixture requires real Main View temporal diagnostics.");
		return false;
	}
	if (SemanticMotionScenario != ESRDatasetSemanticMotionScenario::LegacyCameraRelative &&
		(!bEnableSemanticValidationFixture || !bUseDeterministicCameraTransform ||
		 ReplayPass != ESRDatasetReplayPass::Standard || FrameStep != 1))
	{
		OutError = TEXT("An analytic semantic motion scenario requires the semantic fixture, a deterministic camera, Standard replay, and consecutive frames.");
		return false;
	}
	const bool bSemanticCameraMotion =
		SemanticMotionScenario == ESRDatasetSemanticMotionScenario::CameraOnly ||
		SemanticMotionScenario == ESRDatasetSemanticMotionScenario::Mixed;
	const bool bConfiguredCameraTranslation =
		!DeterministicCameraTranslationPerLogicalFrameCm.IsNearlyZero(UE_SMALL_NUMBER);
	if (SemanticMotionScenario != ESRDatasetSemanticMotionScenario::LegacyCameraRelative &&
		bSemanticCameraMotion != bConfiguredCameraTranslation)
	{
		OutError = bSemanticCameraMotion
			? TEXT("CameraOnly/Mixed semantic validation requires a non-zero deterministic camera translation per logical frame.")
			: TEXT("Static/ObjectOnly semantic validation requires zero deterministic camera translation per logical frame.");
		return false;
	}
	const int32 PoseCacheFrameCount = EndFrame - StartFrame + 1;
	if ((!SkeletalPoseCacheInputFile.IsEmpty() || !SkeletalPoseCacheOutputFile.IsEmpty()) &&
		!bCacheSkeletalAnimationPosesForReplay)
	{
		OutError = TEXT("Skeletal pose-cache input/output files require bCacheSkeletalAnimationPosesForReplay=true.");
		return false;
	}
	if (!SkeletalPoseCacheInputFile.IsEmpty() && !SkeletalPoseCacheOutputFile.IsEmpty())
	{
		OutError = TEXT("A capture job may load or write a skeletal pose-cache artifact, but not both.");
		return false;
	}
	if (bCacheSkeletalAnimationPosesForReplay && WarmupFrames < PoseCacheFrameCount)
	{
		OutError = FString::Printf(
			TEXT("Skeletal pose-cache replay requires at least %d warmup frames for the inclusive logical range."),
			PoseCacheFrameCount);
		return false;
	}
	if (bValidateNonFixtureSkeletalAnimation &&
		(!bCacheSkeletalAnimationPosesForReplay || !bCaptureMainViewTemporalDiagnostics ||
		 !bUseLastCapturedEndpointTransforms || bEnableSemanticValidationFixture ||
		 (ReplayPass != ESRDatasetReplayPass::FrameGenerationEndpoints &&
		  ReplayPass != ESRDatasetReplayPass::FrameGenerationReverseEndpoints)))
	{
		OutError = TEXT("Non-fixture skeletal validation requires pose-cache replay, Main View diagnostics, endpoint previous-state overrides, no semantic fixture, and a forward or reverse endpoint replay role.");
		return false;
	}
	if (bValidateNonFixtureSkeletalAnimation && !NonFixtureSkeletalValidationActorClass.IsValid())
	{
		OutError = TEXT("Non-fixture skeletal validation requires a project-authored NonFixtureSkeletalValidationActorClass.");
		return false;
	}
	if (bValidateProjectAnimatedMaterial &&
		(!bLockMaterialTimeToLogicalFrame || !bCaptureMainViewTemporalDiagnostics ||
		 !bCaptureMainViewHUDlessColor || !bUseDeterministicCameraTransform ||
		 bEnableSemanticValidationFixture ||
		 (ReplayPass != ESRDatasetReplayPass::FrameGenerationEndpoints &&
		  ReplayPass != ESRDatasetReplayPass::FrameGenerationReverseEndpoints)))
	{
		OutError = TEXT("Project animated-material validation requires logical material time, deterministic camera, HUD-less Main View temporal diagnostics, no semantic fixture, and a forward or reverse endpoint role.");
		return false;
	}
	if (bValidateProjectAnimatedMaterial &&
		(!ProjectAnimatedMaterialValidationMaterial.IsValid() ||
		 !ProjectAnimatedMaterialValidationMaterial.ToString().StartsWith(TEXT("/Game/"))))
	{
		OutError = TEXT("Project animated-material validation requires a /Game ProjectAnimatedMaterialValidationMaterial.");
		return false;
	}
	const int32 FirstCapturedFrame = StartFrame + CaptureFrameOffset;
	const int32 CapturedFrameCount = FirstCapturedFrame > EndFrame
		? 0
		: 1 + (EndFrame - FirstCapturedFrame) / FrameStep;
	const int32 RequiredSemanticFrames = ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate ? 1 : 2;
	if (bEnableSemanticValidationFixture && CapturedFrameCount < RequiredSemanticFrames)
	{
		OutError = FString::Printf(
			TEXT("This semantic validation replay role requires at least %d captured frame(s)."),
			RequiredSemanticFrames);
		return false;
	}
	if (bValidateTemporalJitterSignCoverage &&
		(!bEnableSemanticValidationFixture ||
		 SemanticMotionScenario != ESRDatasetSemanticMotionScenario::Static ||
		 !bLockTemporalJitterToLogicalFrame ||
		 TemporalJitterSequenceLength < 4 ||
		 CapturedFrameCount < TemporalJitterSequenceLength ||
		 FrameStep != 1 || ReplayPass != ESRDatasetReplayPass::Standard))
	{
		OutError = TEXT("Temporal jitter sign coverage requires a Static semantic fixture, Standard consecutive replay, logical-frame jitter locking, a sequence length of at least four, and at least one full captured cycle.");
		return false;
	}
	if (ReplayPass == ESRDatasetReplayPass::FrameGenerationEndpoints &&
		(!bCaptureMainViewHUDlessColor || !bSuppressMainViewOnUncapturedFrames ||
		 !bUseLastCapturedEndpointTransforms || !bLockTemporalJitterToLogicalFrame || CapturedFrameCount < 2))
	{
		OutError = TEXT("FrameGenerationEndpoints requires HUD-less Main View color, logical-frame jitter locking, suppressed intermediate renders, endpoint transform overrides, and at least two endpoints.");
		return false;
	}
	if (ReplayPass == ESRDatasetReplayPass::FrameGenerationReverseEndpoints &&
		(!bCaptureMainViewHUDlessColor || !bSuppressMainViewOnUncapturedFrames ||
		 !bUseLastCapturedEndpointTransforms || !bLockTemporalJitterToLogicalFrame || CapturedFrameCount < 2))
	{
		OutError = TEXT("FrameGenerationReverseEndpoints requires HUD-less Main View color, logical-frame jitter locking, suppressed intermediate renders, endpoint transform overrides, and at least two endpoints.");
		return false;
	}
	if (ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate &&
		(!bCaptureMainViewHUDlessColor || !bSuppressMainViewOnUncapturedFrames ||
		 bUseLastCapturedEndpointTransforms || !bLockTemporalJitterToLogicalFrame ||
		 CaptureFrameOffset == 0 || CapturedFrameCount != 1))
	{
		OutError = TEXT("FrameGenerationIntermediate requires exactly one offset HUD-less Main View capture per process, logical-frame jitter locking, suppressed uncaptured renders, and no endpoint override.");
		return false;
	}
	if (OutputDirectory.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("OutputDirectory cannot be empty.");
		return false;
	}

	OutError.Reset();
	return true;
}

double FSRDatasetCaptureJob::GetFixedDeltaSeconds() const
{
	return static_cast<double>(CaptureFrameRateDenominator) / static_cast<double>(CaptureFrameRateNumerator);
}
