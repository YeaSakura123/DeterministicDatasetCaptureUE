#include "SRDatasetCaptureSubsystem.h"

#include "SRDatasetCaptureRig.h"
#include "SRDatasetControllable.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "JsonObjectConverter.h"
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
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogSRDataset, Log, All);

namespace SRDataset::Private
{
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

	for (const TCHAR* Directory : { TEXT("hr"), TEXT("lr"), TEXT("depth") })
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

	ApplyDeterministicRuntimeState();
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

void USRDatasetCaptureSubsystem::ApplyDeterministicRuntimeState()
{
	bPreviousUseFixedTimeStep = FApp::UseFixedTimeStep();
	PreviousFixedDeltaTime = FApp::GetFixedDeltaTime();
	FApp::SetFixedDeltaTime(ActiveJob.GetFixedDeltaSeconds());
	FApp::SetUseFixedTimeStep(true);
	FMath::RandInit(ActiveJob.RandomSeed);
	FMath::SRandInit(ActiveJob.RandomSeed);

	if (ActiveJob.bEnableChaosDeterminism)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.Deterministic")))
		{
			PreviousChaosDeterminism = Variable->GetInt();
			Variable->Set(1, ECVF_SetByCode);
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
}

void USRDatasetCaptureSubsystem::HandleWorldPreActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World != GetWorld() || !SRDataset::Private::IsRunningState(Status.State))
	{
		return;
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
		--WarmupFramesRemaining;
		if (WarmupFramesRemaining <= 0)
		{
			Status.State = ESRDatasetCaptureState::Capturing;
			Status.CurrentFrame = ActiveJob.StartFrame;
		}
		return;
	}

	if ((Status.CurrentFrame - ActiveJob.StartFrame) % ActiveJob.FrameStep == 0)
	{
		FString Error;
		if (!CaptureCurrentFrame(Error))
		{
			FinishCapture(ESRDatasetCaptureState::Failed, Error);
			return;
		}
	}

	++Status.CurrentFrame;
	if (Status.CurrentFrame > ActiveJob.EndFrame)
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

	CaptureRig->ApplyCameraView(View, ActiveJob.bDisableMotionBlur);
	return true;
}

bool USRDatasetCaptureSubsystem::CaptureCurrentFrame(FString& OutError)
{
	if (!UpdateCaptureCamera(OutError))
	{
		return false;
	}

	const int32 FrameNumber = Status.CurrentFrame;
	const double TimeSeconds = FrameNumber * ActiveJob.GetFixedDeltaSeconds();
	TMap<FString, FString> Hashes;
	if (ActiveJob.bResume && IsFrameAlreadyComplete(FrameNumber))
	{
		Hashes.Add(TEXT("hr"), HashFile(MakeFramePath(TEXT("hr"), FrameNumber, TEXT("png"))));
		Hashes.Add(TEXT("lr"), HashFile(MakeFramePath(TEXT("lr"), FrameNumber, TEXT("png"))));
		if (ActiveJob.bCaptureDepth)
		{
			Hashes.Add(TEXT("depth"), HashFile(MakeFramePath(TEXT("depth"), FrameNumber, TEXT("exr"))));
		}
		++Status.SkippedSamples;
		AppendFrameManifest(FrameNumber, TimeSeconds, Hashes, true);
	}
	else
	{
		if (!CaptureRig->CaptureFrame(
			ActiveJob,
			MakeFramePath(TEXT("hr"), FrameNumber, TEXT("png")),
			MakeFramePath(TEXT("lr"), FrameNumber, TEXT("png")),
			MakeFramePath(TEXT("depth"), FrameNumber, TEXT("exr")),
			Hashes,
			OutError))
		{
			return false;
		}
		++Status.CapturedSamples;
		AppendFrameManifest(FrameNumber, TimeSeconds, Hashes, false);
	}

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
	return !ActiveJob.bCaptureDepth || IFileManager::Get().FileExists(*MakeFramePath(TEXT("depth"), FrameNumber, TEXT("exr")));
}

void USRDatasetCaptureSubsystem::AppendFrameManifest(
	const int32 FrameNumber,
	const double TimeSeconds,
	const TMap<FString, FString>& Hashes,
	const bool bResumed)
{
	TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetNumberField(TEXT("frame"), FrameNumber);
	Frame->SetNumberField(TEXT("timeSeconds"), TimeSeconds);
	Frame->SetBoolField(TEXT("resumed"), bResumed);

	TSharedRef<FJsonObject> Files = MakeShared<FJsonObject>();
	Files->SetStringField(TEXT("hr"), FString::Printf(TEXT("hr/frame_%06d.png"), FrameNumber));
	Files->SetStringField(TEXT("lr"), FString::Printf(TEXT("lr/frame_%06d.png"), FrameNumber));
	if (ActiveJob.bCaptureDepth)
	{
		Files->SetStringField(TEXT("depth"), FString::Printf(TEXT("depth/frame_%06d.exr"), FrameNumber));
	}
	Frame->SetObjectField(TEXT("files"), Files);

	TSharedRef<FJsonObject> HashObject = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Hashes)
	{
		HashObject->SetStringField(Pair.Key, Pair.Value);
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
	Camera->SetNumberField(TEXT("orthoWidthCm"), CameraView.OrthoWidth);
	Camera->SetNumberField(TEXT("aspectRatio"), static_cast<double>(ActiveJob.HRResolution.X) / ActiveJob.HRResolution.Y);
	if (CameraView.ProjectionMode == ECameraProjectionMode::Perspective)
	{
		const double FocalLengthPixels = 0.5 * ActiveJob.HRResolution.X / FMath::Tan(FMath::DegreesToRadians(0.5 * CameraView.FOV));
		Camera->SetNumberField(TEXT("focalLengthPixelsX"), FocalLengthPixels);
	}
	Frame->SetObjectField(TEXT("camera"), Camera);

	ManifestFrames.Add(MakeShared<FJsonValueObject>(Frame));
}

bool USRDatasetCaptureSubsystem::WriteManifest(FString& OutError) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 1);
	Root->SetStringField(TEXT("pluginVersion"), TEXT("0.1.0"));
	Root->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Root->SetStringField(TEXT("world"), GetWorld() ? GetWorld()->GetOutermost()->GetName() : FString());
	Root->SetStringField(TEXT("state"), StaticEnum<ESRDatasetCaptureState>()->GetNameStringByValue(static_cast<int64>(Status.State)));
	Root->SetStringField(TEXT("error"), Status.LastError);
	Root->SetNumberField(TEXT("capturedSamples"), Status.CapturedSamples);
	Root->SetNumberField(TEXT("skippedSamples"), Status.SkippedSamples);

	TSharedRef<FJsonObject> JobObject = MakeShared<FJsonObject>();
	FJsonObjectConverter::UStructToJsonObject(FSRDatasetCaptureJob::StaticStruct(), &ActiveJob, JobObject, 0, 0);
	Root->SetObjectField(TEXT("job"), JobObject);

	TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
	Contract->SetNumberField(TEXT("fixedDeltaSeconds"), ActiveJob.GetFixedDeltaSeconds());
	Contract->SetBoolField(TEXT("fixedEngineTimeStep"), true);
	Contract->SetBoolField(TEXT("chaosDeterminism"), ActiveJob.bEnableChaosDeterminism);
	Contract->SetBoolField(TEXT("niagaraAbsoluteAge"), ActiveJob.bControlNiagara);
	Contract->SetBoolField(TEXT("niagaraForcedDeterminism"), ActiveJob.bControlNiagara && ActiveJob.bForceNiagaraDeterminism);
	Contract->SetStringField(TEXT("depthEncoding"), TEXT("OpenEXR float, SceneDepth in Unreal centimeters, R channel"));
	Contract->SetStringField(TEXT("pairing"), TEXT("All modalities are captured after the same world tick; LR downsampling never advances the world."));
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

	if (ActiveJob.bAutoQuit || bCommandLineAutoQuit)
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
