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
	if (bCaptureMainViewTemporalDiagnostics && !bCaptureTemporalDiagnostics)
	{
		OutError = TEXT("bCaptureMainViewTemporalDiagnostics requires bCaptureTemporalDiagnostics=true.");
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
	if (bUseLastCapturedEndpointTransforms && ReplayPass != ESRDatasetReplayPass::FrameGenerationEndpoints)
	{
		OutError = TEXT("Endpoint previous-transform overrides require ReplayPass=FrameGenerationEndpoints.");
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
	if (ReplayPass == ESRDatasetReplayPass::FrameGenerationEndpoints &&
		(!bCaptureMainViewHUDlessColor || !bSuppressMainViewOnUncapturedFrames ||
		 !bUseLastCapturedEndpointTransforms || CapturedFrameCount < 2))
	{
		OutError = TEXT("FrameGenerationEndpoints requires HUD-less Main View color, suppressed intermediate renders, endpoint transform overrides, and at least two endpoints.");
		return false;
	}
	if (ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate &&
		(!bCaptureMainViewHUDlessColor || !bSuppressMainViewOnUncapturedFrames ||
		 bUseLastCapturedEndpointTransforms || CaptureFrameOffset == 0 || CapturedFrameCount != 1))
	{
		OutError = TEXT("FrameGenerationIntermediate requires exactly one offset HUD-less Main View capture per process, suppressed uncaptured renders, and no endpoint override.");
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
