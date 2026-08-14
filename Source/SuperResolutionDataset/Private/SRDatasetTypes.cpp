#include "SRDatasetTypes.h"

bool FSRDatasetCaptureJob::Validate(FString& OutError) const
{
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
