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

	Job = FSRDatasetCaptureJob();
	Job.StreamingWaitSeconds = 0.0f;
	TestFalse(TEXT("Enabled streaming barrier rejects a zero timeout"), Job.Validate(Error));
	Job.bBlockOnStreamingBeforeCapture = false;
	TestTrue(TEXT("Disabled streaming barrier ignores its timeout"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.ContractVersion = TEXT("nr-sr-data-v2");
	TestFalse(TEXT("Unimplemented temporal contract is rejected instead of emitting incomplete data"), Job.Validate(Error));
	TestTrue(TEXT("Temporal rejection explains the certification boundary"), Error.Contains(TEXT("not certified")));

	Job = FSRDatasetCaptureJob();
	Job.bCaptureTemporalDiagnostics = true;
	TestFalse(TEXT("Temporal diagnostics reject derived LR"), Job.Validate(Error));
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	TestTrue(TEXT("Temporal diagnostics accept native LR"), Job.Validate(Error));
	Job.bCaptureMainViewTemporalDiagnostics = true;
	TestTrue(TEXT("Main View diagnostics accept a native LR job"), Job.Validate(Error));
	Job.CameraActorTag = TEXT("DatasetCamera");
	TestFalse(TEXT("Main View diagnostics reject a camera that is not guaranteed to drive the player view"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureMainViewTemporalDiagnostics = true;
	TestFalse(TEXT("Main View diagnostics require the temporal diagnostic output set"), Job.Validate(Error));
	Job.bCaptureTemporalDiagnostics = true;
	Job.LRResolution = FIntPoint(640, 400);
	TestFalse(TEXT("Main View diagnostics reject mismatched HR/LR aspect ratios"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.bEnableSemanticValidationFixture = true;
	TestFalse(TEXT("Semantic fixture requires Main View temporal diagnostics"), Job.Validate(Error));
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	Job.bCaptureMainViewTemporalDiagnostics = true;
	Job.EndFrame = Job.StartFrame;
	TestFalse(TEXT("Semantic fixture requires two frames"), Job.Validate(Error));
	Job.EndFrame = Job.StartFrame + 1;
	Job.FrameStep = 2;
	TestFalse(TEXT("Semantic fixture requires consecutive frames"), Job.Validate(Error));
	Job.FrameStep = 1;
	TestTrue(TEXT("Semantic fixture accepts a two-frame Main View job"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.bCaptureReferenceHR = true;
	TestFalse(TEXT("Reference HR requires temporal HDR extraction"), Job.Validate(Error));
	Job.bCaptureTemporalDiagnostics = true;
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.ReferenceHRScale = 1;
	TestFalse(TEXT("Reference HR rejects a non-supersampled scale"), Job.Validate(Error));
	Job.ReferenceHRScale = 2;
	TestTrue(TEXT("Reference HR accepts a 2x spatial supersample"), Job.Validate(Error));
	Job.HRResolution = FIntPoint(9000, 4500);
	TestFalse(TEXT("Reference HR rejects dimensions beyond the RHI texture limit"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.bCaptureMainViewHUDlessColor = true;
	TestFalse(TEXT("HUD-less color requires the real Main View"), Job.Validate(Error));
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	Job.bCaptureMainViewTemporalDiagnostics = true;
	TestTrue(TEXT("HUD-less color accepts a Main View diagnostic job"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	Job.bCaptureMainViewTemporalDiagnostics = true;
	Job.FrameStep = 2;
	Job.EndFrame = 2;
	Job.bSuppressMainViewOnUncapturedFrames = true;
	Job.bUseLastCapturedEndpointTransforms = true;
	TestFalse(TEXT("Endpoint transform override is rejected without an endpoint replay role"), Job.Validate(Error));
	Job.ReplayPass = ESRDatasetReplayPass::FrameGenerationEndpoints;
	TestFalse(TEXT("Endpoint replay requires HUD-less display color"), Job.Validate(Error));
	Job.bCaptureMainViewHUDlessColor = true;
	TestTrue(TEXT("Endpoint replay accepts render suppression and prior captured transforms"), Job.Validate(Error));
	Job.EndFrame = 1;
	TestFalse(TEXT("Endpoint job must end on a captured frame"), Job.Validate(Error));
	Job.EndFrame = 2;
	Job.CaptureFrameOffset = 2;
	TestFalse(TEXT("Capture phase must be smaller than FrameStep"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.ReplayPass = ESRDatasetReplayPass::FrameGenerationIntermediate;
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	Job.bCaptureMainViewTemporalDiagnostics = true;
	Job.bCaptureMainViewHUDlessColor = true;
	Job.FrameStep = 2;
	Job.StartFrame = 0;
	Job.EndFrame = 1;
	Job.CaptureFrameOffset = 1;
	Job.bSuppressMainViewOnUncapturedFrames = true;
	TestTrue(TEXT("Intermediate replay accepts one isolated offset capture"), Job.Validate(Error));
	Job.EndFrame = 3;
	TestFalse(TEXT("Intermediate replay rejects multiple captures in one retained View State"), Job.Validate(Error));
	TestTrue(TEXT("Intermediate rejection explains the single-process isolation rule"), Error.Contains(TEXT("exactly one")));
	Job.EndFrame = 1;
	Job.CaptureFrameOffset = 0;
	TestFalse(TEXT("Intermediate replay rejects an endpoint phase"), Job.Validate(Error));
	Job.CaptureFrameOffset = 1;
	Job.bUseLastCapturedEndpointTransforms = true;
	TestFalse(TEXT("Intermediate replay rejects endpoint previous-transform overrides"), Job.Validate(Error));
	return true;
}

#endif
