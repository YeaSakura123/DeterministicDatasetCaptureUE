#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SRDatasetTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSRDatasetJobValidationTest,
	"SuperResolutionDataset.Job.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSRDatasetJobValidationTest::RunTest(const FString& Parameters)
{
	FSRDatasetCaptureJob Job;
	FString Error;
	TestTrue(TEXT("Default job is valid"), Job.Validate(Error));
	TestTrue(TEXT("30 fps delta is 1/30"), FMath::IsNearlyEqual(Job.GetFixedDeltaSeconds(), 1.0 / 30.0));

	Job.EndFrame = -1;
	TestFalse(TEXT("Invalid frame range is rejected"), Job.Validate(Error));
	TestFalse(TEXT("Invalid range reports an error"), Error.IsEmpty());

	Job = FSRDatasetCaptureJob();
	Job.LRResolution = FIntPoint(Job.HRResolution.X + 1, Job.HRResolution.Y);
	TestFalse(TEXT("LR larger than HR is rejected"), Job.Validate(Error));
	return true;
}

#endif
