#include "SRDatasetCaptureSubsystem.h"

#include "SRDatasetCaptureRig.h"
#include "SRDatasetControllable.h"
#include "SRDatasetValidationFixture.h"
#include "SRDatasetViewExtension.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "ContentStreaming.h"
#include "Dom/JsonObject.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "JsonObjectConverter.h"
#include "Interfaces/IPluginManager.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Rendering/MotionVectorSimulation.h"
#include "TextureResource.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogSRDataset, Log, All);

namespace SRDataset::Private
{
	const TCHAR* TemporalDiagnosticModalities[] = {
		TEXT("color_hr_native_scene_hdr"),
		TEXT("color_lr_scene_hdr"),
		TEXT("velocity_raw"),
		TEXT("velocity_coverage"),
		TEXT("motion_full_current_to_previous"),
		TEXT("motion_valid"),
		TEXT("depth_device_raw"),
		TEXT("depth_view_linear_meters"),
		TEXT("depth_valid"),
		TEXT("depth_previous_reprojected_device"),
		TEXT("history_rejection_mask"),
		TEXT("history_rejection_valid"),
		TEXT("translucency_after_dof_raw"),
		TEXT("transparency_mask"),
		TEXT("reactive_mask"),
		TEXT("object_id")
	};
	constexpr const TCHAR* ReferenceHRModality = TEXT("color_hr_reference_scene_hdr");
	constexpr const TCHAR* HUDlessColorModality = TEXT("color_main_view_hudless_after_tonemap");

	bool IsRunningState(const ESRDatasetCaptureState State)
	{
		return State == ESRDatasetCaptureState::WarmingUp || State == ESRDatasetCaptureState::Capturing;
	}

	template <typename PropertyType, typename ValueType>
	bool ReadReflectedValue(UObject* Object, const FName Name, ValueType& OutValue)
	{
		if (const PropertyType* Property = FindFProperty<PropertyType>(Object->GetClass(), Name))
		{
			OutValue = Property->GetPropertyValue_InContainer(Object);
			return true;
		}
		return false;
	}

	template <typename PropertyType, typename ValueType>
	bool WriteReflectedValue(UObject* Object, const FName Name, const ValueType Value)
	{
		if (PropertyType* Property = FindFProperty<PropertyType>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		return false;
	}
}

void USRDatasetCaptureSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	PreActorTickHandle = FWorldDelegates::OnWorldPreActorTick.AddUObject(this, &ThisClass::HandleWorldPreActorTick);
	PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &ThisClass::HandleWorldPostActorTick);
}

void USRDatasetCaptureSubsystem::Deinitialize()
{
	if (SRDataset::Private::IsRunningState(Status.State))
	{
		FinishCapture(ESRDatasetCaptureState::Cancelled, TEXT("World subsystem deinitialized."));
	}
	FWorldDelegates::OnWorldPreActorTick.Remove(PreActorTickHandle);
	FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
	Super::Deinitialize();
}

bool USRDatasetCaptureSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void USRDatasetCaptureSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	FString JobPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("SRDatasetJob="), JobPath))
	{
		return;
	}

	bCommandLineAutoQuit = FParse::Param(FCommandLine::Get(), TEXT("SRDatasetAutoQuit"));
	FString Error;
	if (!StartCaptureFromJsonFile(JobPath.TrimQuotes(), Error))
	{
		UE_LOG(LogSRDataset, Error, TEXT("Command-line capture could not start: %s"), *Error);
		const bool bCaptureAlreadyHandledFailure = Status.State == ESRDatasetCaptureState::Failed;
		Status.State = ESRDatasetCaptureState::Failed;
		Status.LastError = Error;
		if (bCommandLineAutoQuit && !bCaptureAlreadyHandledFailure)
		{
			FPlatformMisc::RequestExitWithStatus(false, 1, TEXT("SRDataset command-line startup failure"));
		}
	}
}

bool USRDatasetCaptureSubsystem::StartCaptureFromJsonFile(const FString& JobJsonPath, FString& OutError)
{
	const FString ResolvedJobPath = FPaths::IsRelative(JobJsonPath)
		? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), JobJsonPath)
		: FPaths::ConvertRelativePathToFull(JobJsonPath);

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *ResolvedJobPath))
	{
		OutError = FString::Printf(TEXT("Could not read job JSON: %s"), *ResolvedJobPath);
		return false;
	}

	FSRDatasetCaptureJob Job;
	if (!FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Job, 0, 0))
	{
		OutError = FString::Printf(TEXT("Could not parse job JSON: %s"), *ResolvedJobPath);
		return false;
	}
	return StartCapture(Job, OutError);
}

bool USRDatasetCaptureSubsystem::StartCapture(const FSRDatasetCaptureJob& Job, FString& OutError)
{
	if (SRDataset::Private::IsRunningState(Status.State))
	{
		OutError = TEXT("A dataset capture is already running in this world.");
		return false;
	}
	if (!Job.Validate(OutError))
	{
		return false;
	}

	ActiveJob = Job;
	Status = FSRDatasetCaptureStatus();
	Status.CurrentFrame = Job.StartFrame;
	ResolvedOutputDirectory = ResolveOutputDirectory(Job.OutputDirectory);
	Status.OutputDirectory = ResolvedOutputDirectory;
	ManifestFrames.Reset();
	NextRenderSubmissionId = 0;
	bMainViewCapturePending = false;
	PendingMainViewFrameNumber = INDEX_NONE;
	PendingMainViewTimeSeconds = 0.0;
	PendingMainViewHashes.Reset();
	PendingMainViewRenderSubmissions.Reset();
	bStreamingBarrierComplete = false;
	StreamingRequestsAfterBarrier = INDEX_NONE;
	StreamingTextureCountAfterBarrier = 0;
	PendingStreamingTextureCountAfterBarrier = 0;
	StreamingStateAfterBarrierSha1.Reset();

	if (!PrepareJob(OutError))
	{
		FinishCapture(ESRDatasetCaptureState::Failed, OutError);
		return false;
	}

	UE_LOG(LogSRDataset, Display, TEXT("Started dataset job '%s' at %s"), *ActiveJob.JobName, *ResolvedOutputDirectory);
	return true;
}

bool USRDatasetCaptureSubsystem::PrepareJob(FString& OutError)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		OutError = TEXT("Dataset capture requires a Game or PIE world.");
		return false;
	}

	if (!ActiveJob.ExpectedMap.IsEmpty())
	{
		const FString ActualShortName = FPackageName::GetShortName(UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));
		const FString ExpectedShortName = FPackageName::GetShortName(ActiveJob.ExpectedMap);
		if (!ActualShortName.Equals(ExpectedShortName, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Map guard failed. Expected '%s', running '%s'."), *ExpectedShortName, *ActualShortName);
			return false;
		}
	}

	TArray<const TCHAR*> OutputDirectories = { TEXT("hr"), TEXT("lr"), TEXT("depth") };
	if (ActiveJob.bCaptureTemporalDiagnostics)
	{
		for (const TCHAR* Modality : SRDataset::Private::TemporalDiagnosticModalities)
		{
			OutputDirectories.Add(Modality);
		}
	}
	if (ActiveJob.bCaptureReferenceHR)
	{
		OutputDirectories.Add(SRDataset::Private::ReferenceHRModality);
	}
	if (ActiveJob.bCaptureMainViewHUDlessColor)
	{
		OutputDirectories.Add(SRDataset::Private::HUDlessColorModality);
	}
	for (const TCHAR* Directory : OutputDirectories)
	{
		if (FCString::Strcmp(Directory, TEXT("depth")) == 0 && !ActiveJob.bCaptureDepth)
		{
			continue;
		}
		const FString Path = FPaths::Combine(ResolvedOutputDirectory, Directory);
		if (!IFileManager::Get().MakeDirectory(*Path, true) && !IFileManager::Get().DirectoryExists(*Path))
		{
			OutError = FString::Printf(TEXT("Could not create output directory: %s"), *Path);
			return false;
		}
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(World, ASRDatasetCaptureRig::StaticClass(), TEXT("SRDatasetCaptureRig"));
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.ObjectFlags |= RF_Transient;
	CaptureRig = World->SpawnActor<ASRDatasetCaptureRig>(ASRDatasetCaptureRig::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!CaptureRig || !CaptureRig->Configure(ActiveJob, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Could not create the dataset capture rig.");
		}
		return false;
	}

	if (ActiveJob.Sequence.IsValid())
	{
		ULevelSequence* Sequence = Cast<ULevelSequence>(ActiveJob.Sequence.TryLoad());
		if (!Sequence)
		{
			OutError = FString::Printf(TEXT("Could not load Level Sequence: %s"), *ActiveJob.Sequence.ToString());
			return false;
		}
		FMovieSceneSequencePlaybackSettings PlaybackSettings;
		PlaybackSettings.bAutoPlay = false;
		ALevelSequenceActor* SpawnedSequenceActor = nullptr;
		SequencePlayer = ULevelSequencePlayer::CreateLevelSequencePlayer(World, Sequence, PlaybackSettings, SpawnedSequenceActor);
		SequenceActor = SpawnedSequenceActor;
		if (!SequencePlayer)
		{
			OutError = TEXT("Could not create the Level Sequence player.");
			return false;
		}
		SequencePlayer->Pause();
	}

	if (!PrepareSemanticValidationFixture(OutError))
	{
		return false;
	}

	ApplyDeterministicRuntimeState();
	SnapshotProvenance();
	NotifyControllablesPrepare();
	WarmupFramesRemaining = ActiveJob.WarmupFrames;
	Status.State = WarmupFramesRemaining > 0 ? ESRDatasetCaptureState::WarmingUp : ESRDatasetCaptureState::Capturing;
	Status.CurrentFrame = ActiveJob.StartFrame;

	if (!WriteManifest(OutError))
	{
		return false;
	}
	return true;
}

bool USRDatasetCaptureSubsystem::ShouldCaptureFrame(const int32 FrameNumber) const
{
	return FrameNumber >= ActiveJob.StartFrame &&
		(FrameNumber - ActiveJob.StartFrame) % ActiveJob.FrameStep == ActiveJob.CaptureFrameOffset;
}

bool USRDatasetCaptureSubsystem::PrepareSemanticValidationFixture(FString& OutError)
{
	ValidationHiddenActorStates.Reset();
	if (!ActiveJob.bEnableSemanticValidationFixture)
	{
		return true;
	}

	UWorld* World = GetWorld();
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(
		World, ASRDatasetValidationFixture::StaticClass(), TEXT("SRDatasetValidationFixture"));
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.ObjectFlags |= RF_Transient;
	ValidationFixture = World->SpawnActor<ASRDatasetValidationFixture>(
		ASRDatasetValidationFixture::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!ValidationFixture || !ValidationFixture->Configure(OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Could not create the semantic validation fixture.");
		}
		return false;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || Actor == CaptureRig || Actor == ValidationFixture)
		{
			continue;
		}
		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
		Actor->GetComponents(PrimitiveComponents);
		if (PrimitiveComponents.Num() > 0)
		{
			ValidationHiddenActorStates.Add(Actor, Actor->IsHidden());
			Actor->SetActorHiddenInGame(true);
		}
	}
	return true;
}

void USRDatasetCaptureSubsystem::RestoreSemanticValidationFixture()
{
	for (const TPair<TWeakObjectPtr<AActor>, bool>& Pair : ValidationHiddenActorStates)
	{
		if (AActor* Actor = Pair.Key.Get())
		{
			Actor->SetActorHiddenInGame(Pair.Value);
		}
	}
	ValidationHiddenActorStates.Reset();
}

void USRDatasetCaptureSubsystem::ApplyDeterministicRuntimeState()
{
	bPreviousUseFixedTimeStep = FApp::UseFixedTimeStep();
	PreviousFixedDeltaTime = FApp::GetFixedDeltaTime();
	FApp::SetFixedDeltaTime(ActiveJob.GetFixedDeltaSeconds());
	FApp::SetUseFixedTimeStep(true);
	FMath::RandInit(ActiveJob.RandomSeed);
	FMath::SRandInit(ActiveJob.RandomSeed);

	if (ActiveJob.bCaptureMainViewTemporalDiagnostics)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
		{
			PreviousScreenPercentage = Variable->GetFloat();
			const float RequestedPercentage = 100.0f * static_cast<float>(ActiveJob.LRResolution.X) /
				static_cast<float>(ActiveJob.HRResolution.X);
			Variable->Set(RequestedPercentage, ECVF_SetByCode);
			bOverrodeScreenPercentage = true;
		}
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicRes.OperationMode")))
		{
			PreviousDynamicResolutionOperationMode = Variable->GetInt();
			Variable->Set(0, ECVF_SetByCode);
			bOverrodeDynamicResolution = true;
		}
	}

	if (ActiveJob.bEnableChaosDeterminism)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.Deterministic")))
		{
			PreviousChaosDeterminism = Variable->GetInt();
			Variable->Set(1, ECVF_SetByCode);
		}
	}
	if (ActiveJob.bLockExposure)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptationQuality")))
		{
			PreviousEyeAdaptationQuality = Variable->GetInt();
			Variable->Set(0, ECVF_SetByCode);
			bOverrodeEyeAdaptation = true;
		}
	}
	if (ActiveJob.bEnableSemanticValidationFixture)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth")))
		{
			PreviousCustomDepthMode = Variable->GetInt();
			Variable->Set(3, ECVF_SetByCode);
			bOverrodeCustomDepth = true;
		}
	}
	if (ActiveJob.bUseLastCapturedEndpointTransforms)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MotionVectorSimulation")))
		{
			PreviousMotionVectorSimulation = Variable->GetInt();
			Variable->Set(1, ECVF_SetByCode);
			bOverrodeMotionVectorSimulation = true;
		}
	}
	if (ActiveJob.bSuppressMainViewOnUncapturedFrames)
	{
		bPreviousWorldRenderingEnabled = UGameplayStatics::GetEnableWorldRendering(this);
		bOverrodeWorldRendering = true;
		UGameplayStatics::SetEnableWorldRendering(this, true);
	}
	if (ActiveJob.bForceSynchronousRendering)
	{
		const TPair<const TCHAR*, const TCHAR*> Overrides[] = {
			{ TEXT("r.RDG.ParallelExecute"), TEXT("0") },
			{ TEXT("r.RDG.AsyncCompute"), TEXT("0") },
			{ TEXT("r.OneFrameThreadLag"), TEXT("0") },
			{ TEXT("r.Lumen.AsyncCompute"), TEXT("0") },
			{ TEXT("r.Lumen.DiffuseIndirect.AsyncCompute"), TEXT("0") },
			{ TEXT("r.Lumen.Reflections.AsyncCompute"), TEXT("0") },
			{ TEXT("r.LumenScene.Lighting.AsyncCompute"), TEXT("0") },
			{ TEXT("r.LumenScene.ParallelUpdate"), TEXT("0") },
			{ TEXT("r.LumenScene.SurfaceCache.Nanite.AsyncRasterization"), TEXT("0") },
			{ TEXT("r.GPUScene.ParallelUpdate"), TEXT("0") },
			{ TEXT("r.SceneCulling.Async.Update"), TEXT("0") },
			{ TEXT("r.SceneCulling.Async.Query"), TEXT("0") },
			{ TEXT("r.Nanite.AsyncRasterization"), TEXT("0") },
			{ TEXT("r.Nanite.AsyncRasterization.ShadowDepths"), TEXT("0") },
			{ TEXT("r.Nanite.AsyncRasterization.CustomPass"), TEXT("0") },
			{ TEXT("r.Nanite.AsyncRasterization.LumenMeshCards"), TEXT("0") }
		};
		PreviousRenderDeterminismCVars.Reset();
		for (const TPair<const TCHAR*, const TCHAR*>& Override : Overrides)
		{
			if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Override.Key))
			{
				PreviousRenderDeterminismCVars.Add(Override.Key, Variable->GetString());
				Variable->Set(Override.Value, ECVF_SetByCode);
			}
		}
	}
}

void USRDatasetCaptureSubsystem::RestoreDeterministicRuntimeState()
{
	FApp::SetFixedDeltaTime(PreviousFixedDeltaTime);
	FApp::SetUseFixedTimeStep(bPreviousUseFixedTimeStep);
	if (PreviousChaosDeterminism >= 0)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.Deterministic")))
		{
			Variable->Set(PreviousChaosDeterminism, ECVF_SetByCode);
		}
	}
	PreviousChaosDeterminism = -1;
	if (bOverrodeEyeAdaptation)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptationQuality")))
		{
			Variable->Set(PreviousEyeAdaptationQuality, ECVF_SetByCode);
		}
	}
	PreviousEyeAdaptationQuality = -1;
	bOverrodeEyeAdaptation = false;
	if (bOverrodeCustomDepth)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CustomDepth")))
		{
			Variable->Set(PreviousCustomDepthMode, ECVF_SetByCode);
		}
	}
	PreviousCustomDepthMode = -1;
	bOverrodeCustomDepth = false;
	if (bOverrodeMotionVectorSimulation)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MotionVectorSimulation")))
		{
			Variable->Set(PreviousMotionVectorSimulation, ECVF_SetByCode);
		}
	}
	PreviousMotionVectorSimulation = -1;
	bOverrodeMotionVectorSimulation = false;
	if (bOverrodeWorldRendering)
	{
		UGameplayStatics::SetEnableWorldRendering(this, bPreviousWorldRenderingEnabled);
	}
	bOverrodeWorldRendering = false;
	LastCapturedEndpointTransforms.Reset();
	for (const TPair<FString, FString>& Pair : PreviousRenderDeterminismCVars)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Pair.Key))
		{
			Variable->Set(*Pair.Value, ECVF_SetByCode);
		}
	}
	PreviousRenderDeterminismCVars.Reset();
	if (bOverrodeScreenPercentage)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
		{
			Variable->Set(PreviousScreenPercentage, ECVF_SetByCode);
		}
	}
	if (bOverrodeDynamicResolution)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicRes.OperationMode")))
		{
			Variable->Set(PreviousDynamicResolutionOperationMode, ECVF_SetByCode);
		}
	}
	bOverrodeScreenPercentage = false;
	bOverrodeDynamicResolution = false;
}

void USRDatasetCaptureSubsystem::SnapshotProvenance()
{
	TSharedRef<FJsonObject> JobObject = MakeShared<FJsonObject>();
	FJsonObjectConverter::UStructToJsonObject(FSRDatasetCaptureJob::StaticStruct(), &ActiveJob, JobObject, 0, 0);
	FString NormalizedJobJson;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> JobWriter =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&NormalizedJobJson);
	FJsonSerializer::Serialize(JobObject, JobWriter);
	CaptureConfigSha1 = HashString(NormalizedJobJson);

	const TCHAR* CVarNames[] = {
		TEXT("p.Chaos.Solver.Deterministic"),
		TEXT("r.AntiAliasingMethod"),
		TEXT("r.CustomDepth"),
		TEXT("r.DynamicRes.OperationMode"),
		TEXT("r.EyeAdaptationQuality"),
		TEXT("r.GPUScene.ParallelUpdate"),
		TEXT("r.HDR.Display.ColorGamut"),
		TEXT("r.HDR.Display.OutputDevice"),
		TEXT("r.HDR.EnableHDROutput"),
		TEXT("r.Lumen.AsyncCompute"),
		TEXT("r.Lumen.DiffuseIndirect.AsyncCompute"),
		TEXT("r.Lumen.Reflections.AsyncCompute"),
		TEXT("r.LumenScene.Lighting.AsyncCompute"),
		TEXT("r.LumenScene.ParallelUpdate"),
		TEXT("r.LumenScene.SurfaceCache.Nanite.AsyncRasterization"),
		TEXT("r.MaterialQualityLevel"),
		TEXT("r.MipMapLODBias"),
		TEXT("r.MotionVectorSimulation"),
		TEXT("r.Nanite.AsyncRasterization"),
		TEXT("r.Nanite.AsyncRasterization.CustomPass"),
		TEXT("r.Nanite.AsyncRasterization.LumenMeshCards"),
		TEXT("r.Nanite.AsyncRasterization.ShadowDepths"),
		TEXT("r.OneFrameThreadLag"),
		TEXT("r.RDG.AsyncCompute"),
		TEXT("r.RDG.ParallelExecute"),
		TEXT("r.SceneCulling.Async.Query"),
		TEXT("r.SceneCulling.Async.Update"),
		TEXT("r.ScreenPercentage"),
		TEXT("r.StaticMeshLODDistanceScale"),
		TEXT("r.SkeletalMeshLODBias"),
		TEXT("r.ParticleLODBias"),
		TEXT("foliage.LODDistanceScale"),
		TEXT("r.Nanite.ViewMeshLODBias.Enable"),
		TEXT("r.Nanite.ViewMeshLODBias.Offset"),
		TEXT("r.Nanite.ViewMeshLODBias.Min"),
		TEXT("r.Streaming.Boost"),
		TEXT("r.Streaming.FullyLoadUsedTextures"),
		TEXT("r.Streaming.LimitPoolSizeToVRAM"),
		TEXT("r.Streaming.MipBias"),
		TEXT("r.Streaming.PoolSize"),
		TEXT("r.Streaming.UseAllMips"),
		TEXT("r.Streaming.UseFixedPoolSize"),
		TEXT("r.TemporalAA.Upsampling"),
		TEXT("r.ViewTextureMipBias.Min"),
		TEXT("r.ViewTextureMipBias.Offset"),
		TEXT("r.ViewTextureMipBias.Quantization"),
		TEXT("r.ViewDistanceScale"),
		TEXT("sg.AntiAliasingQuality"),
		TEXT("sg.EffectsQuality"),
		TEXT("sg.GlobalIlluminationQuality"),
		TEXT("sg.PostProcessQuality"),
		TEXT("sg.ReflectionQuality"),
		TEXT("sg.ShadingQuality"),
		TEXT("sg.ShadowQuality"),
		TEXT("sg.TextureQuality"),
		TEXT("sg.ViewDistanceQuality")
	};
	CaptureCVarProfile.Reset();
	TArray<FString> ProfileLines;
	for (const TCHAR* Name : CVarNames)
	{
		if (const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			const FString Value = Variable->GetString();
			CaptureCVarProfile.Add(Name, Value);
			ProfileLines.Add(FString(Name) + TEXT("=") + Value);
		}
	}
	ProfileLines.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
	});
	CaptureCVarProfileCanonical = FString::Join(ProfileLines, TEXT("\n"));
	CaptureCVarProfileSha1 = HashString(CaptureCVarProfileCanonical);

	const FString MapPackage = !ActiveJob.ExpectedMap.IsEmpty()
		? ActiveJob.ExpectedMap
		: (GetWorld() ? UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()) : FString());
	FString MapFilename;
	if (!MapPackage.IsEmpty() && FPackageName::DoesPackageExist(MapPackage, &MapFilename))
	{
		ContentMapSha1 = HashFile(MapFilename);
	}
	else
	{
		ContentMapSha1.Reset();
	}

	ShaderSourceSha1.Reset();
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperResolutionDataset")))
	{
		ShaderSourceSha1 = HashFile(FPaths::Combine(
			Plugin->GetBaseDir(), TEXT("Shaders/Private/SRDatasetExtract.usf")));
	}
}

bool USRDatasetCaptureSubsystem::EnsureStreamingReady(FString& OutError)
{
	if (bStreamingBarrierComplete)
	{
		return true;
	}
	if (IStreamingManager::HasShutdown())
	{
		OutError = TEXT("The engine streaming manager is unavailable before dataset capture.");
		return false;
	}

	FStreamingManagerCollection& StreamingManager = IStreamingManager::Get();
	StreamingRequestsAfterBarrier = ActiveJob.bBlockOnStreamingBeforeCapture
		? StreamingManager.StreamAllResources(ActiveJob.StreamingWaitSeconds)
		: StreamingManager.GetNumWantingResources();
	if (ActiveJob.bBlockOnStreamingBeforeCapture && StreamingRequestsAfterBarrier > 0)
	{
		OutError = FString::Printf(
			TEXT("Streaming barrier timed out after %.3f seconds with %d request(s) still in flight."),
			ActiveJob.StreamingWaitSeconds,
			StreamingRequestsAfterBarrier);
		return false;
	}

	StreamingStateAfterBarrierSha1 = ComputeStreamingStateSha1(
		StreamingTextureCountAfterBarrier,
		PendingStreamingTextureCountAfterBarrier);
	bStreamingBarrierComplete = true;
	UE_LOG(
		LogSRDataset,
		Display,
		TEXT("Streaming barrier ready: requests=%d textures=%d pendingTextures=%d stateSha1=%s"),
		StreamingRequestsAfterBarrier,
		StreamingTextureCountAfterBarrier,
		PendingStreamingTextureCountAfterBarrier,
		*StreamingStateAfterBarrierSha1);
	return true;
}

FString USRDatasetCaptureSubsystem::ComputeStreamingStateSha1(
	int32& OutTextureCount,
	int32& OutPendingTextureCount) const
{
	TArray<FString> TextureStateLines;
	OutTextureCount = 0;
	OutPendingTextureCount = 0;
	for (TObjectIterator<UTexture2D> It; It; ++It)
	{
		const UTexture2D* Texture = *It;
		if (!IsValid(Texture) || Texture->HasAnyFlags(RF_ClassDefaultObject))
		{
			continue;
		}
		const FTextureResource* Resource = Texture->GetResource();
		const bool bPending = Texture->HasPendingInitOrStreaming();
		++OutTextureCount;
		OutPendingTextureCount += bPending ? 1 : 0;
		TextureStateLines.Add(FString::Printf(
			TEXT("%s|%dx%d|assetMips=%d|residentMips=%d|streamable=%d|pending=%d"),
			*Texture->GetPathName(),
			Texture->GetSizeX(),
			Texture->GetSizeY(),
			Texture->GetNumMips(),
			Resource ? Resource->GetCurrentMipCount() : 0,
			Texture->IsStreamable() ? 1 : 0,
			bPending ? 1 : 0));
	}
	TextureStateLines.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
	});
	return HashString(FString::Join(TextureStateLines, TEXT("\n")));
}

USRDatasetCaptureSubsystem::FSceneStateSummary USRDatasetCaptureSubsystem::ComputeSceneStateSummary() const
{
	FSceneStateSummary Summary;
	if (!GetWorld())
	{
		return Summary;
	}

	const auto TransformString = [](const FTransform& Transform)
	{
		const FMatrix Matrix = Transform.ToMatrixWithScale();
		FString Value;
		for (int32 Row = 0; Row < 4; ++Row)
		{
			for (int32 Column = 0; Column < 4; ++Column)
			{
				if (!Value.IsEmpty())
				{
					Value += TEXT(",");
				}
				Value += FString::Printf(TEXT("%.17g"), Matrix.M[Row][Column]);
			}
		}
		return Value;
	};

	TArray<FString> StateLines;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

		++Summary.ActorCount;
		const bool bControllable = Actor->GetClass()->ImplementsInterface(
			USRDatasetControllable::StaticClass());
		Summary.ControllableActorCount += bControllable ? 1 : 0;
		Summary.UncontrolledTickingActorCount +=
			Actor->IsActorTickEnabled() && !bControllable ? 1 : 0;
		if (bControllable)
		{
			Summary.ControllableActors.Add(FString::Printf(
				TEXT("%s|%s"), *Actor->GetPathName(), *Actor->GetClass()->GetPathName()));
		}
		if (Actor->IsActorTickEnabled() && !bControllable)
		{
			Summary.UncontrolledTickingActors.Add(FString::Printf(
				TEXT("%s|%s"), *Actor->GetPathName(), *Actor->GetClass()->GetPathName()));
		}
		StateLines.Add(FString::Printf(
			TEXT("actor|%s|class=%s|transform=%s|hidden=%d|tick=%d|controllable=%d"),
			*Actor->GetPathName(),
			*Actor->GetClass()->GetPathName(),
			*TransformString(Actor->GetActorTransform()),
			Actor->IsHidden() ? 1 : 0,
			Actor->IsActorTickEnabled() ? 1 : 0,
			bControllable ? 1 : 0));

		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!IsValid(Component))
			{
				continue;
			}
			++Summary.ComponentCount;
			const USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
			StateLines.Add(FString::Printf(
				TEXT("component|%s|class=%s|active=%d|registered=%d|tick=%d|visible=%d|transform=%s"),
				*Component->GetPathName(),
				*Component->GetClass()->GetPathName(),
				Component->IsActive() ? 1 : 0,
				Component->IsRegistered() ? 1 : 0,
				Component->PrimaryComponentTick.IsTickFunctionEnabled() ? 1 : 0,
				SceneComponent && SceneComponent->IsVisible() ? 1 : 0,
				SceneComponent ? *TransformString(SceneComponent->GetComponentTransform()) : TEXT("none")));

			if (const USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
			{
				++Summary.SkeletalComponentCount;
				const TArray<FTransform>& BoneTransforms = Skinned->GetComponentSpaceTransforms();
				Summary.BoneCount += BoneTransforms.Num();
				StateLines.Add(FString::Printf(
					TEXT("skinned|%s|asset=%s|bones=%d|revision=%u"),
					*Skinned->GetPathName(),
					Skinned->GetSkinnedAsset() ? *Skinned->GetSkinnedAsset()->GetPathName() : TEXT("none"),
					BoneTransforms.Num(),
					Skinned->GetBoneTransformRevisionNumber()));
				for (int32 BoneIndex = 0; BoneIndex < BoneTransforms.Num(); ++BoneIndex)
				{
					StateLines.Add(FString::Printf(
						TEXT("bone|%s|index=%d|name=%s|componentSpace=%s"),
						*Skinned->GetPathName(),
						BoneIndex,
						*Skinned->GetBoneName(BoneIndex).ToString(),
						*TransformString(BoneTransforms[BoneIndex])));
				}
			}

			if (const UNiagaraComponent* Niagara = Cast<UNiagaraComponent>(Component))
			{
				++Summary.FXComponentCount;
				++Summary.NiagaraComponentCount;
				StateLines.Add(FString::Printf(
					TEXT("niagara|%s|asset=%s|active=%d|ageMode=%d|desiredAge=%.9g|seedOffset=%d|forceSolo=%d|seekDelta=%.9g|maxSimTime=%.9g"),
					*Niagara->GetPathName(),
					Niagara->GetAsset() ? *Niagara->GetAsset()->GetPathName() : TEXT("none"),
					Niagara->IsActive() ? 1 : 0,
					static_cast<int32>(Niagara->GetAgeUpdateMode()),
					Niagara->GetDesiredAge(),
					Niagara->GetRandomSeedOffset(),
					Niagara->GetForceSolo() ? 1 : 0,
					Niagara->GetSeekDelta(),
					Niagara->GetMaxSimTime()));
			}
			else if (const UParticleSystemComponent* Cascade = Cast<UParticleSystemComponent>(Component))
			{
				++Summary.FXComponentCount;
				StateLines.Add(FString::Printf(
					TEXT("cascade|%s|asset=%s|active=%d"),
					*Cascade->GetPathName(),
					Cascade->GetFXSystemAsset() ? *Cascade->GetFXSystemAsset()->GetPathName() : TEXT("none"),
					Cascade->IsActive() ? 1 : 0));
			}
		}
	}

	StateLines.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
	});
	Summary.ControllableActors.Sort();
	Summary.UncontrolledTickingActors.Sort();
	Summary.Sha1 = HashString(FString::Join(StateLines, TEXT("\n")));
	return Summary;
}

void USRDatasetCaptureSubsystem::HandleWorldPreActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World != GetWorld() || !SRDataset::Private::IsRunningState(Status.State))
	{
		return;
	}

	if (bMainViewCapturePending)
	{
		FString Error;
		if (!FinalizePendingMainViewCapture(Error))
		{
			FinishCapture(ESRDatasetCaptureState::Failed, Error);
			return;
		}
		if (Status.CurrentFrame > ActiveJob.EndFrame)
		{
			FinishCapture(ESRDatasetCaptureState::Completed);
			return;
		}
	}
	if (Status.State == ESRDatasetCaptureState::Capturing &&
		ActiveJob.bSuppressMainViewOnUncapturedFrames)
	{
		UGameplayStatics::SetEnableWorldRendering(this, ShouldCaptureFrame(Status.CurrentFrame));
	}

	const int32 EvaluationFrame = Status.State == ESRDatasetCaptureState::WarmingUp
		? ActiveJob.StartFrame
		: Status.CurrentFrame;
	FString Error;
	if (!EvaluateSequence(EvaluationFrame, Error))
	{
		FinishCapture(ESRDatasetCaptureState::Failed, Error);
		return;
	}

	const float TimeSeconds = static_cast<float>(EvaluationFrame * ActiveJob.GetFixedDeltaSeconds());
	DiscoverAndControlNiagara(TimeSeconds);
	NotifyControllablesEvaluate(EvaluationFrame, TimeSeconds);
}

void USRDatasetCaptureSubsystem::HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World != GetWorld() || !SRDataset::Private::IsRunningState(Status.State))
	{
		return;
	}

	if (Status.State == ESRDatasetCaptureState::WarmingUp)
	{
		FString Error;
		if (!UpdateCaptureCamera(Error))
		{
			FinishCapture(ESRDatasetCaptureState::Failed, Error);
			return;
		}
		CaptureRig->WarmupRenderState(ActiveJob);
		--WarmupFramesRemaining;
		if (WarmupFramesRemaining <= 0)
		{
			Status.State = ESRDatasetCaptureState::Capturing;
			Status.CurrentFrame = ActiveJob.StartFrame;
		}
		return;
	}

	if (!bStreamingBarrierComplete)
	{
		FString Error;
		if (!EnsureStreamingReady(Error))
		{
			FinishCapture(ESRDatasetCaptureState::Failed, Error);
			return;
		}
	}

	if (ShouldCaptureFrame(Status.CurrentFrame))
	{
		FString Error;
		if (!CaptureCurrentFrame(Error))
		{
			FinishCapture(ESRDatasetCaptureState::Failed, Error);
			return;
		}
	}

	++Status.CurrentFrame;
	if (Status.CurrentFrame > ActiveJob.EndFrame && !bMainViewCapturePending)
	{
		FinishCapture(ESRDatasetCaptureState::Completed);
	}
}

bool USRDatasetCaptureSubsystem::EvaluateSequence(const int32 FrameNumber, FString& OutError)
{
	if (!SequencePlayer)
	{
		return true;
	}

	const float TimeSeconds = static_cast<float>(FrameNumber * ActiveJob.GetFixedDeltaSeconds());
	FMovieSceneSequencePlaybackParams Params(TimeSeconds, EUpdatePositionMethod::Play);
	Params.bHasJumped = false;
	SequencePlayer->SetPlaybackPosition(Params);
	return true;
}

void USRDatasetCaptureSubsystem::DiscoverAndControlNiagara(const float TimeSeconds)
{
	if (!ActiveJob.bControlNiagara)
	{
		return;
	}

	for (TObjectIterator<UNiagaraComponent> It; It; ++It)
	{
		UNiagaraComponent* Component = *It;
		if (!IsValid(Component) || Component->GetWorld() != GetWorld() || Component->HasAnyFlags(RF_ClassDefaultObject))
		{
			continue;
		}

		if (!NiagaraComponentStates.Contains(Component))
		{
			FNiagaraComponentState State;
			State.AgeUpdateMode = static_cast<uint8>(Component->GetAgeUpdateMode());
			State.RandomSeedOffset = Component->GetRandomSeedOffset();
			State.SeekDelta = Component->GetSeekDelta();
			State.MaxSimTime = Component->GetMaxSimTime();
			State.bForceSolo = Component->GetForceSolo();
			State.bLockSeekDelta = Component->GetLockDesiredAgeDeltaTimeToSeekDelta();
			NiagaraComponentStates.Add(Component, State);

			if (UNiagaraSystem* System = Component->GetAsset())
			{
				if (!NiagaraSystemStates.Contains(System))
				{
					FNiagaraSystemState SystemState;
					SRDataset::Private::ReadReflectedValue<FBoolProperty>(System, TEXT("bDeterminism"), SystemState.bDeterminism);
					SRDataset::Private::ReadReflectedValue<FBoolProperty>(System, TEXT("bFixedTickDelta"), SystemState.bFixedTickDelta);
					SRDataset::Private::ReadReflectedValue<FIntProperty>(System, TEXT("RandomSeed"), SystemState.RandomSeed);
					SRDataset::Private::ReadReflectedValue<FFloatProperty>(System, TEXT("FixedTickDeltaTime"), SystemState.FixedTickDeltaTime);
					NiagaraSystemStates.Add(System, SystemState);
				}
				if (ActiveJob.bForceNiagaraDeterminism)
				{
					SRDataset::Private::WriteReflectedValue<FBoolProperty>(System, TEXT("bDeterminism"), true);
					SRDataset::Private::WriteReflectedValue<FBoolProperty>(System, TEXT("bFixedTickDelta"), true);
					SRDataset::Private::WriteReflectedValue<FIntProperty>(System, TEXT("RandomSeed"), ActiveJob.RandomSeed);
					SRDataset::Private::WriteReflectedValue<FFloatProperty>(System, TEXT("FixedTickDeltaTime"), static_cast<float>(ActiveJob.GetFixedDeltaSeconds()));
				}
			}

			Component->SetForceSolo(true);
			Component->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
			Component->SetRandomSeedOffset(ActiveJob.RandomSeed ^ static_cast<int32>(GetTypeHash(Component->GetPathName())));
			Component->SetSeekDelta(static_cast<float>(ActiveJob.GetFixedDeltaSeconds()));
			Component->SetLockDesiredAgeDeltaTimeToSeekDelta(true);
			Component->SetMaxSimTime(60.0f);
			Component->ResetSystem();
			Component->SeekToDesiredAge(TimeSeconds);
		}
		else
		{
			Component->SetDesiredAge(TimeSeconds);
		}
	}
}

void USRDatasetCaptureSubsystem::RestoreNiagara()
{
	// Restore shared system assets before resetting their component instances.
	for (const TPair<TWeakObjectPtr<UNiagaraSystem>, FNiagaraSystemState>& Pair : NiagaraSystemStates)
	{
		if (UNiagaraSystem* System = Pair.Key.Get())
		{
			const FNiagaraSystemState& State = Pair.Value;
			SRDataset::Private::WriteReflectedValue<FBoolProperty>(System, TEXT("bDeterminism"), State.bDeterminism);
			SRDataset::Private::WriteReflectedValue<FBoolProperty>(System, TEXT("bFixedTickDelta"), State.bFixedTickDelta);
			SRDataset::Private::WriteReflectedValue<FIntProperty>(System, TEXT("RandomSeed"), State.RandomSeed);
			SRDataset::Private::WriteReflectedValue<FFloatProperty>(System, TEXT("FixedTickDeltaTime"), State.FixedTickDeltaTime);
		}
	}

	for (const TPair<TWeakObjectPtr<UNiagaraComponent>, FNiagaraComponentState>& Pair : NiagaraComponentStates)
	{
		if (UNiagaraComponent* Component = Pair.Key.Get())
		{
			const FNiagaraComponentState& State = Pair.Value;
			Component->SetAgeUpdateMode(static_cast<ENiagaraAgeUpdateMode>(State.AgeUpdateMode));
			Component->SetRandomSeedOffset(State.RandomSeedOffset);
			Component->SetSeekDelta(State.SeekDelta);
			Component->SetMaxSimTime(State.MaxSimTime);
			Component->SetLockDesiredAgeDeltaTimeToSeekDelta(State.bLockSeekDelta);
			Component->SetForceSolo(State.bForceSolo);
			Component->ResetSystem();
		}
	}

	NiagaraComponentStates.Reset();
	NiagaraSystemStates.Reset();
}

void USRDatasetCaptureSubsystem::NotifyControllablesPrepare()
{
	PreparedControllables.Reset();
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->GetClass()->ImplementsInterface(USRDatasetControllable::StaticClass()))
		{
			ISRDatasetControllable::Execute_DatasetPrepare(*It, ActiveJob.RandomSeed, static_cast<float>(ActiveJob.GetFixedDeltaSeconds()));
			PreparedControllables.Add(*It);
		}
	}
}

void USRDatasetCaptureSubsystem::NotifyControllablesEvaluate(const int32 FrameNumber, const float TimeSeconds)
{
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->GetClass()->ImplementsInterface(USRDatasetControllable::StaticClass()))
		{
			if (!PreparedControllables.Contains(*It))
			{
				ISRDatasetControllable::Execute_DatasetPrepare(*It, ActiveJob.RandomSeed, static_cast<float>(ActiveJob.GetFixedDeltaSeconds()));
				PreparedControllables.Add(*It);
			}
			ISRDatasetControllable::Execute_DatasetEvaluateFrame(*It, FrameNumber, TimeSeconds);
		}
	}
}

void USRDatasetCaptureSubsystem::NotifyControllablesRestore()
{
	if (!GetWorld())
	{
		return;
	}
	for (const TWeakObjectPtr<AActor>& ActorPtr : PreparedControllables)
	{
		if (AActor* Actor = ActorPtr.Get())
		{
			ISRDatasetControllable::Execute_DatasetRestore(Actor);
		}
	}
	PreparedControllables.Reset();
}

bool USRDatasetCaptureSubsystem::UpdateCaptureCamera(FString& OutError)
{
	FMinimalViewInfo View;
	bool bFoundCamera = false;
	if (SequencePlayer)
	{
		if (UCameraComponent* Camera = SequencePlayer->GetActiveCameraComponent())
		{
			Camera->GetCameraView(0.0f, View);
			bFoundCamera = true;
		}
	}

	if (!bFoundCamera && !ActiveJob.CameraActorTag.IsNone())
	{
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			if (It->ActorHasTag(ActiveJob.CameraActorTag))
			{
				if (UCameraComponent* Camera = It->FindComponentByClass<UCameraComponent>())
				{
					Camera->GetCameraView(0.0f, View);
					bFoundCamera = true;
					break;
				}
			}
		}
	}

	if (!bFoundCamera)
	{
		if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
		{
			if (APlayerCameraManager* CameraManager = Controller->PlayerCameraManager)
			{
				View = CameraManager->GetCameraCacheView();
				bFoundCamera = true;
			}
		}
	}

	if (!bFoundCamera)
	{
		OutError = TEXT("No active Sequencer, tagged, or player camera was found.");
		return false;
	}

	CaptureRig->ApplyCameraView(View, ActiveJob.bDisableMotionBlur, ActiveJob.bLockExposure);
	if (ValidationFixture)
	{
		ValidationFixture->Evaluate(
			View,
			Status.CurrentFrame,
			ActiveJob.StartFrame,
			ActiveJob.HRResolution,
			ActiveJob.bUseLastCapturedEndpointTransforms);
	}
	return true;
}

bool USRDatasetCaptureSubsystem::CaptureCurrentFrame(FString& OutError)
{
	if (!UpdateCaptureCamera(OutError))
	{
		return false;
	}

	const int32 FrameNumber = Status.CurrentFrame;
	const int32 FirstCapturedFrame = ActiveJob.StartFrame + ActiveJob.CaptureFrameOffset;
	const bool bHistoryReset = FrameNumber == FirstCapturedFrame;
	const int32 MotionPreviousLogicalFrameId =
		ActiveJob.ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate
			? FrameNumber - 1
			: (bHistoryReset ? FrameNumber : FrameNumber - ActiveJob.FrameStep);
	ApplyLastCapturedEndpointTransforms();
	const double TimeSeconds = FrameNumber * ActiveJob.GetFixedDeltaSeconds();
	TMap<FString, FString> Hashes;
	TMap<FString, int64> RenderSubmissions;
	if (ActiveJob.bResume && IsFrameAlreadyComplete(FrameNumber))
	{
		Hashes.Add(TEXT("hr"), HashFile(MakeFramePath(TEXT("hr"), FrameNumber, TEXT("png"))));
		Hashes.Add(TEXT("lr"), HashFile(MakeFramePath(TEXT("lr"), FrameNumber, TEXT("png"))));
		if (ActiveJob.bCaptureDepth)
		{
			Hashes.Add(TEXT("depth"), HashFile(MakeFramePath(TEXT("depth"), FrameNumber, TEXT("exr"))));
		}
		if (ActiveJob.bCaptureTemporalDiagnostics)
		{
			for (const TCHAR* Modality : SRDataset::Private::TemporalDiagnosticModalities)
			{
				Hashes.Add(Modality, HashFile(MakeFramePath(Modality, FrameNumber, TEXT("exr"))));
			}
		}
		if (ActiveJob.bCaptureReferenceHR)
		{
			Hashes.Add(
				SRDataset::Private::ReferenceHRModality,
				HashFile(MakeFramePath(SRDataset::Private::ReferenceHRModality, FrameNumber, TEXT("exr"))));
		}
		if (ActiveJob.bCaptureMainViewHUDlessColor)
		{
			Hashes.Add(
				SRDataset::Private::HUDlessColorModality,
				HashFile(MakeFramePath(SRDataset::Private::HUDlessColorModality, FrameNumber, TEXT("exr"))));
		}
		++Status.SkippedSamples;
		AppendFrameManifest(FrameNumber, TimeSeconds, Hashes, RenderSubmissions, true);
	}
	else
	{
		const auto RecordHighResolutionSubmissions = [&]()
		{
			RenderSubmissions.Add(TEXT("hr"), NextRenderSubmissionId++);
			if (ActiveJob.bCaptureReferenceHR)
			{
				RenderSubmissions.Add(TEXT("hr_reference"), NextRenderSubmissionId++);
			}
		};
		const auto RecordLowResolutionSubmissions = [&]()
		{
			if (ActiveJob.LRMode == ESRDatasetLRMode::NativeRender)
			{
				RenderSubmissions.Add(TEXT("lr"), NextRenderSubmissionId++);
			}
			if (ActiveJob.bCaptureDepth)
			{
				RenderSubmissions.Add(TEXT("depth"), NextRenderSubmissionId++);
			}
		};
		if (ActiveJob.AuxiliaryCaptureOrder == ESRDatasetAuxiliaryCaptureOrder::LowResolutionFirst)
		{
			RecordLowResolutionSubmissions();
			RecordHighResolutionSubmissions();
		}
		else
		{
			RecordHighResolutionSubmissions();
			RecordLowResolutionSubmissions();
		}
		if (!CaptureRig->CaptureFrame(
			ActiveJob,
			FrameNumber,
			MotionPreviousLogicalFrameId,
			bHistoryReset,
			MakeFramePath(TEXT("hr"), FrameNumber, TEXT("png")),
			MakeFramePath(TEXT("lr"), FrameNumber, TEXT("png")),
			MakeFramePath(TEXT("depth"), FrameNumber, TEXT("exr")),
			Hashes,
			OutError))
		{
			return false;
		}

		if (ActiveJob.bCaptureMainViewTemporalDiagnostics)
		{
			TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> ViewExtension = GetSRDatasetViewExtension();
			if (!ViewExtension)
			{
				OutError = TEXT("The SRDataset RDG view extension is not available for Main View capture.");
				return false;
			}
			if (!ViewExtension->RequestCapture(ActiveJob.LRResolution, ActiveJob.HRResolution, true, OutError))
			{
				return false;
			}
			if (ActiveJob.bCaptureMainViewHUDlessColor)
			{
				const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> TonemapExtension =
					GetSRDatasetTonemapViewExtension();
				if (!TonemapExtension ||
					!TonemapExtension->RequestTonemappedColorCapture(ActiveJob.HRResolution, true, OutError))
				{
					if (OutError.IsEmpty())
					{
						OutError = TEXT("The SRDataset tonemap view extension is not available.");
					}
					return false;
				}
			}
			RenderSubmissions.Add(TEXT("main_view_temporal"), NextRenderSubmissionId++);
			bMainViewCapturePending = true;
			PendingMainViewFrameNumber = FrameNumber;
			PendingMainViewTimeSeconds = TimeSeconds;
			PendingMainViewHashes = MoveTemp(Hashes);
			PendingMainViewRenderSubmissions = MoveTemp(RenderSubmissions);
		}
		else
		{
			++Status.CapturedSamples;
			AppendFrameManifest(FrameNumber, TimeSeconds, Hashes, RenderSubmissions, false);
		}
	}

	if (!bMainViewCapturePending && ActiveJob.bWriteManifestEveryFrame && !WriteManifest(OutError))
	{
		return false;
	}
	SnapshotCapturedEndpointTransforms();
	if (ValidationFixture)
	{
		ValidationFixture->CommitCapturedFrame();
	}
	return true;
}

void USRDatasetCaptureSubsystem::ApplyLastCapturedEndpointTransforms()
{
	if (!ActiveJob.bUseLastCapturedEndpointTransforms || LastCapturedEndpointTransforms.IsEmpty())
	{
		return;
	}
	for (const TPair<TWeakObjectPtr<USceneComponent>, FTransform>& Pair : LastCapturedEndpointTransforms)
	{
		if (USceneComponent* Component = Pair.Key.Get())
		{
			FMotionVectorSimulation::Get().SetPreviousTransform(Component, Pair.Value);
		}
	}
}

void USRDatasetCaptureSubsystem::SnapshotCapturedEndpointTransforms()
{
	if (!ActiveJob.bUseLastCapturedEndpointTransforms || !GetWorld())
	{
		return;
	}
	LastCapturedEndpointTransforms.Reset();
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		TInlineComponentArray<USceneComponent*> Components;
		It->GetComponents(Components);
		for (USceneComponent* Component : Components)
		{
			if (IsValid(Component) && Component->IsRegistered())
			{
				LastCapturedEndpointTransforms.Add(Component, Component->GetComponentTransform());
			}
		}
	}
}

bool USRDatasetCaptureSubsystem::FinalizePendingMainViewCapture(FString& OutError)
{
	if (!bMainViewCapturePending)
	{
		return true;
	}

	TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> ViewExtension = GetSRDatasetViewExtension();
	if (!ViewExtension)
	{
		OutError = TEXT("The SRDataset RDG view extension disappeared while a Main View capture was pending.");
		return false;
	}

	FSRDatasetTemporalCaptureResult TemporalResult;
	const int32 FirstCapturedFrame = ActiveJob.StartFrame + ActiveJob.CaptureFrameOffset;
	const bool bHistoryReset = PendingMainViewFrameNumber == FirstCapturedFrame;
	const int32 MotionPreviousLogicalFrameId =
		ActiveJob.ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate
			? PendingMainViewFrameNumber - 1
			: (bHistoryReset
				? PendingMainViewFrameNumber
				: PendingMainViewFrameNumber - ActiveJob.FrameStep);
	if (!ViewExtension->WaitAndTakeCapture(TemporalResult, OutError) ||
		!CaptureRig->SaveTemporalCaptureResult(
			TemporalResult,
			MakeFramePath(TEXT("lr"), PendingMainViewFrameNumber, TEXT("png")),
			PendingMainViewFrameNumber,
			MotionPreviousLogicalFrameId,
			bHistoryReset,
			PendingMainViewHashes,
			OutError))
	{
		bMainViewCapturePending = false;
		PendingMainViewFrameNumber = INDEX_NONE;
		PendingMainViewHashes.Reset();
		PendingMainViewRenderSubmissions.Reset();
		return false;
	}
	if (ActiveJob.bCaptureMainViewHUDlessColor)
	{
		const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> TonemapExtension =
			GetSRDatasetTonemapViewExtension();
		FSRDatasetTemporalCaptureResult HUDlessResult;
		if (!TonemapExtension ||
			!TonemapExtension->WaitAndTakeCapture(HUDlessResult, OutError) ||
			!CaptureRig->SaveHUDlessColorResult(
				HUDlessResult,
				MakeFramePath(TEXT("lr"), PendingMainViewFrameNumber, TEXT("png")),
				PendingMainViewHashes,
				OutError))
		{
			bMainViewCapturePending = false;
			PendingMainViewFrameNumber = INDEX_NONE;
			PendingMainViewHashes.Reset();
			PendingMainViewRenderSubmissions.Reset();
			return false;
		}
	}

	++Status.CapturedSamples;
	AppendFrameManifest(
		PendingMainViewFrameNumber,
		PendingMainViewTimeSeconds,
		PendingMainViewHashes,
		PendingMainViewRenderSubmissions,
		false);
	bMainViewCapturePending = false;
	PendingMainViewFrameNumber = INDEX_NONE;
	PendingMainViewTimeSeconds = 0.0;
	PendingMainViewHashes.Reset();
	PendingMainViewRenderSubmissions.Reset();

	if (ActiveJob.bWriteManifestEveryFrame && !WriteManifest(OutError))
	{
		return false;
	}
	return true;
}

bool USRDatasetCaptureSubsystem::IsFrameAlreadyComplete(const int32 FrameNumber) const
{
	if (!IFileManager::Get().FileExists(*MakeFramePath(TEXT("hr"), FrameNumber, TEXT("png"))) ||
		!IFileManager::Get().FileExists(*MakeFramePath(TEXT("lr"), FrameNumber, TEXT("png"))))
	{
		return false;
	}
	if (ActiveJob.bCaptureDepth && !IFileManager::Get().FileExists(*MakeFramePath(TEXT("depth"), FrameNumber, TEXT("exr"))))
	{
		return false;
	}
	if (ActiveJob.bCaptureTemporalDiagnostics)
	{
		for (const TCHAR* Modality : SRDataset::Private::TemporalDiagnosticModalities)
		{
			if (!IFileManager::Get().FileExists(*MakeFramePath(Modality, FrameNumber, TEXT("exr"))))
			{
				return false;
			}
		}
	}
	if (ActiveJob.bCaptureReferenceHR &&
		!IFileManager::Get().FileExists(*MakeFramePath(
			SRDataset::Private::ReferenceHRModality, FrameNumber, TEXT("exr"))))
	{
		return false;
	}
	if (ActiveJob.bCaptureMainViewHUDlessColor &&
		!IFileManager::Get().FileExists(*MakeFramePath(
			SRDataset::Private::HUDlessColorModality, FrameNumber, TEXT("exr"))))
	{
		return false;
	}
	return true;
}

void USRDatasetCaptureSubsystem::AppendFrameManifest(
	const int32 FrameNumber,
	const double TimeSeconds,
	const TMap<FString, FString>& Hashes,
	const TMap<FString, int64>& RenderSubmissions,
	const bool bResumed)
{
	TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
	const int32 FirstCapturedFrame = ActiveJob.StartFrame + ActiveJob.CaptureFrameOffset;
	const bool bFirstCapturedFrame = FrameNumber == FirstCapturedFrame;
	const int32 PreviousCapturedFrame = bFirstCapturedFrame ? FrameNumber : FrameNumber - ActiveJob.FrameStep;
	const bool bIntermediateReplay = ActiveJob.ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate;
	const int32 MotionPreviousFrame = bIntermediateReplay ? FrameNumber - 1 : PreviousCapturedFrame;
	const int32 MotionTimeSpanFrames = bFirstCapturedFrame && !bIntermediateReplay
		? 0
		: FrameNumber - MotionPreviousFrame;
	Frame->SetNumberField(TEXT("frame"), FrameNumber);
	Frame->SetStringField(
		TEXT("replayPass"),
		StaticEnum<ESRDatasetReplayPass>()->GetNameStringByValue(static_cast<int64>(ActiveJob.ReplayPass)));
	Frame->SetNumberField(TEXT("logicalFrameId"), FrameNumber);
	Frame->SetNumberField(TEXT("previousCapturedLogicalFrameId"), PreviousCapturedFrame);
	Frame->SetNumberField(TEXT("motionPreviousLogicalFrameId"), MotionPreviousFrame);
	Frame->SetNumberField(TEXT("motionTimeSpanFrames"), MotionTimeSpanFrames);
	Frame->SetNumberField(
		TEXT("motionTimeSpanS"),
		MotionTimeSpanFrames * ActiveJob.GetFixedDeltaSeconds());
	Frame->SetBoolField(TEXT("motionTrainingUsable"), !bIntermediateReplay);
	Frame->SetBoolField(TEXT("endpointPreviousTransformOverride"), ActiveJob.bUseLastCapturedEndpointTransforms);
	Frame->SetStringField(
		TEXT("auxiliaryCaptureOrder"),
		StaticEnum<ESRDatasetAuxiliaryCaptureOrder>()->GetNameStringByValue(
			static_cast<int64>(ActiveJob.AuxiliaryCaptureOrder)));
	Frame->SetNumberField(TEXT("simulationTick"), FrameNumber);
	Frame->SetNumberField(TEXT("simulationTimeS"), TimeSeconds);
	Frame->SetNumberField(TEXT("deltaTimeS"), ActiveJob.GetFixedDeltaSeconds());
	Frame->SetBoolField(TEXT("simulationAdvance"), true);
	Frame->SetBoolField(TEXT("historyAdvance"), true);
	Frame->SetBoolField(TEXT("reset"), bFirstCapturedFrame);
	Frame->SetStringField(TEXT("resetReason"), bFirstCapturedFrame ? TEXT("job_start") : TEXT("none"));
	Frame->SetNumberField(TEXT("timeSeconds"), TimeSeconds);
	Frame->SetBoolField(TEXT("resumed"), bResumed);
	int32 StreamingTextureCount = 0;
	int32 PendingStreamingTextureCount = 0;
	Frame->SetStringField(
		TEXT("streamingStateSha1"),
		ComputeStreamingStateSha1(StreamingTextureCount, PendingStreamingTextureCount));
	Frame->SetNumberField(TEXT("streamingTextureCount"), StreamingTextureCount);
	Frame->SetNumberField(TEXT("pendingStreamingTextureCount"), PendingStreamingTextureCount);
	Frame->SetNumberField(
		TEXT("streamingRequestsWanting"),
		IStreamingManager::HasShutdown() ? -1 : IStreamingManager::Get().GetNumWantingResources());
	const FSceneStateSummary SceneState = ComputeSceneStateSummary();
	Frame->SetStringField(TEXT("sceneStateSha1"), SceneState.Sha1);
	Frame->SetNumberField(TEXT("sceneActorCount"), SceneState.ActorCount);
	Frame->SetNumberField(TEXT("sceneComponentCount"), SceneState.ComponentCount);
	Frame->SetNumberField(TEXT("sceneSkeletalComponentCount"), SceneState.SkeletalComponentCount);
	Frame->SetNumberField(TEXT("sceneBoneCount"), SceneState.BoneCount);
	Frame->SetNumberField(TEXT("sceneFXComponentCount"), SceneState.FXComponentCount);
	Frame->SetNumberField(TEXT("sceneNiagaraComponentCount"), SceneState.NiagaraComponentCount);
	Frame->SetNumberField(TEXT("sceneControllableActorCount"), SceneState.ControllableActorCount);
	Frame->SetNumberField(
		TEXT("sceneUncontrolledTickingActorCount"),
		SceneState.UncontrolledTickingActorCount);
	const auto StringArray = [](const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	};
	Frame->SetArrayField(TEXT("sceneControllableActors"), StringArray(SceneState.ControllableActors));
	Frame->SetArrayField(
		TEXT("sceneUncontrolledTickingActors"),
		StringArray(SceneState.UncontrolledTickingActors));
	Frame->SetStringField(
		TEXT("sceneStateHashScope"),
		TEXT("sorted_actor_component_transforms_visibility_tick_controllable_skeletal_component_space_bones_niagara_component_state_cascade_component_state_not_particle_payload"));

	TArray<TSharedPtr<FJsonValue>> SubmissionArray;
	TArray<FString> SubmissionModalities;
	RenderSubmissions.GetKeys(SubmissionModalities);
	SubmissionModalities.Sort([&RenderSubmissions](const FString& Left, const FString& Right)
	{
		return RenderSubmissions.FindChecked(Left) < RenderSubmissions.FindChecked(Right);
	});
	for (const FString& Modality : SubmissionModalities)
	{
		TSharedRef<FJsonObject> Submission = MakeShared<FJsonObject>();
		Submission->SetStringField(TEXT("modality"), Modality);
		Submission->SetNumberField(TEXT("renderSubmissionId"), RenderSubmissions.FindChecked(Modality));
		Submission->SetBoolField(TEXT("simulationAdvance"), false);
		Submission->SetStringField(
			TEXT("viewState"),
			Modality == TEXT("main_view_temporal")
				? TEXT("player_main_view")
				: Modality + TEXT("_scene_capture"));
		SubmissionArray.Add(MakeShared<FJsonValueObject>(Submission));
	}
	Frame->SetArrayField(TEXT("renderSubmissions"), SubmissionArray);

	TSharedRef<FJsonObject> Files = MakeShared<FJsonObject>();
	Files->SetStringField(TEXT("hr"), FString::Printf(TEXT("hr/frame_%06d.png"), FrameNumber));
	Files->SetStringField(TEXT("lr"), FString::Printf(TEXT("lr/frame_%06d.png"), FrameNumber));
	if (ActiveJob.bCaptureDepth)
	{
		Files->SetStringField(TEXT("depth"), FString::Printf(TEXT("depth/frame_%06d.exr"), FrameNumber));
	}
	if (ActiveJob.bCaptureTemporalDiagnostics)
	{
		for (const TCHAR* Modality : SRDataset::Private::TemporalDiagnosticModalities)
		{
			Files->SetStringField(Modality, FString::Printf(TEXT("%s/frame_%06d.exr"), Modality, FrameNumber));
		}
	}
	if (ActiveJob.bCaptureReferenceHR)
	{
		Files->SetStringField(
			SRDataset::Private::ReferenceHRModality,
			FString::Printf(TEXT("%s/frame_%06d.exr"), SRDataset::Private::ReferenceHRModality, FrameNumber));
	}
	if (ActiveJob.bCaptureMainViewHUDlessColor)
	{
		Files->SetStringField(
			SRDataset::Private::HUDlessColorModality,
			FString::Printf(TEXT("%s/frame_%06d.exr"), SRDataset::Private::HUDlessColorModality, FrameNumber));
	}
	Frame->SetObjectField(TEXT("files"), Files);

	TSharedRef<FJsonObject> HashObject = MakeShared<FJsonObject>();
	TArray<FString> HashKeys;
	Hashes.GetKeys(HashKeys);
	HashKeys.Sort();
	for (const FString& Key : HashKeys)
	{
		HashObject->SetStringField(Key, Hashes.FindChecked(Key));
	}
	Frame->SetObjectField(TEXT("sha1"), HashObject);

	TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
	const FVector Location = CaptureRig->GetActorLocation();
	const FRotator Rotation = CaptureRig->GetActorRotation();
	const FMinimalViewInfo& CameraView = CaptureRig->GetLastCameraView();
	Camera->SetArrayField(TEXT("locationCm"), {
		MakeShared<FJsonValueNumber>(Location.X), MakeShared<FJsonValueNumber>(Location.Y), MakeShared<FJsonValueNumber>(Location.Z) });
	Camera->SetArrayField(TEXT("rotationDeg"), {
		MakeShared<FJsonValueNumber>(Rotation.Pitch), MakeShared<FJsonValueNumber>(Rotation.Yaw), MakeShared<FJsonValueNumber>(Rotation.Roll) });
	Camera->SetStringField(TEXT("projection"), CameraView.ProjectionMode == ECameraProjectionMode::Perspective ? TEXT("Perspective") : TEXT("Orthographic"));
	Camera->SetNumberField(TEXT("fovDegrees"), CameraView.FOV);
	if (ActiveJob.bEnableSemanticValidationFixture && ValidationFixture)
	{
		const FSRDatasetValidationFixtureFrame& FixtureFrame = ValidationFixture->GetFrameMetadata();
		TSharedRef<FJsonObject> Fixture = MakeShared<FJsonObject>();
		Fixture->SetBoolField(TEXT("enabled"), true);
		Fixture->SetBoolField(TEXT("sourceLevelGeometryHidden"), true);
		Fixture->SetNumberField(TEXT("logicalFrameId"), FixtureFrame.LogicalFrame);
		Fixture->SetNumberField(TEXT("movingObjectId"), ASRDatasetValidationFixture::MovingObjectId);
		Fixture->SetNumberField(TEXT("backgroundObjectId"), ASRDatasetValidationFixture::BackgroundObjectId);
		Fixture->SetNumberField(TEXT("translucentObjectId"), ASRDatasetValidationFixture::TranslucentObjectId);
		Fixture->SetNumberField(TEXT("movingCurrentRightCm"), FixtureFrame.MovingCurrentRightCm);
		Fixture->SetNumberField(TEXT("movingPreviousRightCm"), FixtureFrame.MovingPreviousRightCm);
		Fixture->SetArrayField(TEXT("expectedMovingMotionDisplayPixels"), {
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedMovingMotionDisplayPixels.X),
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedMovingMotionDisplayPixels.Y) });
		TSharedRef<FJsonObject> ExpectedDepth = MakeShared<FJsonObject>();
		for (const TPair<int32, float>& Pair : FixtureFrame.ExpectedFrontDepthMeters)
		{
			ExpectedDepth->SetNumberField(FString::FromInt(Pair.Key), Pair.Value);
		}
		Fixture->SetObjectField(TEXT("expectedFrontDepthMetersByObjectId"), ExpectedDepth);
		Frame->SetObjectField(TEXT("semanticValidationFixture"), Fixture);
	}
	Camera->SetNumberField(TEXT("orthoWidthCm"), CameraView.OrthoWidth);
	Camera->SetNumberField(TEXT("aspectRatio"), static_cast<double>(ActiveJob.HRResolution.X) / ActiveJob.HRResolution.Y);
	if (CameraView.ProjectionMode == ECameraProjectionMode::Perspective)
	{
		const double FocalLengthPixels = 0.5 * ActiveJob.HRResolution.X / FMath::Tan(FMath::DegreesToRadians(0.5 * CameraView.FOV));
		Camera->SetNumberField(TEXT("focalLengthPixelsX"), FocalLengthPixels);
	}
	Frame->SetObjectField(TEXT("camera"), Camera);

	if (ActiveJob.bCaptureTemporalDiagnostics && !bResumed)
	{
		const FSRDatasetTemporalFrameMetadata& Metadata = CaptureRig->GetLastTemporalMetadata();
		if (Metadata.bValid)
		{
			const auto Vector2Array = [](const FVector2f& Value)
			{
				return TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueNumber>(Value.X),
					MakeShared<FJsonValueNumber>(Value.Y) };
			};
			const auto MatrixArray = [](const FMatrix44f& Matrix)
			{
				TArray<TSharedPtr<FJsonValue>> Values;
				Values.Reserve(16);
				for (int32 Row = 0; Row < 4; ++Row)
				{
					for (int32 Column = 0; Column < 4; ++Column)
					{
						Values.Add(MakeShared<FJsonValueNumber>(Matrix.M[Row][Column]));
					}
				}
				return Values;
			};
			const auto Vector3Array = [](const FVector3f& Value)
			{
				return TArray<TSharedPtr<FJsonValue>>{
					MakeShared<FJsonValueNumber>(Value.X),
					MakeShared<FJsonValueNumber>(Value.Y),
					MakeShared<FJsonValueNumber>(Value.Z) };
			};

			TSharedRef<FJsonObject> Temporal = MakeShared<FJsonObject>();
			Temporal->SetStringField(TEXT("pipelineStage"), TEXT("after_dof_before_temporal_upscaler"));
			Temporal->SetStringField(TEXT("colorSpace"), TEXT("linear_scene_rgb"));
			Temporal->SetBoolField(TEXT("preExposed"), true);
			Temporal->SetNumberField(TEXT("preExposure"), Metadata.PreExposure);
			Temporal->SetNumberField(TEXT("exposure"), Metadata.OneOverPreExposure);
			Temporal->SetNumberField(TEXT("renderDeltaTimeS"), Metadata.DeltaTimeSeconds);
			Temporal->SetNumberField(TEXT("renderGameTimeS"), Metadata.GameTimeSeconds);
			Temporal->SetArrayField(TEXT("renderSize"), {
				MakeShared<FJsonValueNumber>(Metadata.ViewSize.X), MakeShared<FJsonValueNumber>(Metadata.ViewSize.Y) });
			Temporal->SetArrayField(TEXT("displaySize"), {
				MakeShared<FJsonValueNumber>(ActiveJob.HRResolution.X), MakeShared<FJsonValueNumber>(ActiveJob.HRResolution.Y) });
			Temporal->SetArrayField(TEXT("viewRect"), {
				MakeShared<FJsonValueNumber>(Metadata.ViewRectMin.X), MakeShared<FJsonValueNumber>(Metadata.ViewRectMin.Y),
				MakeShared<FJsonValueNumber>(Metadata.ViewSize.X), MakeShared<FJsonValueNumber>(Metadata.ViewSize.Y) });
			Temporal->SetArrayField(TEXT("sceneBufferSize"), {
				MakeShared<FJsonValueNumber>(Metadata.BufferSize.X), MakeShared<FJsonValueNumber>(Metadata.BufferSize.Y) });
			Temporal->SetNumberField(TEXT("resolutionFraction"), Metadata.ResolutionFraction);
			Temporal->SetNumberField(TEXT("inverseResolutionFraction"), Metadata.InvResolutionFraction);
			const double GlobalMipMapLODBias = FCString::Atod(*CaptureCVarProfile.FindRef(TEXT("r.MipMapLODBias")));
			Temporal->SetNumberField(TEXT("automaticViewMipBias"), Metadata.MaterialTextureMipBias);
			Temporal->SetStringField(TEXT("automaticViewMipBiasPolicy"), TEXT("engine_main_view_spatial_upscale"));
			Temporal->SetNumberField(TEXT("globalMipMapLODBias"), GlobalMipMapLODBias);
			Temporal->SetNumberField(
				TEXT("effectiveMaterialTextureMipBias"),
				Metadata.MaterialTextureMipBias + GlobalMipMapLODBias);
			Temporal->SetStringField(
				TEXT("effectiveMipBiasScope"),
				TEXT("automatic_view_mip_bias_material_samples_plus_global_mipmap_lod_bias"));
			Temporal->SetNumberField(TEXT("screenPercentage"),
				100.0 * static_cast<double>(ActiveJob.LRResolution.X) / ActiveJob.HRResolution.X);
			Temporal->SetBoolField(TEXT("dynamicResolutionEnabled"), false);
			Temporal->SetNumberField(TEXT("renderFrameNumber"), Metadata.RenderFrameNumber);
			Temporal->SetNumberField(TEXT("stateFrameIndex"), Metadata.StateFrameIndex);
			Temporal->SetNumberField(TEXT("stateFrameIndexMod8"), Metadata.StateFrameIndexMod8);
			Temporal->SetArrayField(TEXT("jitterCurrentNDC"), Vector2Array(Metadata.JitterCurrentNDC));
			Temporal->SetArrayField(TEXT("jitterPreviousNDC"), Vector2Array(Metadata.JitterPreviousNDC));
			const FVector2f CurrentRenderPixels(
				Metadata.JitterCurrentNDC.X * 0.5f * ActiveJob.LRResolution.X,
				-Metadata.JitterCurrentNDC.Y * 0.5f * ActiveJob.LRResolution.Y);
			const FVector2f PreviousRenderPixels(
				Metadata.JitterPreviousNDC.X * 0.5f * ActiveJob.LRResolution.X,
				-Metadata.JitterPreviousNDC.Y * 0.5f * ActiveJob.LRResolution.Y);
			Temporal->SetArrayField(TEXT("jitterCurrentRenderPixel"), Vector2Array(CurrentRenderPixels));
			Temporal->SetArrayField(TEXT("jitterPreviousRenderPixel"), Vector2Array(PreviousRenderPixels));
			Temporal->SetArrayField(TEXT("jitterCurrentDisplayPixel"), Vector2Array(FVector2f(
				CurrentRenderPixels.X * ActiveJob.HRResolution.X / ActiveJob.LRResolution.X,
				CurrentRenderPixels.Y * ActiveJob.HRResolution.Y / ActiveJob.LRResolution.Y)));
			Temporal->SetArrayField(TEXT("jitterPreviousDisplayPixel"), Vector2Array(FVector2f(
				PreviousRenderPixels.X * ActiveJob.HRResolution.X / ActiveJob.LRResolution.X,
				PreviousRenderPixels.Y * ActiveJob.HRResolution.Y / ActiveJob.LRResolution.Y)));
			Temporal->SetStringField(TEXT("jitterSequence"), TEXT("engine_temporal_upscaler_owned"));
			Temporal->SetNumberField(TEXT("jitterIndex"), Metadata.StateFrameIndex);
			Temporal->SetStringField(TEXT("matrixLayout"), TEXT("row_major_flat_16"));
			Temporal->SetStringField(TEXT("matrixVectorConvention"), TEXT("row_vector_mul_matrix"));
			Temporal->SetStringField(TEXT("coordinateSystem"), TEXT("Unreal_left_handed_reversed_z"));
			Temporal->SetStringField(TEXT("clipZRange"), TEXT("zero_to_one_reversed_z"));
			Temporal->SetArrayField(TEXT("viewToClipCurrentJittered"), MatrixArray(Metadata.ViewToClipJittered));
			Temporal->SetArrayField(TEXT("viewToClipCurrentUnjittered"), MatrixArray(Metadata.ViewToClipUnjittered));
			Temporal->SetArrayField(TEXT("viewToClipPrevious"), MatrixArray(Metadata.PreviousViewToClip));
			Temporal->SetArrayField(TEXT("viewToClipPreviousUnjittered"), MatrixArray(Metadata.PreviousViewToClipUnjittered));
			Temporal->SetArrayField(TEXT("clipToPreviousClipUnjittered"), MatrixArray(Metadata.ClipToPreviousClipUnjittered));
			Temporal->SetArrayField(TEXT("clipToPreviousClipJittered"), MatrixArray(Metadata.ClipToPreviousClipJittered));
			Temporal->SetArrayField(TEXT("translatedWorldToViewCurrent"), MatrixArray(Metadata.TranslatedWorldToViewCurrent));
			Temporal->SetArrayField(TEXT("viewToTranslatedWorldCurrent"), MatrixArray(Metadata.ViewToTranslatedWorldCurrent));
			Temporal->SetArrayField(TEXT("translatedWorldToClipCurrentJittered"), MatrixArray(Metadata.TranslatedWorldToClipCurrentJittered));
			Temporal->SetArrayField(TEXT("translatedWorldToClipCurrentUnjittered"), MatrixArray(Metadata.TranslatedWorldToClipCurrentUnjittered));
			Temporal->SetArrayField(TEXT("clipToTranslatedWorldCurrentJittered"), MatrixArray(Metadata.ClipToTranslatedWorldCurrentJittered));
			Temporal->SetArrayField(TEXT("translatedWorldToViewPrevious"), MatrixArray(Metadata.TranslatedWorldToViewPrevious));
			Temporal->SetArrayField(TEXT("viewToTranslatedWorldPrevious"), MatrixArray(Metadata.ViewToTranslatedWorldPrevious));
			Temporal->SetArrayField(TEXT("translatedWorldToClipPreviousJittered"), MatrixArray(Metadata.TranslatedWorldToClipPreviousJittered));
			Temporal->SetArrayField(TEXT("translatedWorldToClipPreviousUnjittered"), MatrixArray(Metadata.TranslatedWorldToClipPreviousUnjittered));
			Temporal->SetArrayField(TEXT("worldViewOriginHighCurrent"), Vector3Array(Metadata.WorldViewOriginHighCurrent));
			Temporal->SetArrayField(TEXT("worldViewOriginLowCurrent"), Vector3Array(Metadata.WorldViewOriginLowCurrent));
			Temporal->SetArrayField(TEXT("preViewTranslationHighCurrent"), Vector3Array(Metadata.PreViewTranslationHighCurrent));
			Temporal->SetArrayField(TEXT("preViewTranslationLowCurrent"), Vector3Array(Metadata.PreViewTranslationLowCurrent));
			Temporal->SetArrayField(TEXT("worldViewOriginHighPrevious"), Vector3Array(Metadata.WorldViewOriginHighPrevious));
			Temporal->SetArrayField(TEXT("worldViewOriginLowPrevious"), Vector3Array(Metadata.WorldViewOriginLowPrevious));
			Temporal->SetArrayField(TEXT("preViewTranslationHighPrevious"), Vector3Array(Metadata.PreViewTranslationHighPrevious));
			Temporal->SetArrayField(TEXT("preViewTranslationLowPrevious"), Vector3Array(Metadata.PreViewTranslationLowPrevious));
			Temporal->SetNumberField(TEXT("nearPlane"), Metadata.NearPlane);
			Temporal->SetNumberField(TEXT("orthoFarPlane"), Metadata.OrthoFarPlane);
			Temporal->SetBoolField(TEXT("reversedZ"), true);
			Temporal->SetBoolField(TEXT("infiniteFar"), CameraView.ProjectionMode == ECameraProjectionMode::Perspective);
			Temporal->SetNumberField(TEXT("viewSpaceToMeters"), 0.01);
			Temporal->SetStringField(TEXT("worldReconstruction"),
				TEXT("clip_to_translated_world_then_subtract_split_pre_view_translation"));
			Temporal->SetStringField(TEXT("motionDefinition"), TEXT("previous_pixel = current_pixel + motion_current_to_previous"));
			Temporal->SetStringField(TEXT("motionUnit"), TEXT("display_pixel"));
			Temporal->SetStringField(TEXT("motionOrigin"), TEXT("top_left"));
			Temporal->SetStringField(TEXT("velocityRawUnit"), TEXT("UE_normalized_screen_current_minus_previous"));
			Temporal->SetStringField(TEXT("motionSource"), TEXT("UE_velocity_where_covered_else_depth_camera_reconstruction"));
			Temporal->SetStringField(
				TEXT("historyRejectionDefinition"),
				TEXT("one_rejects_previous_history_at_motion_reprojected_pixel"));
			Temporal->SetStringField(
				TEXT("historyRejectionSource"),
				TEXT("custom_stencil_identity_else_static_camera_depth_reprojection_v1"));
			Temporal->SetBoolField(TEXT("historyRejectionTrainingUsable"), true);
			Temporal->SetBoolField(TEXT("historyRejectionRequiresValidityMask"), true);
			Temporal->SetBoolField(TEXT("historyRejectionProductionCertified"), false);
			Temporal->SetStringField(
				TEXT("historyRejectionKnownLimit"),
				TEXT("unlabeled_velocity_covered_geometry_is_invalid;custom_stencil_uint8_is_not_instance_unique_and_same_id_self_occlusion_is_unresolved"));
			Temporal->SetNumberField(TEXT("motionPreviousLogicalFrameId"), MotionPreviousFrame);
			Temporal->SetNumberField(TEXT("motionTimeSpanS"), MotionTimeSpanFrames * ActiveJob.GetFixedDeltaSeconds());
			Temporal->SetBoolField(TEXT("motionTrainingUsable"), !bIntermediateReplay);
			Temporal->SetBoolField(TEXT("motionIncludesCamera"), true);
			Temporal->SetBoolField(TEXT("motionJitterRemoved"), true);
			Temporal->SetStringField(TEXT("transparencyMaskSource"), TEXT("one_minus_post_dof_separate_translucency_transmittance"));
			Temporal->SetStringField(TEXT("reactiveMaskSource"), TEXT("conservative_post_dof_transparency_coverage_v1"));
			Temporal->SetBoolField(TEXT("reactiveMaskIncludesOpaqueAnimation"), false);
			Temporal->SetStringField(TEXT("objectIdSource"), TEXT("custom_stencil_uint8_zero_unlabeled"));
			Frame->SetObjectField(TEXT("temporalDiagnostics"), Temporal);

			const FSRDatasetTemporalFrameMetadata& NativeHR = CaptureRig->GetLastNativeHRMetadata();
			if (NativeHR.bValid)
			{
				TSharedRef<FJsonObject> Native = MakeShared<FJsonObject>();
				Native->SetStringField(TEXT("pipelineStage"), TEXT("after_dof_before_temporal_upscaler"));
				Native->SetStringField(TEXT("colorSpace"), TEXT("linear_scene_rgb"));
				Native->SetBoolField(TEXT("preExposed"), true);
				Native->SetNumberField(TEXT("preExposure"), NativeHR.PreExposure);
				Native->SetNumberField(TEXT("exposure"), NativeHR.OneOverPreExposure);
				Native->SetNumberField(TEXT("automaticViewMipBias"), NativeHR.MaterialTextureMipBias);
				Native->SetStringField(TEXT("automaticViewMipBiasPolicy"), TEXT("isolated_full_resolution_scene_capture"));
				Native->SetNumberField(TEXT("globalMipMapLODBias"), GlobalMipMapLODBias);
				Native->SetNumberField(
					TEXT("effectiveMaterialTextureMipBias"),
					NativeHR.MaterialTextureMipBias + GlobalMipMapLODBias);
				Native->SetArrayField(TEXT("renderSize"), {
					MakeShared<FJsonValueNumber>(NativeHR.ViewSize.X), MakeShared<FJsonValueNumber>(NativeHR.ViewSize.Y) });
				Native->SetArrayField(TEXT("displaySize"), {
					MakeShared<FJsonValueNumber>(ActiveJob.HRResolution.X), MakeShared<FJsonValueNumber>(ActiveJob.HRResolution.Y) });
				Native->SetArrayField(TEXT("jitterCurrentNDC"), Vector2Array(NativeHR.JitterCurrentNDC));
				Native->SetArrayField(TEXT("jitterPreviousNDC"), Vector2Array(NativeHR.JitterPreviousNDC));
				Native->SetBoolField(TEXT("fixedOutputGrid"), NativeHR.JitterCurrentNDC.IsNearlyZero());
				Native->SetStringField(TEXT("viewState"), TEXT("isolated_hr_scene_capture"));
				Native->SetArrayField(TEXT("viewToClipCurrentJittered"), MatrixArray(NativeHR.ViewToClipJittered));
				Native->SetArrayField(TEXT("viewToClipCurrentUnjittered"), MatrixArray(NativeHR.ViewToClipUnjittered));
				Native->SetArrayField(TEXT("translatedWorldToViewCurrent"), MatrixArray(NativeHR.TranslatedWorldToViewCurrent));
				Native->SetArrayField(TEXT("worldViewOriginHighCurrent"), Vector3Array(NativeHR.WorldViewOriginHighCurrent));
				Native->SetArrayField(TEXT("worldViewOriginLowCurrent"), Vector3Array(NativeHR.WorldViewOriginLowCurrent));
				Frame->SetObjectField(TEXT("nativeHRDiagnostics"), Native);
			}

			const FSRDatasetTemporalFrameMetadata& ReferenceHR = CaptureRig->GetLastReferenceHRMetadata();
			if (ActiveJob.bCaptureReferenceHR && ReferenceHR.bValid)
			{
				TSharedRef<FJsonObject> Reference = MakeShared<FJsonObject>();
				Reference->SetStringField(TEXT("pipelineStage"), TEXT("after_dof_before_temporal_upscaler"));
				Reference->SetStringField(TEXT("colorSpace"), TEXT("linear_scene_rgb"));
				Reference->SetBoolField(TEXT("preExposed"), true);
				Reference->SetNumberField(TEXT("preExposure"), ReferenceHR.PreExposure);
				Reference->SetNumberField(TEXT("exposure"), ReferenceHR.OneOverPreExposure);
				Reference->SetNumberField(TEXT("automaticViewMipBias"), ReferenceHR.MaterialTextureMipBias);
				Reference->SetStringField(TEXT("automaticViewMipBiasPolicy"), TEXT("isolated_full_resolution_scene_capture"));
				Reference->SetNumberField(TEXT("globalMipMapLODBias"), GlobalMipMapLODBias);
				Reference->SetNumberField(
					TEXT("effectiveMaterialTextureMipBias"),
					ReferenceHR.MaterialTextureMipBias + GlobalMipMapLODBias);
				Reference->SetArrayField(TEXT("sourceRenderSize"), {
					MakeShared<FJsonValueNumber>(ReferenceHR.ViewSize.X),
					MakeShared<FJsonValueNumber>(ReferenceHR.ViewSize.Y) });
				Reference->SetArrayField(TEXT("outputSize"), {
					MakeShared<FJsonValueNumber>(ActiveJob.HRResolution.X),
					MakeShared<FJsonValueNumber>(ActiveJob.HRResolution.Y) });
				Reference->SetNumberField(TEXT("spatialScalePerAxis"), ActiveJob.ReferenceHRScale);
				Reference->SetStringField(
					TEXT("downsampleFilter"),
					StaticEnum<ESRDatasetResizeFilter>()->GetNameStringByValue(
						static_cast<int64>(ActiveJob.ReferenceResizeFilter)));
				Reference->SetArrayField(TEXT("jitterCurrentNDC"), Vector2Array(ReferenceHR.JitterCurrentNDC));
				Reference->SetArrayField(TEXT("jitterPreviousNDC"), Vector2Array(ReferenceHR.JitterPreviousNDC));
				Reference->SetBoolField(TEXT("fixedOutputGrid"), ReferenceHR.JitterCurrentNDC.IsNearlyZero());
				Reference->SetBoolField(TEXT("historyAdvance"), false);
				Reference->SetBoolField(TEXT("simulationAdvance"), false);
				Reference->SetStringField(TEXT("viewState"), TEXT("isolated_reference_scene_capture"));
				Reference->SetArrayField(TEXT("viewToClipCurrentJittered"), MatrixArray(ReferenceHR.ViewToClipJittered));
				Reference->SetArrayField(TEXT("viewToClipCurrentUnjittered"), MatrixArray(ReferenceHR.ViewToClipUnjittered));
				Reference->SetArrayField(TEXT("translatedWorldToViewCurrent"), MatrixArray(ReferenceHR.TranslatedWorldToViewCurrent));
				Reference->SetArrayField(TEXT("worldViewOriginHighCurrent"), Vector3Array(ReferenceHR.WorldViewOriginHighCurrent));
				Reference->SetArrayField(TEXT("worldViewOriginLowCurrent"), Vector3Array(ReferenceHR.WorldViewOriginLowCurrent));
				Frame->SetObjectField(TEXT("referenceHRDiagnostics"), Reference);
			}

			const FSRDatasetTemporalFrameMetadata& HUDless = CaptureRig->GetLastHUDlessColorMetadata();
			if (ActiveJob.bCaptureMainViewHUDlessColor && HUDless.bValid)
			{
				const FIntPoint HUDlessSize = CaptureRig->GetLastHUDlessColorSize();
				TSharedRef<FJsonObject> HUDlessDiagnostics = MakeShared<FJsonObject>();
				HUDlessDiagnostics->SetStringField(TEXT("pipelineStage"), TEXT("after_tonemap_before_ui"));
				HUDlessDiagnostics->SetStringField(TEXT("colorEncoding"), TEXT("tonemapper_output_device_encoded"));
				HUDlessDiagnostics->SetBoolField(TEXT("uiIncluded"), false);
				HUDlessDiagnostics->SetBoolField(TEXT("hudIncluded"), false);
				HUDlessDiagnostics->SetBoolField(TEXT("displayResolution"), true);
				HUDlessDiagnostics->SetArrayField(TEXT("size"), {
					MakeShared<FJsonValueNumber>(HUDlessSize.X),
					MakeShared<FJsonValueNumber>(HUDlessSize.Y) });
				HUDlessDiagnostics->SetNumberField(TEXT("preExposureBeforeTonemap"), HUDless.PreExposure);
				HUDlessDiagnostics->SetNumberField(TEXT("automaticViewMipBias"), HUDless.MaterialTextureMipBias);
				HUDlessDiagnostics->SetStringField(TEXT("automaticViewMipBiasPolicy"), TEXT("engine_main_view_spatial_upscale"));
				HUDlessDiagnostics->SetNumberField(TEXT("globalMipMapLODBias"), GlobalMipMapLODBias);
				HUDlessDiagnostics->SetNumberField(
					TEXT("effectiveMaterialTextureMipBias"),
					HUDless.MaterialTextureMipBias + GlobalMipMapLODBias);
				HUDlessDiagnostics->SetNumberField(TEXT("outputDevice"),
					CaptureCVarProfile.Contains(TEXT("r.HDR.Display.OutputDevice"))
						? FCString::Atoi(*CaptureCVarProfile.FindChecked(TEXT("r.HDR.Display.OutputDevice")))
						: -1);
				HUDlessDiagnostics->SetNumberField(TEXT("colorGamut"),
					CaptureCVarProfile.Contains(TEXT("r.HDR.Display.ColorGamut"))
						? FCString::Atoi(*CaptureCVarProfile.FindChecked(TEXT("r.HDR.Display.ColorGamut")))
						: -1);
				HUDlessDiagnostics->SetBoolField(TEXT("hdrOutputEnabled"),
					CaptureCVarProfile.FindRef(TEXT("r.HDR.EnableHDROutput")) == TEXT("1"));
				Frame->SetObjectField(TEXT("hudlessColorDiagnostics"), HUDlessDiagnostics);
			}
		}
	}

	ManifestFrames.Add(MakeShared<FJsonValueObject>(Frame));
}

bool USRDatasetCaptureSubsystem::WriteManifest(FString& OutError) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 2);
	Root->SetStringField(TEXT("pluginVersion"), TEXT("0.3.2"));
	Root->SetStringField(TEXT("contractVersion"), ActiveJob.ContractVersion);
	Root->SetStringField(
		TEXT("replayPass"),
		StaticEnum<ESRDatasetReplayPass>()->GetNameStringByValue(static_cast<int64>(ActiveJob.ReplayPass)));
	Root->SetStringField(TEXT("certificationStatus"), TEXT("certified_spatial_only"));
	Root->SetBoolField(TEXT("temporalTrainingCertified"), false);
	Root->SetBoolField(TEXT("frameGenerationCertified"), false);
	Root->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Root->SetStringField(TEXT("world"), GetWorld() ? GetWorld()->GetOutermost()->GetName() : FString());
	Root->SetStringField(TEXT("state"), StaticEnum<ESRDatasetCaptureState>()->GetNameStringByValue(static_cast<int64>(Status.State)));
	Root->SetStringField(TEXT("error"), Status.LastError);
	Root->SetNumberField(TEXT("capturedSamples"), Status.CapturedSamples);
	Root->SetNumberField(TEXT("skippedSamples"), Status.SkippedSamples);

	TSharedRef<FJsonObject> JobObject = MakeShared<FJsonObject>();
	FJsonObjectConverter::UStructToJsonObject(FSRDatasetCaptureJob::StaticStruct(), &ActiveJob, JobObject, 0, 0);
	Root->SetObjectField(TEXT("job"), JobObject);

	TSharedRef<FJsonObject> Provenance = MakeShared<FJsonObject>();
	Provenance->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Provenance->SetNumberField(TEXT("engineChangelist"), FEngineVersion::Current().GetChangelist());
	Provenance->SetStringField(TEXT("buildVersion"), FApp::GetBuildVersion());
	Provenance->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	Provenance->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("unavailable"));
	Provenance->SetStringField(TEXT("gpuAdapter"), GRHIAdapterName);
	Provenance->SetStringField(TEXT("gpuDriverInternal"), GRHIAdapterInternalDriverVersion);
	Provenance->SetStringField(TEXT("gpuDriverUser"), GRHIAdapterUserDriverVersion);
	Provenance->SetNumberField(TEXT("gpuVendorId"), GRHIVendorId);
	Provenance->SetNumberField(TEXT("gpuDeviceId"), GRHIDeviceId);
	Provenance->SetStringField(TEXT("captureConfigSha1"), CaptureConfigSha1);
	Provenance->SetStringField(TEXT("cvarProfileSha1"), CaptureCVarProfileSha1);
	Provenance->SetStringField(TEXT("cvarProfileCanonical"), CaptureCVarProfileCanonical);
	Provenance->SetStringField(TEXT("contentMapSha1"), ContentMapSha1);
	Provenance->SetStringField(TEXT("shaderSourceSha1"), ShaderSourceSha1);
	Provenance->SetStringField(TEXT("shaderHashScope"), TEXT("SRDatasetExtract.usf_source_not_compiled_bytecode"));
	Provenance->SetBoolField(TEXT("streamingBarrierEnabled"), ActiveJob.bBlockOnStreamingBeforeCapture);
	Provenance->SetNumberField(TEXT("streamingBarrierWaitSeconds"), ActiveJob.StreamingWaitSeconds);
	Provenance->SetBoolField(TEXT("streamingBarrierComplete"), bStreamingBarrierComplete);
	Provenance->SetNumberField(TEXT("streamingRequestsAfterBarrier"), StreamingRequestsAfterBarrier);
	Provenance->SetNumberField(TEXT("streamingTextureCountAfterBarrier"), StreamingTextureCountAfterBarrier);
	Provenance->SetNumberField(
		TEXT("pendingStreamingTextureCountAfterBarrier"),
		PendingStreamingTextureCountAfterBarrier);
	Provenance->SetStringField(TEXT("streamingStateAfterBarrierSha1"), StreamingStateAfterBarrierSha1);
	Provenance->SetStringField(
		TEXT("streamingStateHashScope"),
		TEXT("sorted_loaded_UTexture2D_path_size_asset_mips_resident_mips_streamable_pending"));
	TSharedRef<FJsonObject> CVarProfile = MakeShared<FJsonObject>();
	TArray<FString> CVarNames;
	CaptureCVarProfile.GetKeys(CVarNames);
	CVarNames.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
	});
	for (const FString& Name : CVarNames)
	{
		CVarProfile->SetStringField(Name, CaptureCVarProfile.FindChecked(Name));
	}
	Provenance->SetObjectField(TEXT("cvars"), CVarProfile);
	Root->SetObjectField(TEXT("provenance"), Provenance);

	TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
	Contract->SetStringField(
		TEXT("captureBackend"),
		ActiveJob.bCaptureMainViewTemporalDiagnostics ? TEXT("scene_capture_2d_plus_player_main_view_rdg") : TEXT("scene_capture_2d"));
	Contract->SetStringField(TEXT("colorResource"), TEXT("final_color_ldr"));
	Contract->SetStringField(TEXT("colorPipelineStage"), TEXT("after_tonemap"));
	Contract->SetStringField(TEXT("colorSpace"), TEXT("display_referred_srgb"));
	Contract->SetStringField(TEXT("transferFunction"), TEXT("sRGB_PNG"));
	Contract->SetBoolField(TEXT("preExposed"), false);
	Contract->SetStringField(TEXT("alphaSemantic"), TEXT("opaque_png_alpha"));
	Contract->SetNumberField(TEXT("fixedDeltaSeconds"), ActiveJob.GetFixedDeltaSeconds());
	Contract->SetBoolField(TEXT("fixedEngineTimeStep"), true);
	Contract->SetBoolField(TEXT("chaosDeterminism"), ActiveJob.bEnableChaosDeterminism);
	Contract->SetBoolField(TEXT("niagaraAbsoluteAge"), ActiveJob.bControlNiagara);
	Contract->SetBoolField(TEXT("niagaraForcedDeterminism"), ActiveJob.bControlNiagara && ActiveJob.bForceNiagaraDeterminism);
	Contract->SetStringField(TEXT("exposureControl"), ActiveJob.bLockExposure ? TEXT("eye_adaptation_disabled") : TEXT("scene_authored_dynamic"));
	Contract->SetStringField(TEXT("renderScheduling"), ActiveJob.bForceSynchronousRendering ? TEXT("offline_synchronous_profile") : TEXT("project_default"));
	Contract->SetStringField(TEXT("depthEncoding"), TEXT("scene_capture_linear_distance_cm"));
	Contract->SetStringField(TEXT("depthContainer"), TEXT("OpenEXR float, R channel"));
	Contract->SetNumberField(TEXT("viewSpaceToMeters"), 0.01);
	Contract->SetStringField(
		TEXT("deviceDepth"),
		ActiveJob.bCaptureTemporalDiagnostics ? TEXT("experimental_raw_reversed_z_from_scene_depth") : TEXT("not_captured"));
	Contract->SetBoolField(TEXT("temporalDiagnosticsEnabled"), ActiveJob.bCaptureTemporalDiagnostics);
	Contract->SetBoolField(TEXT("semanticValidationFixtureEnabled"), ActiveJob.bEnableSemanticValidationFixture);
	Contract->SetStringField(
		TEXT("replayPass"),
		StaticEnum<ESRDatasetReplayPass>()->GetNameStringByValue(static_cast<int64>(ActiveJob.ReplayPass)));
	Contract->SetStringField(
		TEXT("auxiliaryCaptureOrder"),
		StaticEnum<ESRDatasetAuxiliaryCaptureOrder>()->GetNameStringByValue(
			static_cast<int64>(ActiveJob.AuxiliaryCaptureOrder)));
	Contract->SetStringField(
		TEXT("captureOrderInvarianceProtocol"),
		TEXT("separate_process_high_resolution_first_vs_low_resolution_first_normalized_provenance_and_numeric_comparison"));
	Contract->SetBoolField(TEXT("uncapturedMainViewSuppressed"), ActiveJob.bSuppressMainViewOnUncapturedFrames);
	Contract->SetStringField(
		TEXT("endpointPreviousTransformScope"),
		ActiveJob.bUseLastCapturedEndpointTransforms
			? TEXT("scene_component_rigid_transform_only_skeletal_bones_and_wpo_uncertified")
			: TEXT("engine_previous_render_state"));
	if (ActiveJob.bCaptureTemporalDiagnostics)
	{
		Contract->SetStringField(TEXT("motion"), TEXT("experimental_full_current_to_previous_display_pixels"));
		Contract->SetStringField(TEXT("jitterAndMatrices"), TEXT("experimental_captured_from_view_uniforms"));
		Contract->SetStringField(
			TEXT("historyRejection"),
			TEXT("experimental_custom_stencil_identity_else_static_depth_reprojection_with_validity"));
		Contract->SetStringField(TEXT("temporalDiagnosticsStatus"), TEXT("experimental_uncertified"));
		Contract->SetStringField(TEXT("temporalDiagnosticsStage"), TEXT("after_dof_before_temporal_upscaler"));
		Contract->SetStringField(TEXT("nativeHRColor"), TEXT("isolated_hr_scene_capture_after_dof_linear_scene_rgb_pre_exposed"));
		Contract->SetStringField(
			TEXT("referenceHRColor"),
			ActiveJob.bCaptureReferenceHR
				? TEXT("isolated_spatial_supersample_after_dof_downsampled_to_fixed_hr_grid")
				: TEXT("not_captured"));
		Contract->SetStringField(
			TEXT("hudlessDisplayColor"),
			ActiveJob.bCaptureMainViewHUDlessColor
				? TEXT("player_main_view_after_tonemap_before_slate_ui")
				: TEXT("not_captured"));
		Contract->SetStringField(
			TEXT("temporalDiagnosticsView"),
			ActiveJob.bCaptureMainViewTemporalDiagnostics ? TEXT("player_main_view") : TEXT("native_lr_scene_capture"));
	}
	else
	{
		Contract->SetStringField(TEXT("motion"), TEXT("not_captured"));
		Contract->SetStringField(TEXT("jitterAndMatrices"), TEXT("not_captured"));
		Contract->SetStringField(TEXT("historyRejection"), TEXT("not_captured"));
	}
	Contract->SetStringField(TEXT("pairing"), TEXT("All modalities are captured after the same world tick; LR downsampling never advances the world."));
	Contract->SetStringField(
		TEXT("viewStateIsolation"),
		ActiveJob.bCaptureMainViewTemporalDiagnostics
			? TEXT("persistent player Main View history is isolated from one persistent SceneCapture component per reference modality")
			: TEXT("one persistent SceneCapture component per rendered modality"));
	Contract->SetStringField(TEXT("trainingScope"), TEXT("single-frame spatial super-resolution baseline only"));
	Root->SetObjectField(TEXT("determinismContract"), Contract);
	Root->SetArrayField(TEXT("frames"), ManifestFrames);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Could not serialize manifest JSON.");
		return false;
	}

	const FString ManifestPath = FPaths::Combine(ResolvedOutputDirectory, TEXT("manifest.json"));
	const FString TempPath = ManifestPath + TEXT(".part");
	if (!FFileHelper::SaveStringToFile(Json, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!IFileManager::Get().Move(*ManifestPath, *TempPath, true, true, false, true))
	{
		OutError = FString::Printf(TEXT("Could not write manifest: %s"), *ManifestPath);
		return false;
	}
	return true;
}

void USRDatasetCaptureSubsystem::CancelCapture()
{
	if (SRDataset::Private::IsRunningState(Status.State))
	{
		FinishCapture(ESRDatasetCaptureState::Cancelled, TEXT("Capture cancelled by caller."));
	}
}

void USRDatasetCaptureSubsystem::FinishCapture(const ESRDatasetCaptureState FinalState, const FString& Error)
{
	const bool bWasRunning = SRDataset::Private::IsRunningState(Status.State);
	Status.State = FinalState;
	Status.LastError = Error;

	if (ActiveJob.bCaptureTemporalDiagnostics)
	{
		if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> ViewExtension = GetSRDatasetViewExtension())
		{
			ViewExtension->CancelCapture();
		}
	}
	if (ActiveJob.bCaptureMainViewHUDlessColor)
	{
		if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> TonemapExtension =
			GetSRDatasetTonemapViewExtension())
		{
			TonemapExtension->CancelCapture();
		}
	}
	bMainViewCapturePending = false;
	PendingMainViewFrameNumber = INDEX_NONE;
	PendingMainViewTimeSeconds = 0.0;
	PendingMainViewHashes.Reset();
	PendingMainViewRenderSubmissions.Reset();

	if (bWasRunning)
	{
		if (SequencePlayer)
		{
			SequencePlayer->Stop();
			SequencePlayer->RestoreState();
		}
		NotifyControllablesRestore();
		RestoreNiagara();
		RestoreDeterministicRuntimeState();
	}
	RestoreSemanticValidationFixture();

	FString ManifestError;
	if (!ResolvedOutputDirectory.IsEmpty() && IFileManager::Get().DirectoryExists(*ResolvedOutputDirectory) && !WriteManifest(ManifestError))
	{
		UE_LOG(LogSRDataset, Error, TEXT("%s"), *ManifestError);
	}

	if (CaptureRig)
	{
		CaptureRig->Destroy();
		CaptureRig = nullptr;
	}
	if (ValidationFixture)
	{
		ValidationFixture->Destroy();
		ValidationFixture = nullptr;
	}
	if (SequenceActor)
	{
		SequenceActor->Destroy();
	}
	SequencePlayer = nullptr;
	SequenceActor = nullptr;

	if (FinalState == ESRDatasetCaptureState::Completed)
	{
		UE_LOG(LogSRDataset, Display, TEXT("Dataset job '%s' completed: %d captured, %d resumed."),
			*ActiveJob.JobName, Status.CapturedSamples, Status.SkippedSamples);
	}
	else
	{
		UE_LOG(LogSRDataset, Error, TEXT("Dataset job '%s' ended as %s: %s"), *ActiveJob.JobName,
			*StaticEnum<ESRDatasetCaptureState>()->GetNameStringByValue(static_cast<int64>(FinalState)), *Error);
	}

	// Never terminate an interactive editor session. Auto-quit is intended for
	// unattended workers launched by RunDatasetCapture.ps1/UnrealEditor-Cmd.
	if ((ActiveJob.bAutoQuit || bCommandLineAutoQuit) && FApp::IsUnattended())
	{
		const uint8 ExitCode = FinalState == ESRDatasetCaptureState::Completed ? 0 : 1;
		FPlatformMisc::RequestExitWithStatus(false, ExitCode, TEXT("SRDataset job finished"));
	}
}

FString USRDatasetCaptureSubsystem::MakeFramePath(const TCHAR* Modality, const int32 FrameNumber, const TCHAR* Extension) const
{
	return FPaths::Combine(ResolvedOutputDirectory, Modality, FString::Printf(TEXT("frame_%06d.%s"), FrameNumber, Extension));
}

FString USRDatasetCaptureSubsystem::ResolveOutputDirectory(const FString& InPath) const
{
	FString FullPath = FPaths::IsRelative(InPath)
		? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), InPath)
		: FPaths::ConvertRelativePathToFull(InPath);
	FPaths::CollapseRelativeDirectories(FullPath);
	return FullPath;
}

FString USRDatasetCaptureSubsystem::HashFile(const FString& Filename)
{
	TArray64<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *Filename))
	{
		return FString();
	}
	return FSHA1::HashBuffer(Data.GetData(), Data.Num()).ToString();
}

FString USRDatasetCaptureSubsystem::HashString(const FString& Value)
{
	const FTCHARToUTF8 Utf8(*Value);
	return FSHA1::HashBuffer(Utf8.Get(), Utf8.Length()).ToString();
}
