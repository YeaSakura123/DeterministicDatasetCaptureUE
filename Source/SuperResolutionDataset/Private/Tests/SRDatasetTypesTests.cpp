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
	Job.AuxiliaryCaptureOrder = ESRDatasetAuxiliaryCaptureOrder::LowResolutionFirst;
	TestFalse(TEXT("Low-resolution-first rejects a derived LR image with no render submission"), Job.Validate(Error));
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	TestTrue(TEXT("Low-resolution-first accepts an independently rendered LR view"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.StreamingWaitSeconds = 0.0f;
	TestFalse(TEXT("Enabled streaming barrier rejects a zero timeout"), Job.Validate(Error));
	Job.bBlockOnStreamingBeforeCapture = false;
	TestTrue(TEXT("Disabled streaming barrier ignores its timeout"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.bRunSceneControlPreflight = false;
	Job.bRequireSceneControlPreflight = true;
	TestFalse(TEXT("Required scene-control preflight cannot be disabled"), Job.Validate(Error));
	Job.bRunSceneControlPreflight = true;
	Job.bRequireSceneControlPreflight = false;
	Job.bRequireControllableState = true;
	TestFalse(TEXT("Required controllable state requires strict scene-control preflight"), Job.Validate(Error));
	Job.bRequireSceneControlPreflight = true;
	Job.bRequireControllableState = false;
	Job.SceneControlAllowedTickingActorClassPaths = { TEXT("/Script/Engine.SkyAtmosphere*") };
	TestTrue(TEXT("Scene-control rules accept an absolute class prefix with one trailing wildcard"), Job.Validate(Error));
	Job.SceneControlAllowedTickingActorClassPaths = { TEXT("Script/Engine.Actor") };
	TestFalse(TEXT("Scene-control rules reject relative class paths"), Job.Validate(Error));
	Job.SceneControlAllowedTickingActorClassPaths = { TEXT("/Script/*/Actor") };
	TestFalse(TEXT("Scene-control rules reject embedded wildcards"), Job.Validate(Error));
	Job.SceneControlAllowedTickingActorClassPaths = { TEXT("/Script/Engine.*") };
	TestFalse(TEXT("Scene-control rules reject module-wide wildcard exceptions"), Job.Validate(Error));
	Job.SceneControlAllowedTickingActorClassPaths = { TEXT("/Script/Engine.Actor"), TEXT("/Script/Engine.Actor") };
	TestFalse(TEXT("Scene-control rules reject duplicate exceptions"), Job.Validate(Error));

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
	Job.bCaptureSceneCaptureLRComparison = true;
	TestTrue(TEXT("Main View diagnostics accept a paired native-LR SceneCapture extraction"), Job.Validate(Error));
	Job.CameraActorTag = TEXT("DatasetCamera");
	TestFalse(TEXT("Main View diagnostics reject a camera that is not guaranteed to drive the player view"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.bAssignStableInstanceIds = true;
	TestFalse(TEXT("Stable instance IDs require the temporal object-ID raster"), Job.Validate(Error));
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	TestTrue(TEXT("Stable instance IDs accept a native temporal capture"), Job.Validate(Error));
	Job.bEnableSemanticValidationFixture = true;
	TestFalse(TEXT("Stable instance IDs reject fixtures that reserve Custom Stencil values"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.bAllowDynamicInstanceIdTopology = true;
	TestFalse(TEXT("Dynamic instance topology requires stable instance IDs"), Job.Validate(Error));
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	Job.bAssignStableInstanceIds = true;
	Job.bResume = true;
	TestFalse(TEXT("Dynamic instance topology rejects resume without an allocator journal"), Job.Validate(Error));
	Job.bResume = false;
	Job.bValidateDynamicInstanceIdTopology = true;
	Job.EndFrame = Job.StartFrame;
	TestFalse(TEXT("Dynamic instance validation requires three selected frames"), Job.Validate(Error));
	Job.EndFrame = Job.StartFrame + 2;
	TestTrue(TEXT("Dynamic instance validation accepts three consecutive stable-ID frames"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.ControllableStateCacheOutputFile = TEXT("Saved/SRDataset/cache.json");
	TestFalse(TEXT("Controllable cache paths require cache replay"), Job.Validate(Error));
	Job.bCacheControllableStatesForReplay = true;
	Job.bResume = false;
	TestTrue(TEXT("A Standard job may record a controllable state cache"), Job.Validate(Error));
	Job.ControllableStateCacheInputFile = TEXT("Saved/SRDataset/cache.json");
	TestFalse(TEXT("Controllable state cache rejects simultaneous input and output"), Job.Validate(Error));
	Job.ControllableStateCacheOutputFile.Reset();
	TestTrue(TEXT("A job may load a controllable state cache"), Job.Validate(Error));
	Job.bValidateControllableStateCache = true;
	Job.bUseDeterministicCameraTransform = true;
	Job.EndFrame = Job.StartFrame;
	TestFalse(TEXT("Controllable state-cache validation requires two frames"), Job.Validate(Error));
	Job.EndFrame = Job.StartFrame + 1;
	TestTrue(TEXT("Controllable state-cache validation accepts two consecutive frames"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.NiagaraSimCacheOutputFile = TEXT("Saved/SRDataset/niagara.srncache");
	TestFalse(TEXT("Niagara cache paths require native cache replay"), Job.Validate(Error));
	Job.bCacheNiagaraSimForReplay = true;
	Job.bResume = false;
	TestTrue(TEXT("A Standard job may record a Niagara Sim Cache"), Job.Validate(Error));
	Job.NiagaraSimCacheInputFile = TEXT("Saved/SRDataset/niagara.srncache");
	TestFalse(TEXT("Niagara Sim Cache rejects simultaneous input and output"), Job.Validate(Error));
	Job.NiagaraSimCacheOutputFile.Reset();
	TestTrue(TEXT("A job may load a Niagara Sim Cache"), Job.Validate(Error));
	Job.bValidateNiagaraSimCache = true;
	Job.bEnableSemanticValidationFixture = true;
	Job.bUseDeterministicCameraTransform = true;
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	Job.bCaptureMainViewTemporalDiagnostics = true;
	Job.EndFrame = Job.StartFrame;
	TestFalse(TEXT("Niagara Sim Cache validation requires two frames"), Job.Validate(Error));
	Job.EndFrame = Job.StartFrame + 1;
	TestTrue(TEXT("Niagara Sim Cache validation accepts two consecutive frames"), Job.Validate(Error));
	Job.bControlNiagara = false;
	TestFalse(TEXT("Niagara Sim Cache replay requires Niagara control"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.ChaosRigidBodyCacheOutputFile = TEXT("Saved/SRDataset/chaos.srcache");
	TestFalse(TEXT("Chaos cache paths require native cache replay"), Job.Validate(Error));
	Job.bCacheChaosRigidBodyTransformsForReplay = true;
	Job.bResume = false;
	TestTrue(TEXT("A Standard job may record a Chaos rigid-body cache"), Job.Validate(Error));
	Job.ChaosRigidBodyCacheInputFile = TEXT("Saved/SRDataset/chaos.srcache");
	TestFalse(TEXT("Chaos rigid-body cache rejects simultaneous input and output"), Job.Validate(Error));
	Job.ChaosRigidBodyCacheOutputFile.Reset();
	TestTrue(TEXT("A job may load a Chaos rigid-body cache"), Job.Validate(Error));
	Job.bValidateChaosRigidBodyCache = true;
	Job.bUseDeterministicCameraTransform = true;
	Job.EndFrame = Job.StartFrame + 10;
	TestFalse(TEXT("Chaos rigid-body cache validation requires twelve frames"), Job.Validate(Error));
	Job.EndFrame = Job.StartFrame + 11;
	TestTrue(TEXT("Chaos rigid-body cache validation accepts twelve consecutive frames"), Job.Validate(Error));
	Job.bEnableChaosDeterminism = false;
	TestFalse(TEXT("Chaos rigid-body cache replay requires Chaos determinism"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.bValidateMainViewSceneCapturePixelDomain = true;
	TestFalse(TEXT("Pixel-domain validation cannot be enabled without the paired SceneCapture extraction"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.bCaptureSceneCaptureLRComparison = true;
	TestFalse(TEXT("SceneCapture LR comparison requires real Main View temporal diagnostics"), Job.Validate(Error));
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	Job.bCaptureMainViewTemporalDiagnostics = true;
	TestTrue(TEXT("SceneCapture LR comparison accepts the paired native-render paths"), Job.Validate(Error));
	Job.bValidateMainViewSceneCapturePixelDomain = true;
	TestFalse(TEXT("Pixel-domain validation rejects an uncontrolled scene"), Job.Validate(Error));
	Job.bEnableSemanticValidationFixture = true;
	Job.bUseDeterministicCameraTransform = true;
	Job.SemanticMotionScenario = ESRDatasetSemanticMotionScenario::Static;
	Job.bLockTemporalJitterToLogicalFrame = true;
	TestTrue(TEXT("Pixel-domain validation accepts the locked static semantic fixture"), Job.Validate(Error));

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
	Job.SemanticMotionScenario = ESRDatasetSemanticMotionScenario::Static;
	TestFalse(TEXT("Analytic motion scenarios require a deterministic camera"), Job.Validate(Error));
	Job.bUseDeterministicCameraTransform = true;
	TestTrue(TEXT("Static analytic motion accepts a fixed deterministic camera"), Job.Validate(Error));
	Job.SemanticMotionScenario = ESRDatasetSemanticMotionScenario::CameraOnly;
	TestFalse(TEXT("Camera-only analytic motion requires a non-zero camera trajectory"), Job.Validate(Error));
	Job.DeterministicCameraTranslationPerLogicalFrameCm = FVector(0.0, 20.0, 0.0);
	TestTrue(TEXT("Camera-only analytic motion accepts a deterministic camera trajectory"), Job.Validate(Error));
	Job.SemanticMotionScenario = ESRDatasetSemanticMotionScenario::ObjectOnly;
	TestFalse(TEXT("Object-only analytic motion rejects camera translation"), Job.Validate(Error));
	Job.DeterministicCameraTranslationPerLogicalFrameCm = FVector::ZeroVector;
	TestTrue(TEXT("Object-only analytic motion accepts a fixed camera"), Job.Validate(Error));
	Job.SemanticMotionScenario = ESRDatasetSemanticMotionScenario::Mixed;
	TestFalse(TEXT("Mixed analytic motion requires camera translation"), Job.Validate(Error));
	Job.DeterministicCameraTranslationPerLogicalFrameCm = FVector(0.0, 20.0, 0.0);
	TestTrue(TEXT("Mixed analytic motion accepts combined camera/object motion"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	Job.bCaptureMainViewTemporalDiagnostics = true;
	Job.bEnableSemanticValidationFixture = true;
	Job.bUseDeterministicCameraTransform = true;
	Job.SemanticMotionScenario = ESRDatasetSemanticMotionScenario::Static;
	Job.StartFrame = 0;
	Job.EndFrame = 7;
	Job.bValidateTemporalJitterSignCoverage = true;
	TestFalse(TEXT("Jitter sign coverage requires logical-frame jitter locking"), Job.Validate(Error));
	Job.bLockTemporalJitterToLogicalFrame = true;
	Job.TemporalJitterSequenceLength = 8;
	TestTrue(TEXT("Jitter sign coverage accepts one complete static eight-phase cycle"), Job.Validate(Error));
	Job.EndFrame = 6;
	TestFalse(TEXT("Jitter sign coverage rejects an incomplete logical cycle"), Job.Validate(Error));

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
	Job.bLockTemporalJitterToLogicalFrame = true;
	TestTrue(TEXT("Endpoint replay accepts render suppression and prior captured transforms"), Job.Validate(Error));
	Job.EndFrame = 1;
	TestFalse(TEXT("Endpoint job must end on a captured frame"), Job.Validate(Error));
	Job.EndFrame = 2;
	Job.CaptureFrameOffset = 2;
	TestFalse(TEXT("Capture phase must be smaller than FrameStep"), Job.Validate(Error));

	Job = FSRDatasetCaptureJob();
	Job.ReplayPass = ESRDatasetReplayPass::FrameGenerationReverseEndpoints;
	Job.LRMode = ESRDatasetLRMode::NativeRender;
	Job.bCaptureTemporalDiagnostics = true;
	Job.bCaptureMainViewTemporalDiagnostics = true;
	Job.bCaptureMainViewHUDlessColor = true;
	Job.FrameStep = 2;
	Job.EndFrame = 2;
	Job.bSuppressMainViewOnUncapturedFrames = true;
	Job.bUseLastCapturedEndpointTransforms = true;
	Job.bLockTemporalJitterToLogicalFrame = true;
	TestTrue(TEXT("Reverse endpoint replay accepts the same isolated endpoint contract"), Job.Validate(Error));
	Job.bUseLastCapturedEndpointTransforms = false;
	TestFalse(TEXT("Reverse endpoint replay requires saved future endpoint transforms"), Job.Validate(Error));

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
	Job.bLockTemporalJitterToLogicalFrame = true;
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

	Job = FSRDatasetCaptureJob();
	Job.bLockTemporalJitterToLogicalFrame = true;
	Job.TemporalJitterSequenceLength = 9;
	TestFalse(TEXT("Logical-frame jitter locking rejects an unsafe sequence modulus"), Job.Validate(Error));
	return true;
}

#endif
