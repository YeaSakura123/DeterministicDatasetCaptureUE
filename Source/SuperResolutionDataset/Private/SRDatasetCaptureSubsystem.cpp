#include "SRDatasetCaptureSubsystem.h"

#include "SRDatasetCaptureRig.h"
#include "SRDatasetControllable.h"
#include "SRDatasetValidationFixture.h"
#include "SRDatasetViewExtension.h"

#include "Camera/CameraComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "ContentStreaming.h"
#include "Dom/JsonObject.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Animation/AnimInstance.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
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
#include "Materials/Material.h"
#include "Materials/MaterialExpressionParticleRandom.h"
#include "Materials/MaterialExpressionPerInstanceRandom.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemImpl.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "Particles/ParticleSystemComponent.h"
#include "Kismet/GameplayStatics.h"
#include "RenderCore.h"
#include "Rendering/MotionVectorSimulation.h"
#include "TextureResource.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/MemoryReader.h"
#include "Styling/CoreStyle.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"

#include <cmath>
#include <limits>

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
		TEXT("history_rejection_reason"),
		TEXT("disocclusion_mask"),
		TEXT("disocclusion_valid"),
		TEXT("disocclusion_reason"),
		TEXT("translucency_after_dof_raw"),
		TEXT("transparency_mask"),
		TEXT("reactive_mask"),
		TEXT("object_id"),
		TEXT("normal_world"),
		TEXT("base_color_linear"),
		TEXT("material_properties"),
		TEXT("gbuffer_valid")
	};
	constexpr const TCHAR* SceneCaptureLRComparisonModality = TEXT("color_lr_scene_capture_hdr");
	constexpr const TCHAR* ReferenceHRModality = TEXT("color_hr_reference_scene_hdr");
	constexpr const TCHAR* HUDlessColorModality = TEXT("color_main_view_hudless_after_tonemap");
	constexpr const TCHAR* UIColorAlphaModality = TEXT("ui_color_alpha");

	class SValidationUI final : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SValidationUI) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&)
		{
			SetVisibility(EVisibility::HitTestInvisible);
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D(256.0f, 144.0f);
		}

		virtual int32 OnPaint(
			const FPaintArgs& Args,
			const FGeometry& AllottedGeometry,
			const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements,
			int32 LayerId,
			const FWidgetStyle& InWidgetStyle,
			bool bParentEnabled) const override
		{
			const FVector2D Size = AllottedGeometry.GetLocalSize();
			const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
			const auto DrawNormalizedBox = [&](const FVector2D Min, const FVector2D Extent, const FLinearColor Color)
			{
				FSlateDrawElement::MakeBox(
					OutDrawElements,
					LayerId,
					AllottedGeometry.ToPaintGeometry(Extent * Size, FSlateLayoutTransform(Min * Size)),
					WhiteBrush,
					ESlateDrawEffect::None,
					Color);
			};
			// Known straight-alpha probes: opaque red, 50% green and 75% blue.
			// The Slate target stores premultiplied RGB plus coverage alpha.
			DrawNormalizedBox(FVector2D(0.05, 0.08), FVector2D(0.20, 0.14), FLinearColor(1.0, 0.0, 0.0, 1.0));
			DrawNormalizedBox(FVector2D(0.32, 0.08), FVector2D(0.20, 0.14), FLinearColor(0.0, 1.0, 0.0, 0.5));
			DrawNormalizedBox(FVector2D(0.59, 0.08), FVector2D(0.20, 0.14), FLinearColor(0.0, 0.0, 1.0, 0.75));
			return LayerId + 1;
		}
	};

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
	WorldTickEndHandle = FWorldDelegates::OnWorldTickEnd.AddUObject(this, &ThisClass::HandleWorldTickEnd);
}

void USRDatasetCaptureSubsystem::Deinitialize()
{
	if (SRDataset::Private::IsRunningState(Status.State))
	{
		FinishCapture(ESRDatasetCaptureState::Cancelled, TEXT("World subsystem deinitialized."));
	}
	FWorldDelegates::OnWorldPreActorTick.Remove(PreActorTickHandle);
	FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
	FWorldDelegates::OnWorldTickEnd.Remove(WorldTickEndHandle);
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
	// Project-authored validation Actors and their AnimInstances can consume the
	// global random streams during SpawnActor/BeginPlay, before the normal runtime
	// determinism profile is installed. Seed before creating any capture Actors.
	FMath::RandInit(ActiveJob.RandomSeed);
	FMath::SRandInit(ActiveJob.RandomSeed);
	Status = FSRDatasetCaptureStatus();
	Status.CurrentFrame = GetInitialEvaluationFrame();
	ResolvedOutputDirectory = ResolveOutputDirectory(Job.OutputDirectory);
	Status.OutputDirectory = ResolvedOutputDirectory;
	ManifestFrames.Reset();
	NextRenderSubmissionId = 0;
	bMainViewCapturePending = false;
	PendingMainViewFrameNumber = INDEX_NONE;
	PendingMainViewTimeSeconds = 0.0;
	PendingMainViewHashes.Reset();
	PendingMainViewRenderSubmissions.Reset();
	SceneControlPreflight = FSceneControlPreflightReport();
	bStreamingBarrierComplete = false;
	StreamingRequestsAfterBarrier = INDEX_NONE;
	StreamingTextureCountAfterBarrier = 0;
	PendingStreamingTextureCountAfterBarrier = 0;
	StreamingStateAfterBarrierSha1.Reset();
	CachedSkeletalPoseFrames.Reset();
	NonFixtureSkeletalObjectIds.Reset();
	NonFixtureSkeletalStencilStates.Reset();
	NonFixtureHiddenActorStates.Reset();
	StableInstanceStencilStates.Reset();
	StableInstanceIdRecords.Reset();
	StableInstanceIdMappingSha1.Reset();
	bStableInstanceIdsPrepared = false;
	WarmupPoseCacheFrame = INDEX_NONE;
	AppliedCachedSkeletalPoseComponentCount = 0;
	AppliedCachedSkeletalPoseBoneCount = 0;
	SkippedCachedSkeletalPoseComponents.Reset();
	SkeletalPoseCacheArtifactSha1.Reset();
	bSkeletalPoseCacheLoadedFromArtifact = false;
	DeterministicCameraPlayerController.Reset();
	PreviousPlayerViewTarget.Reset();

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
	if (ActiveJob.bCaptureTemporalDiagnostics)
	{
		const IConsoleVariable* ForwardShading =
			IConsoleManager::Get().FindConsoleVariable(TEXT("r.ForwardShading"));
		if (ForwardShading && ForwardShading->GetInt() != 0)
		{
			OutError = TEXT("Temporal GBuffer diagnostics require deferred shading (r.ForwardShading=0).");
			return false;
		}
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
	if (ActiveJob.bCaptureSceneCaptureLRComparison)
	{
		OutputDirectories.Add(SRDataset::Private::SceneCaptureLRComparisonModality);
	}
	if (ActiveJob.bCaptureReferenceHR)
	{
		OutputDirectories.Add(SRDataset::Private::ReferenceHRModality);
	}
	if (ActiveJob.bCaptureMainViewHUDlessColor)
	{
		OutputDirectories.Add(SRDataset::Private::HUDlessColorModality);
	}
	if (ActiveJob.bCaptureUIColorAlpha)
	{
		OutputDirectories.Add(SRDataset::Private::UIColorAlphaModality);
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
	if (!PrepareDeterministicCamera(OutError))
	{
		return false;
	}

	if (!PrepareSemanticValidationFixture(OutError))
	{
		return false;
	}
	if (!PrepareNonFixtureSkeletalValidation(OutError))
	{
		return false;
	}
	if (!PrepareProjectAnimatedMaterialValidation(OutError))
	{
		return false;
	}
	if (!CheckWidgetComponentPolicy(OutError))
	{
		return false;
	}
	if (!LoadSkeletalPoseCacheArtifact(OutError))
	{
		return false;
	}

	ApplyDeterministicRuntimeState();
	if (ActiveJob.bLockTemporalJitterToLogicalFrame && !bOverrodeTemporalJitterIndex)
	{
		OutError = TEXT("Logical-frame jitter locking requires r.TemporalAA.Debug.OverrideTemporalIndex in a non-shipping capture build.");
		return false;
	}
	SnapshotProvenance();
	NotifyControllablesPrepare();
	// Install Niagara's deterministic/solo controls before the preflight so the
	// report can inspect runtime instance parameters without touching a shared
	// simulation. Age zero preserves the progressive warmup ramp.
	DiscoverAndControlNiagara(0.0f);
	if (!RunSceneControlPreflight(OutError))
	{
		return false;
	}
	WarmupFramesRemaining = ActiveJob.WarmupFrames;
	Status.State = WarmupFramesRemaining > 0 ? ESRDatasetCaptureState::WarmingUp : ESRDatasetCaptureState::Capturing;
	Status.CurrentFrame = GetInitialEvaluationFrame();
	if (!UpdateCaptureCamera(true, OutError))
	{
		return false;
	}

	if (!WriteManifest(OutError))
	{
		return false;
	}
	return true;
}

bool USRDatasetCaptureSubsystem::IsReverseEndpointReplay() const
{
	return ActiveJob.ReplayPass == ESRDatasetReplayPass::FrameGenerationReverseEndpoints;
}

int32 USRDatasetCaptureSubsystem::GetEvaluationDirection() const
{
	return IsReverseEndpointReplay() ? -1 : 1;
}

int32 USRDatasetCaptureSubsystem::GetInitialEvaluationFrame() const
{
	return IsReverseEndpointReplay() ? ActiveJob.EndFrame : ActiveJob.StartFrame;
}

int32 USRDatasetCaptureSubsystem::GetFirstCapturedFrame() const
{
	if (!IsReverseEndpointReplay())
	{
		return ActiveJob.StartFrame + ActiveJob.CaptureFrameOffset;
	}
	const int32 FirstPhaseFrame = ActiveJob.StartFrame + ActiveJob.CaptureFrameOffset;
	return ActiveJob.EndFrame - (ActiveJob.EndFrame - FirstPhaseFrame) % ActiveJob.FrameStep;
}

int32 USRDatasetCaptureSubsystem::GetPreviouslyCapturedFrame(const int32 FrameNumber) const
{
	return FrameNumber - GetEvaluationDirection() * ActiveJob.FrameStep;
}

int32 USRDatasetCaptureSubsystem::GetTemporalJitterOverrideIndex(const int32 FrameNumber) const
{
	const int32 SequenceLength = FMath::Max(1, ActiveJob.TemporalJitterSequenceLength);
	const int32 Unnormalized = FrameNumber + ActiveJob.TemporalJitterPhaseOffset;
	return ((Unnormalized % SequenceLength) + SequenceLength) % SequenceLength;
}

uint32 USRDatasetCaptureSubsystem::GetLogicalViewStateFrameIndex(const int32 FrameNumber) const
{
	// Unsigned addition defines negative logical frames modulo 2^32 without
	// signed overflow. Mod8 remains the signed logical frame plus phase, while
	// every replay process receives the same full renderer state index.
	return static_cast<uint32>(FrameNumber) +
		static_cast<uint32>(ActiveJob.TemporalJitterPhaseOffset);
}

bool USRDatasetCaptureSubsystem::IsPastEvaluationRange(const int32 FrameNumber) const
{
	return IsReverseEndpointReplay()
		? FrameNumber < ActiveJob.StartFrame
		: FrameNumber > ActiveJob.EndFrame;
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
	if (ActiveJob.bCaptureUIColorAlpha)
	{
		UGameViewportClient* ViewportClient = World->GetGameViewport();
		if (!ViewportClient)
		{
			OutError = TEXT("The semantic UI validation fixture requires a game viewport.");
			return false;
		}
		ValidationUIWidget = SNew(SRDataset::Private::SValidationUI);
		ViewportClient->AddViewportWidgetContent(ValidationUIWidget.ToSharedRef(), 1000000);
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
	if (ValidationUIWidget)
	{
		if (UWorld* World = GetWorld())
		{
			if (UGameViewportClient* ViewportClient = World->GetGameViewport())
			{
				ViewportClient->RemoveViewportWidgetContent(ValidationUIWidget.ToSharedRef());
			}
		}
		ValidationUIWidget.Reset();
	}
	for (const TPair<TWeakObjectPtr<AActor>, bool>& Pair : ValidationHiddenActorStates)
	{
		if (AActor* Actor = Pair.Key.Get())
		{
			Actor->SetActorHiddenInGame(Pair.Value);
		}
	}
	ValidationHiddenActorStates.Reset();
}

bool USRDatasetCaptureSubsystem::PrepareNonFixtureSkeletalValidation(FString& OutError)
{
	NonFixtureSkeletalObjectIds.Reset();
	NonFixtureSkeletalStencilStates.Reset();
	if (!ActiveJob.bValidateNonFixtureSkeletalAnimation)
	{
		return true;
	}

	UWorld* World = GetWorld();
	UClass* ProbeClass = ActiveJob.NonFixtureSkeletalValidationActorClass.TryLoadClass<AActor>();
	if (!World || !ProbeClass)
	{
		OutError = FString::Printf(
			TEXT("Could not load non-fixture skeletal validation Actor class: %s"),
			*ActiveJob.NonFixtureSkeletalValidationActorClass.ToString());
		return false;
	}
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(
		World,
		ProbeClass,
		TEXT("SRDatasetProjectSkeletalProbe"));
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.ObjectFlags |= RF_Transient;
	NonFixtureSkeletalValidationActor = World->SpawnActor<AActor>(
		ProbeClass,
		FTransform::Identity,
		SpawnParameters);
	if (!NonFixtureSkeletalValidationActor)
	{
		OutError = FString::Printf(
			TEXT("Could not spawn non-fixture skeletal validation Actor: %s"),
			*ActiveJob.NonFixtureSkeletalValidationActorClass.ToString());
		return false;
	}
	NonFixtureSkeletalValidationActor->SetActorEnableCollision(false);
	NonFixtureSkeletalValidationActor->Tags.AddUnique(TEXT("SRDatasetProjectAssetProbe"));
	if (ACharacter* Character = Cast<ACharacter>(NonFixtureSkeletalValidationActor))
	{
		if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			Movement->DisableMovement();
		}
	}
	TInlineComponentArray<UPrimitiveComponent*> ProbePrimitives;
	NonFixtureSkeletalValidationActor->GetComponents(ProbePrimitives);
	for (UPrimitiveComponent* Primitive : ProbePrimitives)
	{
		if (!IsValid(Primitive))
		{
			continue;
		}
		Primitive->SetHiddenInGame(false, true);
		Primitive->SetVisibility(true, true);
		Primitive->SetOnlyOwnerSee(false);
		Primitive->SetOwnerNoSee(false);
	}
	PositionNonFixtureSkeletalValidationActor();
	if (APlayerController* Controller = World->GetFirstPlayerController())
	{
		if (APawn* OriginalPlayerPawn = Controller->GetPawn();
			OriginalPlayerPawn && OriginalPlayerPawn != NonFixtureSkeletalValidationActor)
		{
			NonFixtureHiddenActorStates.Add(
				OriginalPlayerPawn,
				OriginalPlayerPawn->IsHidden());
			OriginalPlayerPawn->SetActorHiddenInGame(true);
		}
	}

	int32 NextObjectId = 128;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || Actor == CaptureRig || Actor == ValidationFixture ||
			NonFixtureHiddenActorStates.Contains(Actor))
		{
			continue;
		}
		TInlineComponentArray<USkinnedMeshComponent*> Components;
		Actor->GetComponents(Components);
		for (USkinnedMeshComponent* Component : Components)
		{
			if (!IsValid(Component) || !Component->IsRegistered() || !Component->GetSkinnedAsset())
			{
				continue;
			}
			if (NextObjectId > 254)
			{
				OutError = TEXT("Non-fixture skeletal validation supports at most 127 labeled components per job.");
				RestoreNonFixtureSkeletalValidation();
				return false;
			}
			FPrimitiveStencilState State;
			State.bRenderCustomDepth = Component->bRenderCustomDepth;
			State.CustomDepthStencilValue = static_cast<uint8>(Component->CustomDepthStencilValue);
			State.CustomDepthStencilWriteMask = static_cast<uint8>(Component->CustomDepthStencilWriteMask);
			NonFixtureSkeletalStencilStates.Add(Component, State);
			NonFixtureSkeletalObjectIds.Add(Component, NextObjectId);
			Component->SetRenderCustomDepth(true);
			Component->SetCustomDepthStencilWriteMask(ERendererStencilMask::ERSM_Default);
			Component->SetCustomDepthStencilValue(NextObjectId++);
		}
	}
	if (NonFixtureSkeletalObjectIds.IsEmpty())
	{
		OutError = TEXT("Non-fixture skeletal validation found no registered project skeletal components.");
		return false;
	}
	return true;
}

void USRDatasetCaptureSubsystem::RestoreNonFixtureSkeletalValidation()
{
	for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, FPrimitiveStencilState>& Pair :
		NonFixtureSkeletalStencilStates)
	{
		if (UPrimitiveComponent* Component = Pair.Key.Get())
		{
			Component->SetRenderCustomDepth(Pair.Value.bRenderCustomDepth);
			Component->SetCustomDepthStencilWriteMask(
				static_cast<ERendererStencilMask>(Pair.Value.CustomDepthStencilWriteMask));
			Component->SetCustomDepthStencilValue(Pair.Value.CustomDepthStencilValue);
		}
	}
	NonFixtureSkeletalStencilStates.Reset();
	NonFixtureSkeletalObjectIds.Reset();
	for (const TPair<TWeakObjectPtr<AActor>, bool>& Pair : NonFixtureHiddenActorStates)
	{
		if (AActor* Actor = Pair.Key.Get())
		{
			Actor->SetActorHiddenInGame(Pair.Value);
		}
	}
	NonFixtureHiddenActorStates.Reset();
}

void USRDatasetCaptureSubsystem::PositionNonFixtureSkeletalValidationActor()
{
	if (!NonFixtureSkeletalValidationActor || !CaptureRig)
	{
		return;
	}
	const FMinimalViewInfo& View = CaptureRig->GetLastCameraView();
	const FVector Forward = View.Rotation.Vector();
	const FVector Up = FRotationMatrix(View.Rotation).GetScaledAxis(EAxis::Z);
	const FVector Location = View.Location + Forward * 500.0f - Up * 90.0f;
	const FRotator Rotation(0.0f, View.Rotation.Yaw + 180.0f, 0.0f);
	NonFixtureSkeletalValidationActor->SetActorLocationAndRotation(
		Location,
		Rotation,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
}

bool USRDatasetCaptureSubsystem::PrepareProjectAnimatedMaterialValidation(FString& OutError)
{
	ProjectAnimatedMaterialValidationBasePath.Reset();
	if (!ActiveJob.bValidateProjectAnimatedMaterial)
	{
		return true;
	}

	UWorld* World = GetWorld();
	ProjectAnimatedMaterialValidationInterface = Cast<UMaterialInterface>(
		ActiveJob.ProjectAnimatedMaterialValidationMaterial.TryLoad());
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!World || !ProjectAnimatedMaterialValidationInterface || !CubeMesh)
	{
		OutError = FString::Printf(
			TEXT("Could not load project animated-material validation assets: %s"),
			*ActiveJob.ProjectAnimatedMaterialValidationMaterial.ToString());
		return false;
	}
	if (const UMaterial* BaseMaterial = ProjectAnimatedMaterialValidationInterface->GetMaterial())
	{
		ProjectAnimatedMaterialValidationBasePath = BaseMaterial->GetPathName();
	}
	if (!ProjectAnimatedMaterialValidationBasePath.StartsWith(TEXT("/Game/")))
	{
		OutError = TEXT("Project animated-material validation resolved to a non-project base material.");
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(
		World,
		AStaticMeshActor::StaticClass(),
		TEXT("SRDatasetProjectAnimatedMaterialReceiver"));
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.ObjectFlags |= RF_Transient;
	AStaticMeshActor* Receiver = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(),
		FTransform::Identity,
		SpawnParameters);
	if (!Receiver)
	{
		OutError = TEXT("Could not spawn the project animated-material validation receiver.");
		return false;
	}
	ProjectAnimatedMaterialValidationReceiver = Receiver;
	ProjectAnimatedMaterialValidationComponent = Receiver->GetStaticMeshComponent();
	if (!ProjectAnimatedMaterialValidationComponent)
	{
		OutError = TEXT("The project animated-material validation receiver has no static-mesh component.");
		RestoreProjectAnimatedMaterialValidation();
		return false;
	}
	Receiver->Tags.AddUnique(TEXT("SRDatasetProjectAnimatedMaterialProbe"));
	Receiver->SetActorEnableCollision(false);
	Receiver->SetActorTickEnabled(false);
	ProjectAnimatedMaterialValidationComponent->SetMobility(EComponentMobility::Movable);
	ProjectAnimatedMaterialValidationComponent->SetStaticMesh(CubeMesh);
	ProjectAnimatedMaterialValidationComponent->SetMaterial(
		0,
		ProjectAnimatedMaterialValidationInterface);
	// The project flashlight material reads Custom Primitive Data supplied by
	// its Blueprint carrier. Populate a deterministic neutral/white payload on
	// this isolated receiver so the material network is visible without running
	// the Blueprint's stateful random Tick graph.
	for (int32 DataIndex = 0; DataIndex < 8; ++DataIndex)
	{
		ProjectAnimatedMaterialValidationComponent->SetCustomPrimitiveDataFloat(
			DataIndex,
			1.0f);
	}
	ProjectAnimatedMaterialValidationComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProjectAnimatedMaterialValidationComponent->SetCastShadow(false);
	ProjectAnimatedMaterialValidationComponent->SetHiddenInGame(false, true);
	ProjectAnimatedMaterialValidationComponent->SetVisibility(true, true);
	ProjectAnimatedMaterialValidationComponent->SetRenderCustomDepth(true);
	ProjectAnimatedMaterialValidationComponent->SetCustomDepthStencilWriteMask(
		ERendererStencilMask::ERSM_Default);
	ProjectAnimatedMaterialValidationComponent->SetCustomDepthStencilValue(150);
	ProjectAnimatedMaterialValidationComponent->bEvaluateWorldPositionOffset = true;
	ProjectAnimatedMaterialValidationComponent->bWorldPositionOffsetWritesVelocity = true;
	PositionProjectAnimatedMaterialValidationReceiver();
	return true;
}

void USRDatasetCaptureSubsystem::PositionProjectAnimatedMaterialValidationReceiver()
{
	if (!ProjectAnimatedMaterialValidationReceiver || !CaptureRig)
	{
		return;
	}
	const FMinimalViewInfo& View = CaptureRig->GetLastCameraView();
	const FVector Forward = View.Rotation.Vector();
	const FVector Up = FRotationMatrix(View.Rotation).GetScaledAxis(EAxis::Z);
	ProjectAnimatedMaterialValidationReceiver->SetActorLocationAndRotation(
		View.Location + Forward * 500.0f + Up * 15.0f,
		View.Rotation,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	ProjectAnimatedMaterialValidationReceiver->SetActorScale3D(FVector(0.15f, 2.5f, 1.5f));
}

void USRDatasetCaptureSubsystem::RestoreProjectAnimatedMaterialValidation()
{
	ProjectAnimatedMaterialValidationComponent = nullptr;
	ProjectAnimatedMaterialValidationInterface = nullptr;
	ProjectAnimatedMaterialValidationBasePath.Reset();
	if (ProjectAnimatedMaterialValidationReceiver)
	{
		ProjectAnimatedMaterialValidationReceiver->Destroy();
		ProjectAnimatedMaterialValidationReceiver = nullptr;
	}
}

TArray<UPrimitiveComponent*> USRDatasetCaptureSubsystem::CollectStableInstanceComponents() const
{
	TArray<UPrimitiveComponent*> Components;
	UWorld* World = GetWorld();
	if (!World)
	{
		return Components;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || Actor == CaptureRig || Actor == ValidationFixture ||
			Actor == ProjectAnimatedMaterialValidationReceiver)
		{
			continue;
		}
		TInlineComponentArray<UPrimitiveComponent*> ActorComponents;
		Actor->GetComponents(ActorComponents);
		for (UPrimitiveComponent* Component : ActorComponents)
		{
			if (IsValid(Component) && Component->IsRegistered() &&
				Component->CanEverRender() && !Component->IsVisualizationComponent())
			{
				Components.Add(Component);
			}
		}
	}
	Components.Sort([](const UPrimitiveComponent& Left, const UPrimitiveComponent& Right)
	{
		return Left.GetPathName().Compare(Right.GetPathName(), ESearchCase::CaseSensitive) < 0;
	});
	return Components;
}

bool USRDatasetCaptureSubsystem::PrepareStableInstanceIds(FString& OutError)
{
	StableInstanceStencilStates.Reset();
	StableInstanceIdRecords.Reset();
	StableInstanceIdMappingSha1.Reset();
	bStableInstanceIdsPrepared = false;
	if (!ActiveJob.bAssignStableInstanceIds)
	{
		return true;
	}

	const TArray<UPrimitiveComponent*> Components = CollectStableInstanceComponents();
	if (Components.IsEmpty())
	{
		OutError = TEXT("Stable instance-ID assignment found no registered renderable primitive components.");
		return false;
	}
	if (Components.Num() > 255)
	{
		OutError = FString::Printf(
			TEXT("Stable instance-ID v1 supports at most 255 components, but the fixed topology contains %d. Use a smaller loaded scene or a future wider encoding."),
			Components.Num());
		return false;
	}

	FString Canonical;
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		UPrimitiveComponent* Component = Components[Index];
		AActor* Owner = Component->GetOwner();
		const int32 InstanceId = Index + 1;
		FPrimitiveStencilState State;
		State.bRenderCustomDepth = Component->bRenderCustomDepth;
		State.CustomDepthStencilValue = static_cast<uint8>(Component->CustomDepthStencilValue);
		State.CustomDepthStencilWriteMask = static_cast<uint8>(Component->CustomDepthStencilWriteMask);
		StableInstanceStencilStates.Add(Component, State);

		FStableInstanceIdRecord Record;
		Record.InstanceId = InstanceId;
		Record.ComponentPath = Component->GetPathName();
		Record.ActorPath = Owner ? Owner->GetPathName() : TEXT("none");
		Record.ActorClassPath = Owner ? Owner->GetClass()->GetPathName() : TEXT("none");
		Record.ComponentClassPath = Component->GetClass()->GetPathName();
		for (const FString* Value : {
			&Record.ComponentPath, &Record.ActorPath, &Record.ActorClassPath, &Record.ComponentClassPath })
		{
			if (Value->Contains(TEXT("\t")) || Value->Contains(TEXT("\n")) || Value->Contains(TEXT("\r")))
			{
				OutError = FString::Printf(TEXT("Stable instance-ID path contains a forbidden tab/newline: %s"), **Value);
				RestoreStableInstanceIds();
				return false;
			}
		}
		StableInstanceIdRecords.Add(Record);
		Canonical += FString::Printf(
			TEXT("%d\t%s\t%s\t%s\t%s\n"),
			Record.InstanceId,
			*Record.ComponentPath,
			*Record.ActorPath,
			*Record.ActorClassPath,
			*Record.ComponentClassPath);

		Component->SetRenderCustomDepth(true);
		Component->SetCustomDepthStencilWriteMask(ERendererStencilMask::ERSM_Default);
		Component->SetCustomDepthStencilValue(InstanceId);
	}
	StableInstanceIdMappingSha1 = HashString(Canonical);
	if (!WriteStableInstanceIdMap(OutError))
	{
		RestoreStableInstanceIds();
		return false;
	}
	bStableInstanceIdsPrepared = true;
	return true;
}

bool USRDatasetCaptureSubsystem::ValidateStableInstanceIds(FString& OutError) const
{
	if (!ActiveJob.bAssignStableInstanceIds)
	{
		return true;
	}
	if (!bStableInstanceIdsPrepared)
	{
		OutError = TEXT("Stable instance IDs were not prepared after warmup and the streaming barrier.");
		return false;
	}
	const TArray<UPrimitiveComponent*> Components = CollectStableInstanceComponents();
	if (Components.Num() != StableInstanceIdRecords.Num())
	{
		TSet<FString> PreparedPaths;
		TSet<FString> CurrentPaths;
		for (const FStableInstanceIdRecord& Record : StableInstanceIdRecords)
		{
			PreparedPaths.Add(Record.ComponentPath);
		}
		for (const UPrimitiveComponent* Component : Components)
		{
			CurrentPaths.Add(Component->GetPathName());
		}
		TArray<FString> Added;
		TArray<FString> Removed;
		for (const FString& Path : CurrentPaths)
		{
			if (!PreparedPaths.Contains(Path))
			{
				Added.Add(Path);
			}
		}
		for (const FString& Path : PreparedPaths)
		{
			if (!CurrentPaths.Contains(Path))
			{
				Removed.Add(Path);
			}
		}
		Added.Sort();
		Removed.Sort();
		OutError = FString::Printf(
			TEXT("Stable instance topology changed: prepared=%d current=%d added=[%s] removed=[%s]."),
			StableInstanceIdRecords.Num(),
			Components.Num(),
			*FString::Join(Added, TEXT(",")),
			*FString::Join(Removed, TEXT(",")));
		return false;
	}
	for (int32 Index = 0; Index < Components.Num(); ++Index)
	{
		const UPrimitiveComponent* Component = Components[Index];
		const FStableInstanceIdRecord& Record = StableInstanceIdRecords[Index];
		if (Component->GetPathName() != Record.ComponentPath ||
			!Component->bRenderCustomDepth ||
			Component->CustomDepthStencilValue != Record.InstanceId ||
			Component->CustomDepthStencilWriteMask != ERendererStencilMask::ERSM_Default)
		{
			OutError = FString::Printf(
				TEXT("Stable instance topology/label drift at ID %d: expected=%s current=%s stencil=%d enabled=%s."),
				Record.InstanceId,
				*Record.ComponentPath,
				*Component->GetPathName(),
				Component->CustomDepthStencilValue,
				Component->bRenderCustomDepth ? TEXT("true") : TEXT("false"));
			return false;
		}
	}
	return true;
}

bool USRDatasetCaptureSubsystem::WriteStableInstanceIdMap(FString& OutError) const
{
	if (!ActiveJob.bAssignStableInstanceIds)
	{
		return true;
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 1);
	Root->SetStringField(TEXT("encoding"), TEXT("custom_stencil_uint8"));
	Root->SetNumberField(TEXT("backgroundId"), 0);
	Root->SetNumberField(TEXT("maximumAssignableId"), 255);
	Root->SetBoolField(TEXT("fixedTopologyRequired"), true);
	Root->SetNumberField(TEXT("instanceCount"), StableInstanceIdRecords.Num());
	Root->SetStringField(
		TEXT("hashScope"),
		TEXT("instance_id_component_actor_actor_class_component_class_tab_lf_utf8_sorted_by_component_path"));
	Root->SetStringField(TEXT("sha1"), StableInstanceIdMappingSha1);
	TArray<TSharedPtr<FJsonValue>> Instances;
	Instances.Reserve(StableInstanceIdRecords.Num());
	for (const FStableInstanceIdRecord& Record : StableInstanceIdRecords)
	{
		TSharedRef<FJsonObject> Instance = MakeShared<FJsonObject>();
		Instance->SetNumberField(TEXT("instanceId"), Record.InstanceId);
		Instance->SetStringField(TEXT("componentPath"), Record.ComponentPath);
		Instance->SetStringField(TEXT("actorPath"), Record.ActorPath);
		Instance->SetStringField(TEXT("actorClassPath"), Record.ActorClassPath);
		Instance->SetStringField(TEXT("componentClassPath"), Record.ComponentClassPath);
		Instances.Add(MakeShared<FJsonValueObject>(Instance));
	}
	Root->SetArrayField(TEXT("instances"), Instances);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Could not serialize stable instance-ID mapping JSON.");
		return false;
	}
	const FString Path = FPaths::Combine(ResolvedOutputDirectory, TEXT("instance_id_map.json"));
	const FString TempPath = Path + TEXT(".part");
	if (!FFileHelper::SaveStringToFile(Json, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!IFileManager::Get().Move(*Path, *TempPath, true, true, false, true))
	{
		OutError = FString::Printf(TEXT("Could not write stable instance-ID map: %s"), *Path);
		return false;
	}
	return true;
}

void USRDatasetCaptureSubsystem::RestoreStableInstanceIds()
{
	for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, FPrimitiveStencilState>& Pair :
		StableInstanceStencilStates)
	{
		if (UPrimitiveComponent* Component = Pair.Key.Get())
		{
			Component->SetRenderCustomDepth(Pair.Value.bRenderCustomDepth);
			Component->SetCustomDepthStencilWriteMask(
				static_cast<ERendererStencilMask>(Pair.Value.CustomDepthStencilWriteMask));
			Component->SetCustomDepthStencilValue(Pair.Value.CustomDepthStencilValue);
		}
	}
	StableInstanceStencilStates.Reset();
}

bool USRDatasetCaptureSubsystem::PrepareDeterministicCamera(FString& OutError)
{
	if (!ActiveJob.bUseDeterministicCameraTransform)
	{
		return true;
	}
	UWorld* World = GetWorld();
	APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	if (!World || !Controller)
	{
		OutError = TEXT("Deterministic camera override requires player controller 0.");
		return false;
	}
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(
		World,
		ACameraActor::StaticClass(),
		TEXT("SRDatasetDeterministicCamera"));
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.ObjectFlags |= RF_Transient;
	DeterministicCameraActor = World->SpawnActor<ACameraActor>(
		ACameraActor::StaticClass(),
		FTransform(
			ActiveJob.DeterministicCameraRotationDegrees,
			ActiveJob.DeterministicCameraLocationCm),
		SpawnParameters);
	if (!DeterministicCameraActor)
	{
		OutError = TEXT("Could not spawn the deterministic dataset camera.");
		return false;
	}
	if (UCameraComponent* Camera = DeterministicCameraActor->GetCameraComponent())
	{
		Camera->SetProjectionMode(ECameraProjectionMode::Perspective);
		Camera->SetFieldOfView(ActiveJob.DeterministicCameraFOVDegrees);
		Camera->SetAspectRatio(
			static_cast<float>(ActiveJob.HRResolution.X) /
			static_cast<float>(ActiveJob.HRResolution.Y));
	}
	DeterministicCameraPlayerController = Controller;
	PreviousPlayerViewTarget = Controller->GetViewTarget();
	EnforceDeterministicCamera(GetInitialEvaluationFrame());
	return true;
}

void USRDatasetCaptureSubsystem::EnforceDeterministicCamera(const int32 LogicalFrame)
{
	if (!ActiveJob.bUseDeterministicCameraTransform || !DeterministicCameraActor)
	{
		return;
	}
	const FVector LogicalLocation = ActiveJob.DeterministicCameraLocationCm +
		ActiveJob.DeterministicCameraTranslationPerLogicalFrameCm *
		static_cast<double>(LogicalFrame - ActiveJob.StartFrame);
	DeterministicCameraActor->SetActorLocationAndRotation(
		LogicalLocation,
		ActiveJob.DeterministicCameraRotationDegrees,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	if (APlayerController* Controller = DeterministicCameraPlayerController.Get())
	{
		if (Controller->GetViewTarget() != DeterministicCameraActor)
		{
			Controller->SetViewTarget(DeterministicCameraActor);
		}
	}
}

void USRDatasetCaptureSubsystem::RestoreDeterministicCamera()
{
	if (APlayerController* Controller = DeterministicCameraPlayerController.Get())
	{
		if (AActor* PreviousTarget = PreviousPlayerViewTarget.Get())
		{
			Controller->SetViewTarget(PreviousTarget);
		}
	}
	if (DeterministicCameraActor)
	{
		DeterministicCameraActor->Destroy();
		DeterministicCameraActor = nullptr;
	}
	DeterministicCameraPlayerController.Reset();
	PreviousPlayerViewTarget.Reset();
}

void USRDatasetCaptureSubsystem::CacheSkeletalPosesForLogicalFrame(const int32 FrameNumber)
{
	if (!ActiveJob.bCacheSkeletalAnimationPosesForReplay || !GetWorld())
	{
		return;
	}
	TMap<TWeakObjectPtr<USkinnedMeshComponent>, FSkeletalEndpointState>& FrameCache =
		CachedSkeletalPoseFrames.FindOrAdd(FrameNumber);
	FrameCache.Reset();
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		TInlineComponentArray<USkinnedMeshComponent*> Components;
		It->GetComponents(Components);
		for (USkinnedMeshComponent* Component : Components)
		{
			if (!IsValid(Component) || !Component->IsRegistered() || !Component->GetSkinnedAsset())
			{
				continue;
			}
			const TArray<FTransform>& CurrentBones = Component->GetComponentSpaceTransforms();
			if (CurrentBones.IsEmpty())
			{
				continue;
			}
			FSkeletalEndpointState State;
			State.ComponentPath = Component->GetPathName();
			State.SkinnedAssetPath = Component->GetSkinnedAsset()->GetPathName();
			State.ComponentSpaceTransforms = CurrentBones;
			State.BoneVisibilityStates = Component->GetBoneVisibilityStates();
			FrameCache.Add(Component, MoveTemp(State));
		}
	}
}

void USRDatasetCaptureSubsystem::ApplyCachedSkeletalPoses(const int32 FrameNumber)
{
	AppliedCachedSkeletalPoseComponentCount = 0;
	AppliedCachedSkeletalPoseBoneCount = 0;
	SkippedCachedSkeletalPoseComponents.Reset();
	if (!ActiveJob.bCacheSkeletalAnimationPosesForReplay)
	{
		return;
	}
	const TMap<TWeakObjectPtr<USkinnedMeshComponent>, FSkeletalEndpointState>* FrameCache =
		CachedSkeletalPoseFrames.Find(FrameNumber);
	if (!FrameCache)
	{
		SkippedCachedSkeletalPoseComponents.Add(
			FString::Printf(TEXT("logical_frame_%d|cache_missing"), FrameNumber));
		return;
	}
	for (const TPair<TWeakObjectPtr<USkinnedMeshComponent>, FSkeletalEndpointState>& Pair : *FrameCache)
	{
		USkinnedMeshComponent* Component = Pair.Key.Get();
		const FSkeletalEndpointState& State = Pair.Value;
		if (!IsValid(Component) || !Component->IsRegistered() || !Component->GetSkinnedAsset() ||
			Component->GetSkinnedAsset()->GetPathName() != State.SkinnedAssetPath)
		{
			SkippedCachedSkeletalPoseComponents.Add(State.ComponentPath + TEXT("|unavailable_or_asset_changed"));
			continue;
		}
		// Animation FinalizeBoneTransform has already flipped the double buffer for
		// this world tick. At this point the public editable array is deliberately
		// the *previous* buffer (used separately by endpoint motion below), while
		// GetComponentSpaceTransforms is the current render pose. The component is
		// non-const, so update that current read buffer explicitly.
		TArray<FTransform>& CurrentTransforms = const_cast<TArray<FTransform>&>(
			Component->GetComponentSpaceTransforms());
		if (CurrentTransforms.Num() != State.ComponentSpaceTransforms.Num())
		{
			SkippedCachedSkeletalPoseComponents.Add(State.ComponentPath + TEXT("|bone_count_changed"));
			continue;
		}
		CurrentTransforms = State.ComponentSpaceTransforms;
		TArray<uint8>& CurrentVisibility = const_cast<TArray<uint8>&>(
			Component->GetBoneVisibilityStates());
		if (CurrentVisibility.Num() == State.BoneVisibilityStates.Num())
		{
			CurrentVisibility = State.BoneVisibilityStates;
		}
		else if (!State.BoneVisibilityStates.IsEmpty())
		{
			SkippedCachedSkeletalPoseComponents.Add(State.ComponentPath + TEXT("|visibility_count_changed"));
			continue;
		}
		Component->UpdateBounds();
		Component->MarkRenderTransformDirty();
		Component->MarkRenderDynamicDataDirty();
		++AppliedCachedSkeletalPoseComponentCount;
		AppliedCachedSkeletalPoseBoneCount += State.ComponentSpaceTransforms.Num();
	}
	SkippedCachedSkeletalPoseComponents.Sort();
}

FString USRDatasetCaptureSubsystem::ResolveProjectFile(const FString& InPath) const
{
	FString FullPath = FPaths::IsRelative(InPath)
		? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), InPath)
		: FPaths::ConvertRelativePathToFull(InPath);
	FPaths::CollapseRelativeDirectories(FullPath);
	return FullPath;
}

bool USRDatasetCaptureSubsystem::SaveSkeletalPoseCacheArtifact(FString& OutError)
{
	if (ActiveJob.SkeletalPoseCacheOutputFile.IsEmpty())
	{
		return true;
	}
	const int32 ExpectedFrameCount = ActiveJob.EndFrame - ActiveJob.StartFrame + 1;
	if (CachedSkeletalPoseFrames.Num() != ExpectedFrameCount)
	{
		OutError = FString::Printf(
			TEXT("Skeletal pose-cache bake produced %d frames; expected %d."),
			CachedSkeletalPoseFrames.Num(),
			ExpectedFrameCount);
		return false;
	}

	FBufferArchive Archive;
	uint32 Magic = 0x53525043; // 'SRPC'
	int32 Version = 1;
	FString EngineVersion = FEngineVersion::Current().ToString();
	FString WorldPackage = GetWorld() ? GetWorld()->GetOutermost()->GetName() : FString();
	FString ValidationActorClass = ActiveJob.NonFixtureSkeletalValidationActorClass.ToString();
	int32 StartFrame = ActiveJob.StartFrame;
	int32 EndFrame = ActiveJob.EndFrame;
	int32 FrameRateNumerator = ActiveJob.CaptureFrameRateNumerator;
	int32 FrameRateDenominator = ActiveJob.CaptureFrameRateDenominator;
	int32 RandomSeed = ActiveJob.RandomSeed;
	Archive << Magic << Version << EngineVersion << WorldPackage << ValidationActorClass;
	Archive << StartFrame << EndFrame << FrameRateNumerator << FrameRateDenominator << RandomSeed;
	int32 FrameCount = CachedSkeletalPoseFrames.Num();
	Archive << FrameCount;

	TArray<int32> FrameNumbers;
	CachedSkeletalPoseFrames.GetKeys(FrameNumbers);
	FrameNumbers.Sort();
	for (int32 FrameNumber : FrameNumbers)
	{
		Archive << FrameNumber;
		TArray<FSkeletalEndpointState> States;
		CachedSkeletalPoseFrames.FindChecked(FrameNumber).GenerateValueArray(States);
		States.Sort([](const FSkeletalEndpointState& Left, const FSkeletalEndpointState& Right)
		{
			return Left.ComponentPath.Compare(Right.ComponentPath, ESearchCase::CaseSensitive) < 0;
		});
		int32 ComponentCount = States.Num();
		Archive << ComponentCount;
		for (FSkeletalEndpointState& State : States)
		{
			Archive << State.ComponentPath << State.SkinnedAssetPath;
			int32 BoneCount = State.ComponentSpaceTransforms.Num();
			Archive << BoneCount;
			for (FTransform& Transform : State.ComponentSpaceTransforms)
			{
				Archive << Transform;
			}
			int32 VisibilityCount = State.BoneVisibilityStates.Num();
			Archive << VisibilityCount;
			for (uint8& Visibility : State.BoneVisibilityStates)
			{
				Archive << Visibility;
			}
		}
	}

	const FString ArtifactPath = ResolveProjectFile(ActiveJob.SkeletalPoseCacheOutputFile);
	const FString ParentDirectory = FPaths::GetPath(ArtifactPath);
	if ((!IFileManager::Get().MakeDirectory(*ParentDirectory, true) &&
		 !IFileManager::Get().DirectoryExists(*ParentDirectory)))
	{
		OutError = FString::Printf(TEXT("Could not create skeletal pose-cache directory: %s"), *ParentDirectory);
		return false;
	}
	const FString TempPath = ArtifactPath + TEXT(".part");
	if (!FFileHelper::SaveArrayToFile(Archive, *TempPath) ||
		!IFileManager::Get().Move(*ArtifactPath, *TempPath, true, true, false, true))
	{
		OutError = FString::Printf(TEXT("Could not write skeletal pose-cache artifact: %s"), *ArtifactPath);
		return false;
	}
	SkeletalPoseCacheArtifactSha1 = HashFile(ArtifactPath);
	if (SkeletalPoseCacheArtifactSha1.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Could not hash skeletal pose-cache artifact: %s"), *ArtifactPath);
		return false;
	}
	bSkeletalPoseCacheLoadedFromArtifact = false;
	return true;
}

bool USRDatasetCaptureSubsystem::LoadSkeletalPoseCacheArtifact(FString& OutError)
{
	if (ActiveJob.SkeletalPoseCacheInputFile.IsEmpty())
	{
		return true;
	}
	const FString ArtifactPath = ResolveProjectFile(ActiveJob.SkeletalPoseCacheInputFile);
	TArray<uint8> Data;
	if (!FFileHelper::LoadFileToArray(Data, *ArtifactPath))
	{
		OutError = FString::Printf(TEXT("Could not read skeletal pose-cache artifact: %s"), *ArtifactPath);
		return false;
	}
	FMemoryReader Reader(Data, true);
	uint32 Magic = 0;
	int32 Version = 0;
	FString EngineVersion;
	FString WorldPackage;
	FString ValidationActorClass;
	int32 StartFrame = INDEX_NONE;
	int32 EndFrame = INDEX_NONE;
	int32 FrameRateNumerator = 0;
	int32 FrameRateDenominator = 0;
	int32 RandomSeed = 0;
	Reader << Magic << Version << EngineVersion << WorldPackage << ValidationActorClass;
	Reader << StartFrame << EndFrame << FrameRateNumerator << FrameRateDenominator << RandomSeed;
	const FString ExpectedWorld = GetWorld() ? GetWorld()->GetOutermost()->GetName() : FString();
	if (Reader.IsError() || Magic != 0x53525043 || Version != 1 ||
		EngineVersion != FEngineVersion::Current().ToString() ||
		WorldPackage != ExpectedWorld ||
		ValidationActorClass != ActiveJob.NonFixtureSkeletalValidationActorClass.ToString() ||
		StartFrame != ActiveJob.StartFrame || EndFrame != ActiveJob.EndFrame ||
		FrameRateNumerator != ActiveJob.CaptureFrameRateNumerator ||
		FrameRateDenominator != ActiveJob.CaptureFrameRateDenominator ||
		RandomSeed != ActiveJob.RandomSeed)
	{
		OutError = TEXT("Skeletal pose-cache artifact header does not match this engine, map, frame range, rate, seed, or validation Actor class.");
		return false;
	}

	TMap<FString, USkinnedMeshComponent*> ComponentsByPath;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		TInlineComponentArray<USkinnedMeshComponent*> Components;
		It->GetComponents(Components);
		for (USkinnedMeshComponent* Component : Components)
		{
			if (IsValid(Component) && Component->IsRegistered() && Component->GetSkinnedAsset())
			{
				ComponentsByPath.Add(Component->GetPathName(), Component);
			}
		}
	}

	int32 FrameCount = 0;
	Reader << FrameCount;
	const int32 ExpectedFrameCount = ActiveJob.EndFrame - ActiveJob.StartFrame + 1;
	if (Reader.IsError() || FrameCount != ExpectedFrameCount)
	{
		OutError = FString::Printf(
			TEXT("Skeletal pose-cache artifact contains %d frames; expected %d."),
			FrameCount,
			ExpectedFrameCount);
		return false;
	}
	CachedSkeletalPoseFrames.Reset();
	for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
	{
		int32 FrameNumber = INDEX_NONE;
		int32 ComponentCount = 0;
		Reader << FrameNumber << ComponentCount;
		if (Reader.IsError() || FrameNumber < ActiveJob.StartFrame || FrameNumber > ActiveJob.EndFrame ||
			CachedSkeletalPoseFrames.Contains(FrameNumber) || ComponentCount <= 0 || ComponentCount > 100000)
		{
			OutError = TEXT("Skeletal pose-cache artifact contains an invalid or duplicate frame/component count.");
			CachedSkeletalPoseFrames.Reset();
			return false;
		}
		TMap<TWeakObjectPtr<USkinnedMeshComponent>, FSkeletalEndpointState>& FrameCache =
			CachedSkeletalPoseFrames.Add(FrameNumber);
		TSet<FString> SeenComponentPaths;
		for (int32 ComponentIndex = 0; ComponentIndex < ComponentCount; ++ComponentIndex)
		{
			FSkeletalEndpointState State;
			Reader << State.ComponentPath << State.SkinnedAssetPath;
			int32 BoneCount = 0;
			Reader << BoneCount;
			if (Reader.IsError() || BoneCount <= 0 || BoneCount > 100000 ||
				SeenComponentPaths.Contains(State.ComponentPath))
			{
				OutError = TEXT("Skeletal pose-cache artifact contains invalid bone data or duplicate component paths.");
				CachedSkeletalPoseFrames.Reset();
				return false;
			}
			SeenComponentPaths.Add(State.ComponentPath);
			State.ComponentSpaceTransforms.SetNum(BoneCount);
			for (FTransform& Transform : State.ComponentSpaceTransforms)
			{
				Reader << Transform;
			}
			int32 VisibilityCount = 0;
			Reader << VisibilityCount;
			if (Reader.IsError() || VisibilityCount < 0 || VisibilityCount > 100000)
			{
				OutError = TEXT("Skeletal pose-cache artifact contains an invalid visibility-state count.");
				CachedSkeletalPoseFrames.Reset();
				return false;
			}
			State.BoneVisibilityStates.SetNum(VisibilityCount);
			for (uint8& Visibility : State.BoneVisibilityStates)
			{
				Reader << Visibility;
			}
			USkinnedMeshComponent* const* ComponentPtr = ComponentsByPath.Find(State.ComponentPath);
			USkinnedMeshComponent* Component = ComponentPtr ? *ComponentPtr : nullptr;
			if (Reader.IsError() || !IsValid(Component) || !Component->GetSkinnedAsset() ||
				Component->GetSkinnedAsset()->GetPathName() != State.SkinnedAssetPath ||
				(!Component->GetComponentSpaceTransforms().IsEmpty() &&
				 Component->GetComponentSpaceTransforms().Num() != BoneCount))
			{
				OutError = FString::Printf(
					TEXT("Skeletal pose-cache component is unavailable or incompatible: %s"),
					*State.ComponentPath);
				CachedSkeletalPoseFrames.Reset();
				return false;
			}
			FrameCache.Add(Component, MoveTemp(State));
		}
	}
	if (Reader.IsError() || !Reader.AtEnd() || CachedSkeletalPoseFrames.Num() != ExpectedFrameCount)
	{
		OutError = TEXT("Skeletal pose-cache artifact ended unexpectedly or contains trailing data.");
		CachedSkeletalPoseFrames.Reset();
		return false;
	}
	SkeletalPoseCacheArtifactSha1 = HashFile(ArtifactPath);
	if (SkeletalPoseCacheArtifactSha1.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Could not hash skeletal pose-cache artifact: %s"), *ArtifactPath);
		CachedSkeletalPoseFrames.Reset();
		return false;
	}
	bSkeletalPoseCacheLoadedFromArtifact = true;
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
	PreviousRenderDeterminismCVars.Reset();
	const auto ApplyTrackedCVarOverride = [this](const TCHAR* Name, const TCHAR* Value)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			PreviousRenderDeterminismCVars.Add(Name, Variable->GetString());
			Variable->Set(Value, ECVF_SetByCode);
		}
	};

	if (ActiveJob.bCaptureMainViewTemporalDiagnostics)
	{
		// WPO on an otherwise stationary component only participates in the
		// velocity pass when vertex-deformation velocity output is enabled.
		ApplyTrackedCVarOverride(TEXT("r.Velocity.EnableVertexDeformation"), TEXT("1"));
	}

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
	if (ActiveJob.bCaptureTemporalDiagnostics)
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
		// A simulated previous transform must still reach FScene when the current
		// endpoint happens to equal a recently primed intermediate transform.
		ApplyTrackedCVarOverride(TEXT("r.SkipRedundantTransformUpdate"), TEXT("0"));
	}
	if (ActiveJob.bLockTemporalJitterToLogicalFrame)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.TemporalAA.Debug.OverrideTemporalIndex")))
		{
			PreviousTemporalJitterOverrideIndex = Variable->GetInt();
			// SnapshotProvenance sees the same neutral value in every replay;
			// HandleWorldPreActorTick applies the logical-frame phase dynamically.
			Variable->Set(0, ECVF_SetByCode);
			bOverrodeTemporalJitterIndex = true;
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
		for (const TPair<const TCHAR*, const TCHAR*>& Override : Overrides)
		{
			ApplyTrackedCVarOverride(Override.Key, Override.Value);
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
	if (bOverrodeTemporalJitterIndex)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.TemporalAA.Debug.OverrideTemporalIndex")))
		{
			Variable->Set(PreviousTemporalJitterOverrideIndex, ECVF_SetByCode);
		}
	}
	PreviousTemporalJitterOverrideIndex = -1;
	bOverrodeTemporalJitterIndex = false;
	ClearLogicalViewStateFrameIndex();
	if (bOverrodeWorldRendering)
	{
		UGameplayStatics::SetEnableWorldRendering(this, bPreviousWorldRenderingEnabled);
	}
	bOverrodeWorldRendering = false;
	LastCapturedEndpointTransforms.Reset();
	LastCapturedEndpointBoneStates.Reset();
	AppliedEndpointBoneComponentCount = 0;
	AppliedEndpointBoneCount = 0;
	SkippedEndpointBoneComponents.Reset();
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

void USRDatasetCaptureSubsystem::ApplyLogicalTemporalJitter(const int32 FrameNumber)
{
	if (!ActiveJob.bLockTemporalJitterToLogicalFrame)
	{
		return;
	}
	if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
		TEXT("r.TemporalAA.Debug.OverrideTemporalIndex")))
	{
		Variable->Set(GetTemporalJitterOverrideIndex(FrameNumber), ECVF_SetByCode);
	}
	const uint32 ViewStateFrameIndex = GetLogicalViewStateFrameIndex(FrameNumber);
	if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> ViewExtension =
		GetSRDatasetViewExtension())
	{
		ViewExtension->SetDeterministicViewFrameIndex(ViewStateFrameIndex);
	}
	if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> TonemapExtension =
		GetSRDatasetTonemapViewExtension())
	{
		TonemapExtension->SetDeterministicViewFrameIndex(ViewStateFrameIndex);
	}
}

void USRDatasetCaptureSubsystem::ClearLogicalViewStateFrameIndex()
{
	if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> ViewExtension =
		GetSRDatasetViewExtension())
	{
		ViewExtension->ClearDeterministicViewFrameIndex();
	}
	if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> TonemapExtension =
		GetSRDatasetTonemapViewExtension())
	{
		TonemapExtension->ClearDeterministicViewFrameIndex();
	}
}

void USRDatasetCaptureSubsystem::ApplyLogicalMaterialTime(const int32 FrameNumber)
{
	if (!ActiveJob.bLockMaterialTimeToLogicalFrame)
	{
		return;
	}

	// Keep a non-zero signed view delta even for warmup/reset frames so the
	// renderer advances temporal view state. Reset metadata makes that history
	// unusable for training, while the explicit material previous frame remains
	// auditable and independent of wall-clock time.
	int32 PreviousFrameNumber = FrameNumber - GetEvaluationDirection();
	if (Status.State == ESRDatasetCaptureState::Capturing && ShouldCaptureFrame(FrameNumber))
	{
		const bool bFirstCapturedFrame = FrameNumber == GetFirstCapturedFrame();
		if (ActiveJob.ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate)
		{
			PreviousFrameNumber = FrameNumber - 1;
		}
		else if (!bFirstCapturedFrame)
		{
			PreviousFrameNumber = GetPreviouslyCapturedFrame(FrameNumber);
		}
	}

	const double FixedDeltaSeconds = ActiveJob.GetFixedDeltaSeconds();
	const double CurrentTimeSeconds = static_cast<double>(FrameNumber) * FixedDeltaSeconds;
	const float MaterialDeltaSeconds = static_cast<float>(
		static_cast<double>(FrameNumber - PreviousFrameNumber) * FixedDeltaSeconds);
	if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> ViewExtension =
		GetSRDatasetViewExtension())
	{
		ViewExtension->SetDeterministicViewTime(CurrentTimeSeconds, MaterialDeltaSeconds);
	}
	if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> TonemapExtension =
		GetSRDatasetTonemapViewExtension())
	{
		TonemapExtension->SetDeterministicViewTime(CurrentTimeSeconds, MaterialDeltaSeconds);
	}
}

void USRDatasetCaptureSubsystem::ClearLogicalMaterialTime()
{
	if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> ViewExtension =
		GetSRDatasetViewExtension())
	{
		ViewExtension->ClearDeterministicViewTime();
	}
	if (const TSharedPtr<FSRDatasetViewExtension, ESPMode::ThreadSafe> TonemapExtension =
		GetSRDatasetTonemapViewExtension())
	{
		TonemapExtension->ClearDeterministicViewTime();
	}
}

TArray<FString> USRDatasetCaptureSubsystem::GetActiveWidgetComponentPaths() const
{
	TArray<FString> Paths;
	if (!GetWorld())
	{
		return Paths;
	}
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		const AActor* Actor = *It;
		if (!IsValid(Actor) || Actor->IsHidden())
		{
			continue;
		}
		TInlineComponentArray<UWidgetComponent*> Components;
		Actor->GetComponents(Components);
		for (const UWidgetComponent* Component : Components)
		{
			if (IsValid(Component) && Component->IsRegistered() && Component->IsVisible())
			{
				Paths.Add(Component->GetPathName());
			}
		}
	}
	Paths.Sort([](const FString& Left, const FString& Right)
	{
		return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
	});
	return Paths;
}

bool USRDatasetCaptureSubsystem::CheckWidgetComponentPolicy(FString& OutError) const
{
	if (!ActiveJob.bRejectVisibleWidgetComponents)
	{
		return true;
	}
	const TArray<FString> Paths = GetActiveWidgetComponentPaths();
	if (!Paths.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("HUD-less capture rejects visible UWidgetComponent scene residue. Hide/remove or explicitly classify: %s"),
			*FString::Join(Paths, TEXT(", ")));
		return false;
	}
	return true;
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
		TEXT("r.ForwardShading"),
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
		TEXT("r.SkipRedundantTransformUpdate"),
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
		TEXT("r.TemporalAA.Debug.OverrideTemporalIndex"),
		TEXT("r.Velocity.EnableVertexDeformation"),
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
			const FString CanonicalState =
				ISRDatasetControllable::Execute_DatasetGetDeterministicState(Actor);
			if (CanonicalState.IsEmpty())
			{
				Summary.ControllableActorsWithoutState.Add(Actor->GetPathName());
			}
			else
			{
				FTCHARToUTF8 Utf8(*CanonicalState);
				FSceneStateSummary::FControllableStateSummary& State =
					Summary.ControllableStates.AddDefaulted_GetRef();
				State.ActorPath = Actor->GetPathName();
				State.StateSha1 = HashString(CanonicalState);
				State.Utf8Bytes = Utf8.Length();
				++Summary.ControllableStateCount;
				StateLines.Add(FString::Printf(
					TEXT("controllable_state|%s|sha1=%s|utf8Bytes=%d"),
					*State.ActorPath,
					*State.StateSha1,
					State.Utf8Bytes));
			}
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
				FSceneStateSummary::FNiagaraSummary NiagaraSummary;
				NiagaraSummary.ComponentPath = Niagara->GetPathName();
				NiagaraSummary.AssetPath = Niagara->GetAsset() ? Niagara->GetAsset()->GetPathName() : TEXT("none");
				NiagaraSummary.DesiredAgeSeconds = Niagara->GetDesiredAge();
				NiagaraSummary.ComponentLocationCm = Niagara->GetComponentLocation();
				NiagaraSummary.ComponentBoundsOriginCm = Niagara->Bounds.Origin;
				NiagaraSummary.ComponentBoundsExtentCm = Niagara->Bounds.BoxExtent;
				for (int32 MaterialIndex = 0; MaterialIndex < Niagara->GetNumMaterials(); ++MaterialIndex)
				{
					const UMaterialInterface* Material = Niagara->GetMaterial(MaterialIndex);
					NiagaraSummary.MaterialPaths.Add(Material ? Material->GetPathName() : TEXT("none"));
				}
				if (const UNiagaraSystem* System = Niagara->GetAsset())
				{
					NiagaraSummary.bSystemDeterminism = System->NeedsDeterminism();
					NiagaraSummary.bSystemFixedTick = System->HasFixedTickDelta();
					NiagaraSummary.SystemFixedTickSeconds = System->GetFixedTickDeltaTime();
				}
				if (FNiagaraSystemInstanceControllerConstPtr Controller = Niagara->GetSystemInstanceController())
				{
					NiagaraSummary.SimulationAgeSeconds = Controller->GetAge();
					if (const FNiagaraSystemInstance* Instance = Controller->GetSoloSystemInstance())
					{
						NiagaraSummary.bSoloInstanceObservable = true;
						for (const FNiagaraEmitterInstanceRef& EmitterRef : Instance->GetEmitters())
						{
							const FNiagaraEmitterInstance& Emitter = EmitterRef.Get();
							const int32 ParticleCount = Emitter.GetNumParticles();
							++NiagaraSummary.EmitterCount;
							NiagaraSummary.EmitterParticleCounts.Add(ParticleCount);
							NiagaraSummary.EmitterDeterminism.Add(Emitter.IsDeterministic());
							const FVersionedNiagaraEmitterData* EmitterData =
								Emitter.GetEmitterHandle().GetEmitterData();
							NiagaraSummary.EmitterRandomSeeds.Add(
								EmitterData ? EmitterData->RandomSeed : 0);
							NiagaraSummary.ParticleCount += ParticleCount;
							NiagaraSummary.TotalSpawnedParticleCount += Emitter.GetTotalSpawnedParticles();
							Emitter.GetEmitterHandle().ForEachEnabledRendererWithIndex(
								[&NiagaraSummary](const UNiagaraRendererProperties*, int32)
								{
									++NiagaraSummary.RendererCount;
								});
							if (Emitter.GetSimTarget() == ENiagaraSimTarget::GPUComputeSim)
							{
								++NiagaraSummary.GPUEmitterCount;
							}
							else
							{
								++NiagaraSummary.CPUEmitterCount;
							}
							const FBox EmitterBounds = Emitter.GetBounds();
							StateLines.Add(FString::Printf(
								TEXT("niagaraEmitter|%s|name=%s|simTarget=%d|deterministic=%d|randomSeed=%d|particles=%d|totalSpawned=%d|boundsMin=(%.9g,%.9g,%.9g)|boundsMax=(%.9g,%.9g,%.9g)"),
								*Niagara->GetPathName(),
								*Emitter.GetEmitterHandle().GetUniqueInstanceName(),
								static_cast<int32>(Emitter.GetSimTarget()),
								Emitter.IsDeterministic() ? 1 : 0,
								EmitterData ? EmitterData->RandomSeed : 0,
								ParticleCount,
								Emitter.GetTotalSpawnedParticles(),
								EmitterBounds.Min.X, EmitterBounds.Min.Y, EmitterBounds.Min.Z,
								EmitterBounds.Max.X, EmitterBounds.Max.Y, EmitterBounds.Max.Z));
						}
					}
				}
				Summary.NiagaraEmitterCount += NiagaraSummary.EmitterCount;
				Summary.NiagaraCPUEmitterCount += NiagaraSummary.CPUEmitterCount;
				Summary.NiagaraGPUEmitterCount += NiagaraSummary.GPUEmitterCount;
				Summary.NiagaraParticleCount += NiagaraSummary.ParticleCount;
				Summary.NiagaraTotalSpawnedParticleCount += NiagaraSummary.TotalSpawnedParticleCount;
				Summary.NiagaraComponents.Add(MoveTemp(NiagaraSummary));
				StateLines.Add(FString::Printf(
					TEXT("niagara|%s|asset=%s|active=%d|ageMode=%d|desiredAge=%.9g|seedOffset=%d|forceSolo=%d|seekDelta=%.9g|maxSimTime=%.9g|systemDeterminism=%d|systemFixedTick=%d|systemFixedTickSeconds=%.9g|simulationAgeSeconds=%.9g|emitters=%d|renderers=%d|particles=%d|totalSpawned=%d|location=(%.9g,%.9g,%.9g)|boundsOrigin=(%.9g,%.9g,%.9g)|boundsExtent=(%.9g,%.9g,%.9g)|materials=%s"),
					*Niagara->GetPathName(),
					Niagara->GetAsset() ? *Niagara->GetAsset()->GetPathName() : TEXT("none"),
					Niagara->IsActive() ? 1 : 0,
					static_cast<int32>(Niagara->GetAgeUpdateMode()),
					Niagara->GetDesiredAge(),
					Niagara->GetRandomSeedOffset(),
					Niagara->GetForceSolo() ? 1 : 0,
					Niagara->GetSeekDelta(),
					Niagara->GetMaxSimTime(),
					Summary.NiagaraComponents.Last().bSystemDeterminism ? 1 : 0,
					Summary.NiagaraComponents.Last().bSystemFixedTick ? 1 : 0,
					Summary.NiagaraComponents.Last().SystemFixedTickSeconds,
					Summary.NiagaraComponents.Last().SimulationAgeSeconds,
					Summary.NiagaraComponents.Last().EmitterCount,
					Summary.NiagaraComponents.Last().RendererCount,
					Summary.NiagaraComponents.Last().ParticleCount,
					Summary.NiagaraComponents.Last().TotalSpawnedParticleCount,
					Summary.NiagaraComponents.Last().ComponentLocationCm.X,
					Summary.NiagaraComponents.Last().ComponentLocationCm.Y,
					Summary.NiagaraComponents.Last().ComponentLocationCm.Z,
					Summary.NiagaraComponents.Last().ComponentBoundsOriginCm.X,
					Summary.NiagaraComponents.Last().ComponentBoundsOriginCm.Y,
					Summary.NiagaraComponents.Last().ComponentBoundsOriginCm.Z,
					Summary.NiagaraComponents.Last().ComponentBoundsExtentCm.X,
					Summary.NiagaraComponents.Last().ComponentBoundsExtentCm.Y,
					Summary.NiagaraComponents.Last().ComponentBoundsExtentCm.Z,
					*FString::Join(Summary.NiagaraComponents.Last().MaterialPaths, TEXT(","))));
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
	Summary.ControllableActorsWithoutState.Sort();
	Summary.ControllableStates.Sort([](
		const FSceneStateSummary::FControllableStateSummary& Left,
		const FSceneStateSummary::FControllableStateSummary& Right)
	{
		return Left.ActorPath.Compare(Right.ActorPath, ESearchCase::CaseSensitive) < 0;
	});
	Summary.UncontrolledTickingActors.Sort();
	Summary.NiagaraComponents.Sort([](
		const FSceneStateSummary::FNiagaraSummary& Left,
		const FSceneStateSummary::FNiagaraSummary& Right)
	{
		return Left.ComponentPath.Compare(Right.ComponentPath, ESearchCase::CaseSensitive) < 0;
	});
	Summary.Sha1 = HashString(FString::Join(StateLines, TEXT("\n")));
	return Summary;
}

bool USRDatasetCaptureSubsystem::ValidateControllableStates(FString& OutError) const
{
	if (!ActiveJob.bRequireControllableState || !GetWorld())
	{
		return true;
	}

	TArray<FString> MissingActors;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || !Actor->GetClass()->ImplementsInterface(
			USRDatasetControllable::StaticClass()))
		{
			continue;
		}
		if (ISRDatasetControllable::Execute_DatasetGetDeterministicState(Actor).IsEmpty())
		{
			MissingActors.Add(Actor->GetPathName());
		}
	}
	MissingActors.Sort();
	if (!MissingActors.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("bRequireControllableState rejected %d SRDatasetControllable Actor(s) with empty canonical state: %s"),
			MissingActors.Num(),
			*FString::Join(MissingActors, TEXT(", ")));
		return false;
	}
	return true;
}

bool USRDatasetCaptureSubsystem::MatchesSceneControlClassRule(
	const FString& ClassPath,
	const TArray<FString>& Rules)
{
	for (const FString& RawRule : Rules)
	{
		const FString Rule = RawRule.TrimStartAndEnd();
		if (Rule.EndsWith(TEXT("*"), ESearchCase::CaseSensitive))
		{
			if (ClassPath.StartsWith(Rule.LeftChop(1), ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		else if (ClassPath.Equals(Rule, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

bool USRDatasetCaptureSubsystem::RunSceneControlPreflight(FString& OutError)
{
	SceneControlPreflight = FSceneControlPreflightReport();
	if (!ActiveJob.bRunSceneControlPreflight)
	{
		return true;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		OutError = TEXT("Scene-control preflight requires a valid world.");
		return false;
	}

	SceneControlPreflight.bRan = true;
	const auto NormalizeRules = [](const TArray<FString>& Source, TArray<FString>& Destination)
	{
		Destination.Reset(Source.Num());
		for (const FString& Rule : Source)
		{
			Destination.Add(Rule.TrimStartAndEnd());
		}
		Destination.Sort([](const FString& Left, const FString& Right)
		{
			return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
		});
	};
	NormalizeRules(
		ActiveJob.SceneControlAllowedTickingActorClassPaths,
		SceneControlPreflight.AllowedTickingActorClassPaths);
	NormalizeRules(
		ActiveJob.SceneControlAllowedTickingComponentClassPaths,
		SceneControlPreflight.AllowedTickingComponentClassPaths);
	NormalizeRules(
		ActiveJob.SceneControlAllowedNiagaraDataInterfaceClassPaths,
		SceneControlPreflight.AllowedNiagaraDataInterfaceClassPaths);
	NormalizeRules(
		ActiveJob.SceneControlAllowedMaterialExpressionClassPaths,
		SceneControlPreflight.AllowedMaterialExpressionClassPaths);

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

		const bool bActorControlledByInterface = Actor->GetClass()->ImplementsInterface(
			USRDatasetControllable::StaticClass());
		const bool bActorControlledBySubsystem =
			Actor == CaptureRig.Get() ||
			Actor == ValidationFixture.Get() ||
			Actor == SequenceActor.Get() ||
			Actor == DeterministicCameraActor.Get() ||
			Actor == NonFixtureSkeletalValidationActor.Get() ||
			Actor == ProjectAnimatedMaterialValidationReceiver.Get();
		const bool bActorControlled = bActorControlledByInterface || bActorControlledBySubsystem;
		if (Actor->IsActorTickEnabled())
		{
			const FString ClassPath = Actor->GetClass()->GetPathName();
			const FString Entry = FString::Printf(
				TEXT("%s|%s|tick=actor"),
				*Actor->GetPathName(),
				*ClassPath);
			if (bActorControlled)
			{
				SceneControlPreflight.ControlledTickingActors.Add(Entry);
			}
			else if (MatchesSceneControlClassRule(
				ClassPath,
				SceneControlPreflight.AllowedTickingActorClassPaths))
			{
				SceneControlPreflight.AllowedTickingActors.Add(Entry);
			}
			else
			{
				SceneControlPreflight.UncontrolledTickingActors.Add(Entry);
			}
		}

		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!IsValid(Component))
			{
				continue;
			}

			if (Component->IsRegistered() && Component->IsComponentTickEnabled())
			{
				const FString ClassPath = Component->GetClass()->GetPathName();
				const FString Entry = FString::Printf(
					TEXT("%s|%s|owner=%s|tick=component"),
					*Component->GetPathName(),
					*ClassPath,
					*Actor->GetPathName());
				if (bActorControlled)
				{
					SceneControlPreflight.ControlledTickingComponents.Add(Entry);
				}
				else if (MatchesSceneControlClassRule(
					ClassPath,
					SceneControlPreflight.AllowedTickingComponentClassPaths))
				{
					SceneControlPreflight.AllowedTickingComponents.Add(Entry);
				}
				else
				{
					SceneControlPreflight.UncontrolledTickingComponents.Add(Entry);
				}
			}

			UNiagaraComponent* Niagara = Cast<UNiagaraComponent>(Component);
			if (Niagara && Niagara->GetAsset())
			{
				UNiagaraSystem* System = Niagara->GetAsset();
				const FString ComponentPath = Niagara->GetPathName();
				const FString AssetPath = System->GetPathName();
				const auto RecordDataInterface = [this, &ComponentPath, &AssetPath](
					const FString& Source,
					const FString& VariableName,
					const UNiagaraDataInterface* DataInterface)
				{
					if (!IsValid(DataInterface))
					{
						return;
					}
					const FString ClassPath = DataInterface->GetClass()->GetPathName();
					const FString Entry = FString::Printf(
						TEXT("%s|asset=%s|source=%s|variable=%s|object=%s|class=%s"),
						*ComponentPath,
						*AssetPath,
						*Source,
						*VariableName,
						*DataInterface->GetPathName(),
						*ClassPath);
					SceneControlPreflight.NiagaraDataInterfaces.AddUnique(Entry);
					if (MatchesSceneControlClassRule(
						ClassPath,
						SceneControlPreflight.AllowedNiagaraDataInterfaceClassPaths))
					{
						SceneControlPreflight.AllowedNiagaraDataInterfaces.AddUnique(Entry);
					}
					else
					{
						SceneControlPreflight.UncontrolledNiagaraDataInterfaces.AddUnique(Entry);
					}
				};
				const auto RecordParameterStore = [&RecordDataInterface](
					const FString& Source,
					const FNiagaraParameterStore& Store)
				{
					for (const UNiagaraDataInterface* DataInterface : Store.GetDataInterfaces())
					{
						const FNiagaraVariableBase* Variable = Store.FindVariableFromDataInterface(DataInterface);
						RecordDataInterface(
							Source,
							Variable ? Variable->GetName().ToString() : TEXT("unknown"),
							DataInterface);
					}
				};

				RecordParameterStore(TEXT("component_override"), Niagara->GetOverrideParameters());
				RecordParameterStore(TEXT("system_exposed"), System->GetExposedParameters());
				if (Niagara->GetForceSolo())
				{
					if (FNiagaraSystemInstanceControllerPtr Controller = Niagara->GetSystemInstanceController())
					{
						if (FNiagaraSystemInstance* Instance = Controller->GetSoloSystemInstance())
						{
							RecordParameterStore(TEXT("runtime_instance"), Instance->GetInstanceParameters());
						}
					}
				}
				System->ForEachScript([&RecordDataInterface](const UNiagaraScript* Script)
				{
					if (!Script)
					{
						return;
					}
					for (const FNiagaraScriptResolvedDataInterfaceInfo& Info : Script->GetResolvedDataInterfaces())
					{
						RecordDataInterface(
							FString::Printf(TEXT("script_resolved:%s"), *Script->GetPathName()),
							Info.Name.ToString(),
							Info.ResolvedDataInterface);
					}
				});
			}

			UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component);
			if (!Primitive || !Primitive->IsRegistered())
			{
				continue;
			}
			for (int32 MaterialIndex = 0; MaterialIndex < Primitive->GetNumMaterials(); ++MaterialIndex)
			{
				UMaterialInterface* MaterialInterface = Primitive->GetMaterial(MaterialIndex);
				UMaterial* Material = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
				if (!Material)
				{
					continue;
				}

				const auto RecordMaterialInput = [this, Primitive, MaterialInterface, Material, MaterialIndex](
					const UMaterialExpression* Expression,
					const TCHAR* Source,
					const bool bControlled)
				{
					if (!IsValid(Expression))
					{
						return;
					}
					const FString ClassPath = Expression->GetClass()->GetPathName();
					const FString Entry = FString::Printf(
						TEXT("%s|slot=%d|interface=%s|material=%s|expression=%s|class=%s|source=%s"),
						*Primitive->GetPathName(),
						MaterialIndex,
						MaterialInterface ? *MaterialInterface->GetPathName() : TEXT("none"),
						*Material->GetPathName(),
						*Expression->GetPathName(),
						*ClassPath,
						Source);
					if (bControlled)
					{
						SceneControlPreflight.ControlledMaterialInputs.AddUnique(Entry);
					}
					else if (MatchesSceneControlClassRule(
						ClassPath,
						SceneControlPreflight.AllowedMaterialExpressionClassPaths))
					{
						SceneControlPreflight.AllowedMaterialInputs.AddUnique(Entry);
					}
					else
					{
						SceneControlPreflight.UncontrolledMaterialInputs.AddUnique(Entry);
					}
				};

				TArray<UMaterialExpressionTime*> TimeExpressions;
				Material->GetAllExpressionsInMaterialAndFunctionsOfType(TimeExpressions);
				for (const UMaterialExpressionTime* Expression : TimeExpressions)
				{
					RecordMaterialInput(
						Expression,
						Expression->bIgnorePause ? TEXT("real_time") : TEXT("game_time"),
						ActiveJob.bLockMaterialTimeToLogicalFrame);
				}

				TArray<UMaterialExpressionPerInstanceRandom*> PerInstanceRandomExpressions;
				Material->GetAllExpressionsInMaterialAndFunctionsOfType(PerInstanceRandomExpressions);
				for (const UMaterialExpressionPerInstanceRandom* Expression : PerInstanceRandomExpressions)
				{
					RecordMaterialInput(Expression, TEXT("per_instance_random"), false);
				}

				TArray<UMaterialExpressionParticleRandom*> ParticleRandomExpressions;
				Material->GetAllExpressionsInMaterialAndFunctionsOfType(ParticleRandomExpressions);
				const bool bParticleRandomControlled =
					Niagara != nullptr && ActiveJob.bControlNiagara && ActiveJob.bForceNiagaraDeterminism;
				for (const UMaterialExpressionParticleRandom* Expression : ParticleRandomExpressions)
				{
					RecordMaterialInput(Expression, TEXT("particle_random"), bParticleRandomControlled);
				}
			}
		}
	}

	const auto SortStrings = [](TArray<FString>& Values)
	{
		Values.Sort([](const FString& Left, const FString& Right)
		{
			return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
		});
	};
	SortStrings(SceneControlPreflight.ControlledTickingActors);
	SortStrings(SceneControlPreflight.AllowedTickingActors);
	SortStrings(SceneControlPreflight.UncontrolledTickingActors);
	SortStrings(SceneControlPreflight.ControlledTickingComponents);
	SortStrings(SceneControlPreflight.AllowedTickingComponents);
	SortStrings(SceneControlPreflight.UncontrolledTickingComponents);
	SortStrings(SceneControlPreflight.NiagaraDataInterfaces);
	SortStrings(SceneControlPreflight.AllowedNiagaraDataInterfaces);
	SortStrings(SceneControlPreflight.UncontrolledNiagaraDataInterfaces);
	SortStrings(SceneControlPreflight.ControlledMaterialInputs);
	SortStrings(SceneControlPreflight.AllowedMaterialInputs);
	SortStrings(SceneControlPreflight.UncontrolledMaterialInputs);

	SceneControlPreflight.bPassed =
		SceneControlPreflight.UncontrolledTickingActors.IsEmpty() &&
		SceneControlPreflight.UncontrolledTickingComponents.IsEmpty() &&
		SceneControlPreflight.UncontrolledNiagaraDataInterfaces.IsEmpty() &&
		SceneControlPreflight.UncontrolledMaterialInputs.IsEmpty();

	TArray<FString> CanonicalLines;
	CanonicalLines.Add(TEXT("schemaVersion=1"));
	CanonicalLines.Add(FString::Printf(
		TEXT("required=%d"),
		ActiveJob.bRequireSceneControlPreflight ? 1 : 0));
	const auto AddCanonicalLines = [&CanonicalLines](const TCHAR* Label, const TArray<FString>& Values)
	{
		for (const FString& Value : Values)
		{
			CanonicalLines.Add(FString::Printf(TEXT("%s|%s"), Label, *Value));
		}
	};
	AddCanonicalLines(TEXT("allowedTickingActorClassPath"), SceneControlPreflight.AllowedTickingActorClassPaths);
	AddCanonicalLines(TEXT("allowedTickingComponentClassPath"), SceneControlPreflight.AllowedTickingComponentClassPaths);
	AddCanonicalLines(TEXT("allowedNiagaraDataInterfaceClassPath"), SceneControlPreflight.AllowedNiagaraDataInterfaceClassPaths);
	AddCanonicalLines(TEXT("allowedMaterialExpressionClassPath"), SceneControlPreflight.AllowedMaterialExpressionClassPaths);
	AddCanonicalLines(TEXT("controlledTickingActor"), SceneControlPreflight.ControlledTickingActors);
	AddCanonicalLines(TEXT("allowedTickingActor"), SceneControlPreflight.AllowedTickingActors);
	AddCanonicalLines(TEXT("uncontrolledTickingActor"), SceneControlPreflight.UncontrolledTickingActors);
	AddCanonicalLines(TEXT("controlledTickingComponent"), SceneControlPreflight.ControlledTickingComponents);
	AddCanonicalLines(TEXT("allowedTickingComponent"), SceneControlPreflight.AllowedTickingComponents);
	AddCanonicalLines(TEXT("uncontrolledTickingComponent"), SceneControlPreflight.UncontrolledTickingComponents);
	AddCanonicalLines(TEXT("niagaraDataInterface"), SceneControlPreflight.NiagaraDataInterfaces);
	AddCanonicalLines(TEXT("allowedNiagaraDataInterface"), SceneControlPreflight.AllowedNiagaraDataInterfaces);
	AddCanonicalLines(TEXT("uncontrolledNiagaraDataInterface"), SceneControlPreflight.UncontrolledNiagaraDataInterfaces);
	AddCanonicalLines(TEXT("controlledMaterialInput"), SceneControlPreflight.ControlledMaterialInputs);
	AddCanonicalLines(TEXT("allowedMaterialInput"), SceneControlPreflight.AllowedMaterialInputs);
	AddCanonicalLines(TEXT("uncontrolledMaterialInput"), SceneControlPreflight.UncontrolledMaterialInputs);
	SceneControlPreflight.Sha1 = HashString(FString::Join(CanonicalLines, TEXT("\n")));

	if (!WriteSceneControlPreflightReport(OutError))
	{
		return false;
	}
	if (ActiveJob.bRequireSceneControlPreflight && !SceneControlPreflight.bPassed)
	{
		OutError = FString::Printf(
			TEXT("Scene-control preflight failed: actors=%d components=%d NiagaraDIs=%d materialInputs=%d. Inspect %s."),
			SceneControlPreflight.UncontrolledTickingActors.Num(),
			SceneControlPreflight.UncontrolledTickingComponents.Num(),
			SceneControlPreflight.UncontrolledNiagaraDataInterfaces.Num(),
			SceneControlPreflight.UncontrolledMaterialInputs.Num(),
			*FPaths::Combine(ResolvedOutputDirectory, TEXT("scene_control_preflight.json")));
		return false;
	}
	if (!SceneControlPreflight.bPassed)
	{
		UE_LOG(
			LogSRDataset,
			Warning,
			TEXT("Scene-control preflight is report-only and found %d Actor, %d component, %d Niagara DI and %d material-input exception(s)."),
			SceneControlPreflight.UncontrolledTickingActors.Num(),
			SceneControlPreflight.UncontrolledTickingComponents.Num(),
			SceneControlPreflight.UncontrolledNiagaraDataInterfaces.Num(),
			SceneControlPreflight.UncontrolledMaterialInputs.Num());
	}
	return true;
}

bool USRDatasetCaptureSubsystem::WriteSceneControlPreflightReport(FString& OutError) const
{
	if (!SceneControlPreflight.bRan)
	{
		OutError = TEXT("Cannot write a scene-control preflight report before the scan has run.");
		return false;
	}

	const auto ToJsonArray = [](const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	};
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 1);
	Root->SetStringField(TEXT("pluginVersion"), TEXT("0.12.0"));
	Root->SetBoolField(TEXT("ran"), SceneControlPreflight.bRan);
	Root->SetBoolField(TEXT("required"), ActiveJob.bRequireSceneControlPreflight);
	Root->SetBoolField(TEXT("passed"), SceneControlPreflight.bPassed);
	Root->SetStringField(TEXT("sha1"), SceneControlPreflight.Sha1);
	Root->SetStringField(
		TEXT("hashScope"),
		TEXT("schema_required_sorted_allowlist_rules_and_classified_tick_niagara_di_material_input_records"));

	TSharedRef<FJsonObject> Allowlists = MakeShared<FJsonObject>();
	Allowlists->SetArrayField(
		TEXT("tickingActorClassPaths"),
		ToJsonArray(SceneControlPreflight.AllowedTickingActorClassPaths));
	Allowlists->SetArrayField(
		TEXT("tickingComponentClassPaths"),
		ToJsonArray(SceneControlPreflight.AllowedTickingComponentClassPaths));
	Allowlists->SetArrayField(
		TEXT("niagaraDataInterfaceClassPaths"),
		ToJsonArray(SceneControlPreflight.AllowedNiagaraDataInterfaceClassPaths));
	Allowlists->SetArrayField(
		TEXT("materialExpressionClassPaths"),
		ToJsonArray(SceneControlPreflight.AllowedMaterialExpressionClassPaths));
	Root->SetObjectField(TEXT("allowlists"), Allowlists);

	TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("controlledTickingActors"), SceneControlPreflight.ControlledTickingActors.Num());
	Counts->SetNumberField(TEXT("allowedTickingActors"), SceneControlPreflight.AllowedTickingActors.Num());
	Counts->SetNumberField(TEXT("uncontrolledTickingActors"), SceneControlPreflight.UncontrolledTickingActors.Num());
	Counts->SetNumberField(TEXT("controlledTickingComponents"), SceneControlPreflight.ControlledTickingComponents.Num());
	Counts->SetNumberField(TEXT("allowedTickingComponents"), SceneControlPreflight.AllowedTickingComponents.Num());
	Counts->SetNumberField(TEXT("uncontrolledTickingComponents"), SceneControlPreflight.UncontrolledTickingComponents.Num());
	Counts->SetNumberField(TEXT("niagaraDataInterfaces"), SceneControlPreflight.NiagaraDataInterfaces.Num());
	Counts->SetNumberField(TEXT("allowedNiagaraDataInterfaces"), SceneControlPreflight.AllowedNiagaraDataInterfaces.Num());
	Counts->SetNumberField(TEXT("uncontrolledNiagaraDataInterfaces"), SceneControlPreflight.UncontrolledNiagaraDataInterfaces.Num());
	Counts->SetNumberField(TEXT("controlledMaterialInputs"), SceneControlPreflight.ControlledMaterialInputs.Num());
	Counts->SetNumberField(TEXT("allowedMaterialInputs"), SceneControlPreflight.AllowedMaterialInputs.Num());
	Counts->SetNumberField(TEXT("uncontrolledMaterialInputs"), SceneControlPreflight.UncontrolledMaterialInputs.Num());
	Root->SetObjectField(TEXT("counts"), Counts);

	Root->SetArrayField(TEXT("controlledTickingActors"), ToJsonArray(SceneControlPreflight.ControlledTickingActors));
	Root->SetArrayField(TEXT("allowedTickingActors"), ToJsonArray(SceneControlPreflight.AllowedTickingActors));
	Root->SetArrayField(TEXT("uncontrolledTickingActors"), ToJsonArray(SceneControlPreflight.UncontrolledTickingActors));
	Root->SetArrayField(TEXT("controlledTickingComponents"), ToJsonArray(SceneControlPreflight.ControlledTickingComponents));
	Root->SetArrayField(TEXT("allowedTickingComponents"), ToJsonArray(SceneControlPreflight.AllowedTickingComponents));
	Root->SetArrayField(TEXT("uncontrolledTickingComponents"), ToJsonArray(SceneControlPreflight.UncontrolledTickingComponents));
	Root->SetArrayField(TEXT("niagaraDataInterfaces"), ToJsonArray(SceneControlPreflight.NiagaraDataInterfaces));
	Root->SetArrayField(TEXT("allowedNiagaraDataInterfaces"), ToJsonArray(SceneControlPreflight.AllowedNiagaraDataInterfaces));
	Root->SetArrayField(TEXT("uncontrolledNiagaraDataInterfaces"), ToJsonArray(SceneControlPreflight.UncontrolledNiagaraDataInterfaces));
	Root->SetArrayField(TEXT("controlledMaterialInputs"), ToJsonArray(SceneControlPreflight.ControlledMaterialInputs));
	Root->SetArrayField(TEXT("allowedMaterialInputs"), ToJsonArray(SceneControlPreflight.AllowedMaterialInputs));
	Root->SetArrayField(TEXT("uncontrolledMaterialInputs"), ToJsonArray(SceneControlPreflight.UncontrolledMaterialInputs));

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Could not serialize the scene-control preflight report.");
		return false;
	}
	const FString ReportPath = FPaths::Combine(ResolvedOutputDirectory, TEXT("scene_control_preflight.json"));
	const FString TempPath = ReportPath + TEXT(".part");
	if (!FFileHelper::SaveStringToFile(Json, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
		!IFileManager::Get().Move(*ReportPath, *TempPath, true, true, false, true))
	{
		OutError = FString::Printf(TEXT("Could not write scene-control preflight report: %s"), *ReportPath);
		return false;
	}
	return true;
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
		if (IsPastEvaluationRange(Status.CurrentFrame))
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

	int32 EvaluationFrame = Status.State == ESRDatasetCaptureState::WarmingUp
		? GetInitialEvaluationFrame()
		: Status.CurrentFrame;
	WarmupPoseCacheFrame = INDEX_NONE;
	if (Status.State == ESRDatasetCaptureState::WarmingUp &&
		ActiveJob.bCacheSkeletalAnimationPosesForReplay)
	{
		const int32 PoseFrameCount = ActiveJob.EndFrame - ActiveJob.StartFrame + 1;
		const int32 CompletedWarmupFrames = ActiveJob.WarmupFrames - WarmupFramesRemaining;
		const int32 FirstPoseCacheWarmupIndex = ActiveJob.WarmupFrames - PoseFrameCount;
		if (CompletedWarmupFrames >= FirstPoseCacheWarmupIndex)
		{
			WarmupPoseCacheFrame = ActiveJob.StartFrame +
				(CompletedWarmupFrames - FirstPoseCacheWarmupIndex);
			EvaluationFrame = WarmupPoseCacheFrame;
		}
	}
	EnforceDeterministicCamera(EvaluationFrame);
	ApplyLogicalTemporalJitter(EvaluationFrame);
	FString Error;
	if (!EvaluateSequence(EvaluationFrame, Error))
	{
		FinishCapture(ESRDatasetCaptureState::Failed, Error);
		return;
	}
	// Match Niagara's float fixed-tick lattice exactly. Casting the double product
	// (for example 15 / 30) to 0.5 and dividing by float(1 / 30) floors to 14
	// ticks in UNiagaraComponent::TickComponent due to representation error.
	const float FixedDeltaSeconds = static_cast<float>(ActiveJob.GetFixedDeltaSeconds());
	const float TimeSeconds = static_cast<float>(EvaluationFrame) * FixedDeltaSeconds;
	ApplyLogicalMaterialTime(EvaluationFrame);
	float NiagaraTimeSeconds = TimeSeconds;
	if (Status.State == ESRDatasetCaptureState::WarmingUp && ActiveJob.WarmupFrames > 0)
	{
		// A non-zero initial logical frame can require many absolute Niagara
		// steps before the first sample. Spread those steps across the existing
		// render warmup so renderer resources observe progressive finalized states
		// instead of receiving their first populated payload after one large jump.
		const int32 FinalTargetTicks = FMath::Max(
			0,
			FMath::RoundToInt(TimeSeconds / FixedDeltaSeconds));
		const int32 CompletedWarmupFrames =
			FMath::Clamp(ActiveJob.WarmupFrames - WarmupFramesRemaining + 1, 1, ActiveJob.WarmupFrames);
		const int32 WarmupTargetTicks = static_cast<int32>(
			(static_cast<int64>(FinalTargetTicks) * CompletedWarmupFrames + ActiveJob.WarmupFrames - 1) /
			ActiveJob.WarmupFrames);
		NiagaraTimeSeconds = static_cast<float>(WarmupTargetTicks) * FixedDeltaSeconds;
	}
	DiscoverAndControlNiagara(NiagaraTimeSeconds);
	NotifyControllablesEvaluate(EvaluationFrame, TimeSeconds);
}

void USRDatasetCaptureSubsystem::HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World != GetWorld() || !SRDataset::Private::IsRunningState(Status.State))
	{
		return;
	}
	PositionNonFixtureSkeletalValidationActor();
	PositionProjectAnimatedMaterialValidationReceiver();
	FString WidgetPolicyError;
	if (!CheckWidgetComponentPolicy(WidgetPolicyError))
	{
		FinishCapture(ESRDatasetCaptureState::Failed, WidgetPolicyError);
		return;
	}
	if (Status.State == ESRDatasetCaptureState::WarmingUp)
	{
		if (WarmupPoseCacheFrame != INDEX_NONE && ActiveJob.SkeletalPoseCacheInputFile.IsEmpty())
		{
			CacheSkeletalPosesForLogicalFrame(WarmupPoseCacheFrame);
		}
		return;
	}
	if (!ShouldCaptureFrame(Status.CurrentFrame))
	{
		return;
	}

	// Endpoint previous transforms must be installed after actor/animation ticks
	// but before UWorld's end-of-frame render-data batch. Installing them at
	// capture time is too late once an intermediate maintenance view has caused
	// GPU Scene and skeletal buffers to consume the current transform.
	ApplyCachedSkeletalPoses(Status.CurrentFrame);
	ApplyLastCapturedEndpointTransforms();
}

void USRDatasetCaptureSubsystem::HandleWorldTickEnd(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (World != GetWorld() || !SRDataset::Private::IsRunningState(Status.State))
	{
		return;
	}
	FinalizeNiagaraForCapture();

	if (Status.State == ESRDatasetCaptureState::WarmingUp)
	{
		FString Error;
		if (!UpdateCaptureCamera(true, Error))
		{
			FinishCapture(ESRDatasetCaptureState::Failed, Error);
			return;
		}
		CaptureRig->WarmupRenderState(ActiveJob);
		TArray<UPrimitiveComponent*> RendererPrimeComponents;
		RendererPrimeComponents.Reserve(NiagaraComponentStates.Num());
		for (const TPair<TWeakObjectPtr<UNiagaraComponent>, FNiagaraComponentState>& Pair : NiagaraComponentStates)
		{
			if (UNiagaraComponent* Component = Pair.Key.Get())
			{
				RendererPrimeComponents.Add(Component);
			}
		}
		if (!RendererPrimeComponents.IsEmpty())
		{
			CaptureRig->PrimeRendererState(RendererPrimeComponents);
		}
		--WarmupFramesRemaining;
		if (WarmupFramesRemaining <= 0)
		{
			if (!SaveSkeletalPoseCacheArtifact(Error))
			{
				FinishCapture(ESRDatasetCaptureState::Failed, Error);
				return;
			}
			Status.State = ESRDatasetCaptureState::Capturing;
			Status.CurrentFrame = GetInitialEvaluationFrame();
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
	if (ActiveJob.bAssignStableInstanceIds && !bStableInstanceIdsPrepared)
	{
		FString Error;
		if (!PrepareStableInstanceIds(Error))
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
	else if (ActiveJob.bSuppressMainViewOnUncapturedFrames)
	{
		// Keep renderer-side resources (notably newly populated Niagara renderer
		// payloads) alive without advancing the player Main View's temporal history.
		TArray<UPrimitiveComponent*> RendererPrimeComponents;
		RendererPrimeComponents.Reserve(NiagaraComponentStates.Num());
		for (const TPair<TWeakObjectPtr<UNiagaraComponent>, FNiagaraComponentState>& Pair : NiagaraComponentStates)
		{
			if (UNiagaraComponent* Component = Pair.Key.Get())
			{
				RendererPrimeComponents.Add(Component);
			}
		}
		if (!RendererPrimeComponents.IsEmpty())
		{
			CaptureRig->PrimeRendererState(RendererPrimeComponents);
		}
	}

	Status.CurrentFrame += GetEvaluationDirection();
	if (IsPastEvaluationRange(Status.CurrentFrame) && !bMainViewCapturePending)
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
	FMovieSceneSequencePlaybackParams Params(
		TimeSeconds,
		IsReverseEndpointReplay() ? EUpdatePositionMethod::Jump : EUpdatePositionMethod::Play);
	Params.bHasJumped = IsReverseEndpointReplay();
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

		const bool bNewlyControlled = !NiagaraComponentStates.Contains(Component);
		if (bNewlyControlled)
		{
			FNiagaraComponentState State;
			State.AgeUpdateMode = static_cast<uint8>(Component->GetAgeUpdateMode());
			State.RandomSeedOffset = Component->GetRandomSeedOffset();
			State.SeekDelta = Component->GetSeekDelta();
			State.MaxSimTime = Component->GetMaxSimTime();
			State.bForceSolo = Component->GetForceSolo();
			State.bLockSeekDelta = Component->GetLockDesiredAgeDeltaTimeToSeekDelta();
			SRDataset::Private::ReadReflectedValue<FBoolProperty>(
				Component, TEXT("bCanRenderWhileSeeking"), State.bCanRenderWhileSeeking);
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
					for (const FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
					{
						if (const FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
						{
							FNiagaraSystemState::FEmitterState EmitterState;
							EmitterState.HandleId = EmitterHandle.GetId();
							EmitterState.bDeterminism = EmitterData->bDeterminism;
							EmitterState.RandomSeed = EmitterData->RandomSeed;
							SystemState.Emitters.Add(EmitterState);
						}
					}
					NiagaraSystemStates.Add(System, SystemState);
				}
				if (ActiveJob.bForceNiagaraDeterminism)
				{
					SRDataset::Private::WriteReflectedValue<FBoolProperty>(System, TEXT("bDeterminism"), true);
					SRDataset::Private::WriteReflectedValue<FBoolProperty>(System, TEXT("bFixedTickDelta"), true);
					SRDataset::Private::WriteReflectedValue<FIntProperty>(System, TEXT("RandomSeed"), ActiveJob.RandomSeed);
					SRDataset::Private::WriteReflectedValue<FFloatProperty>(System, TEXT("FixedTickDeltaTime"), static_cast<float>(ActiveJob.GetFixedDeltaSeconds()));
					for (FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
					{
						if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
						{
							EmitterData->bDeterminism = true;
							EmitterData->RandomSeed = static_cast<int32>(HashCombineFast(
								static_cast<uint32>(ActiveJob.RandomSeed),
								GetTypeHash(EmitterHandle.GetId())));
						}
					}
				}
			}

			Component->SetForceSolo(true);
			Component->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
			Component->SetRandomSeedOffset(ActiveJob.RandomSeed ^ static_cast<int32>(GetTypeHash(Component->GetPathName())));
			Component->SetSeekDelta(static_cast<float>(ActiveJob.GetFixedDeltaSeconds()));
			Component->SetLockDesiredAgeDeltaTimeToSeekDelta(true);
			Component->SetCanRenderWhileSeeking(true);
			Component->SetMaxSimTime(60.0f);
			// Emitter determinism is cached into FNiagaraEmitterInstance at
			// construction time. ResetSystem only re-activates the existing
			// instance, so rebuild it after applying asset/component controls.
			Component->ReinitializeSystem();
		}

		const float FixedDeltaSeconds = static_cast<float>(ActiveJob.GetFixedDeltaSeconds());
		const int32 TargetTickCount = FMath::Max(
			0,
			FMath::RoundToInt(TimeSeconds / FixedDeltaSeconds));
		FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
		if (!Controller.IsValid())
		{
			continue;
		}

		int32 CurrentTickCount = FMath::Max(
			0,
			FMath::RoundToInt(Controller->GetAge() / FixedDeltaSeconds));
		if (!bNewlyControlled && CurrentTickCount > TargetTickCount)
		{
			// An absolute rewind must start from the deterministic initial state;
			// merely setting DesiredAge to zero leaves the existing particle data
			// alive because Niagara does not execute a simulation tick at age zero.
			Component->ReinitializeSystem();
			Controller = Component->GetSystemInstanceController();
			CurrentTickCount = 0;
		}
		const int32 TicksToAdvance = TargetTickCount - CurrentTickCount;
		const bool bUseNormalWarmupTick =
			Status.State == ESRDatasetCaptureState::WarmingUp && TicksToAdvance == 1;
		if (Controller.IsValid() && TicksToAdvance > 0 && !bUseNormalWarmupTick)
		{
			// Do not use DesiredAge's MultiTick catch-up for dataset replay. In UE
			// 5.7 MultiTick exposes the entire seek duration as Engine.DeltaTime to
			// every internal substep, so systems that read Engine.DeltaTime do not
			// match ordinary fixed-step playback. AdvanceSimulation executes the
			// same one-tick path repeatedly and therefore preserves per-step inputs.
			Component->AdvanceSimulation(TicksToAdvance, FixedDeltaSeconds);
			// ManualTick can still leave emitter work awaiting its GT finalize path.
			// Complete it before the component's regular DesiredAge tick observes the
			// new age later in this world tick.
			Controller->WaitForConcurrentTickAndFinalize(true);
		}
		const float TargetAgeSeconds = static_cast<float>(TargetTickCount) * FixedDeltaSeconds;
		Component->SetDesiredAge(
			bUseNormalWarmupTick
				? std::nextafter(TargetAgeSeconds, std::numeric_limits<float>::infinity())
				: TargetAgeSeconds);
	}
}

void USRDatasetCaptureSubsystem::FinalizeNiagaraForCapture()
{
	if (!ActiveJob.bControlNiagara)
	{
		return;
	}
	// UWorld starts its normal end-of-frame component updates before
	// OnWorldPostActorTick. Complete that batch first: it can contain Niagara's
	// concurrent simulation finalization and dynamic-render-data submission.
	GetWorld()->FinishAsyncSendAllEndOfFrameUpdates();

	bool bFinalizedNiagara = false;
	for (const TPair<TWeakObjectPtr<UNiagaraComponent>, FNiagaraComponentState>& Pair : NiagaraComponentStates)
	{
		if (UNiagaraComponent* Component = Pair.Key.Get())
		{
			if (FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController())
			{
				Controller->WaitForConcurrentTickAndFinalize(true);
				// Suppressed player views do not refresh primitive visibility timestamps.
				// Keep controlled FX renderer-resident without rendering an intermediate
				// scene, which would consume opaque/skeletal endpoint motion history.
				const float WorldTimeSeconds = static_cast<float>(GetWorld()->GetTimeSeconds());
				Component->SetLastRenderTime(WorldTimeSeconds);
				Controller->SetLastRenderTime(WorldTimeSeconds);
				Component->MarkRenderDynamicDataDirty();
				Component->DoDeferredRenderUpdates_Concurrent();
				bFinalizedNiagara = true;
			}
		}
	}
	if (bFinalizedNiagara)
	{
		// Niagara dynamic data uses its own render-command pipe in UE 5.7. A
		// SceneCapture enqueued immediately afterwards is therefore not ordered
		// against that pipe unless we establish an explicit barrier. This flush
		// publishes the finalized particle payload and bounds without advancing
		// world time, simulation age, or temporal history.
		FlushRenderingCommands();
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
			for (FNiagaraEmitterHandle& EmitterHandle : System->GetEmitterHandles())
			{
				const FNiagaraSystemState::FEmitterState* EmitterState = State.Emitters.FindByPredicate(
					[&EmitterHandle](const FNiagaraSystemState::FEmitterState& Candidate)
					{
						return Candidate.HandleId == EmitterHandle.GetId();
					});
				if (EmitterState)
				{
					if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
					{
						EmitterData->bDeterminism = EmitterState->bDeterminism;
						EmitterData->RandomSeed = EmitterState->RandomSeed;
					}
				}
			}
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
			Component->SetCanRenderWhileSeeking(State.bCanRenderWhileSeeking);
			Component->SetForceSolo(State.bForceSolo);
			Component->ReinitializeSystem();
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

bool USRDatasetCaptureSubsystem::UpdateCaptureCamera(
	const bool bEvaluateValidationFixture,
	FString& OutError)
{
	FMinimalViewInfo View;
	bool bFoundCamera = false;
	if (ActiveJob.bUseDeterministicCameraTransform && DeterministicCameraActor)
	{
		const int32 CameraLogicalFrame =
			Status.State == ESRDatasetCaptureState::WarmingUp
				? (WarmupPoseCacheFrame != INDEX_NONE ? WarmupPoseCacheFrame : GetInitialEvaluationFrame())
				: Status.CurrentFrame;
		EnforceDeterministicCamera(CameraLogicalFrame);
		if (UCameraComponent* Camera = DeterministicCameraActor->GetCameraComponent())
		{
			Camera->GetCameraView(0.0f, View);
			bFoundCamera = true;
		}
	}
	if (!bFoundCamera && SequencePlayer)
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
	if (bEvaluateValidationFixture && ValidationFixture)
	{
		ValidationFixture->Evaluate(
			View,
			Status.CurrentFrame,
			ActiveJob.StartFrame,
			ActiveJob.HRResolution,
			ActiveJob.bUseLastCapturedEndpointTransforms,
			ActiveJob.SemanticMotionScenario);
	}
	return true;
}

bool USRDatasetCaptureSubsystem::CaptureCurrentFrame(FString& OutError)
{
	if (!UpdateCaptureCamera(true, OutError))
	{
		return false;
	}
	if (!ValidateStableInstanceIds(OutError))
	{
		return false;
	}
	if (!ValidateControllableStates(OutError))
	{
		return false;
	}

	const int32 FrameNumber = Status.CurrentFrame;
	const int32 FirstCapturedFrame = GetFirstCapturedFrame();
	const bool bHistoryReset = FrameNumber == FirstCapturedFrame;
	const int32 MotionPreviousLogicalFrameId =
		ActiveJob.ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate
			? FrameNumber - 1
			: (bHistoryReset ? FrameNumber : GetPreviouslyCapturedFrame(FrameNumber));
	// Re-apply after the world's normal deferred component batch. This second
	// application is required by double-buffered skeletal meshes; SceneCapture's
	// flush then selects the explicitly restored previous bone buffer.
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
		if (ActiveJob.bCaptureSceneCaptureLRComparison)
		{
			Hashes.Add(
				SRDataset::Private::SceneCaptureLRComparisonModality,
				HashFile(MakeFramePath(
					SRDataset::Private::SceneCaptureLRComparisonModality,
					FrameNumber,
					TEXT("exr"))));
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
		if (ActiveJob.bCaptureUIColorAlpha)
		{
			Hashes.Add(
				SRDataset::Private::UIColorAlphaModality,
				HashFile(MakeFramePath(SRDataset::Private::UIColorAlphaModality, FrameNumber, TEXT("png"))));
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
		if (ActiveJob.bCaptureUIColorAlpha)
		{
			RenderSubmissions.Add(TEXT("ui_layer"), NextRenderSubmissionId++);
			if (!CaptureRig->CaptureUIColorAlpha(
				ActiveJob,
				MakeFramePath(SRDataset::Private::UIColorAlphaModality, FrameNumber, TEXT("png")),
				Hashes,
				OutError))
			{
				return false;
			}
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
	AppliedEndpointBoneComponentCount = 0;
	AppliedEndpointBoneCount = 0;
	SkippedEndpointBoneComponents.Reset();
	if (!ActiveJob.bUseLastCapturedEndpointTransforms)
	{
		return;
	}
	for (const TPair<TWeakObjectPtr<USceneComponent>, FTransform>& Pair : LastCapturedEndpointTransforms)
	{
		if (USceneComponent* Component = Pair.Key.Get())
		{
			FMotionVectorSimulation::Get().SetPreviousTransform(Component, Pair.Value);
			Component->MarkRenderTransformDirty();
		}
	}

	for (const TPair<TWeakObjectPtr<USkinnedMeshComponent>, FSkeletalEndpointState>& Pair :
		LastCapturedEndpointBoneStates)
	{
		const FSkeletalEndpointState& State = Pair.Value;
		USkinnedMeshComponent* Component = Pair.Key.Get();
		if (!IsValid(Component) || !Component->IsRegistered())
		{
			SkippedEndpointBoneComponents.Add(State.ComponentPath + TEXT("|unavailable"));
			continue;
		}
		const FString CurrentAssetPath = Component->GetSkinnedAsset()
			? Component->GetSkinnedAsset()->GetPathName()
			: TEXT("none");
		if (CurrentAssetPath != State.SkinnedAssetPath)
		{
			SkippedEndpointBoneComponents.Add(State.ComponentPath + TEXT("|skinned_asset_changed"));
			continue;
		}
		TArray<FTransform>& EditablePreviousTransforms = Component->GetEditableComponentSpaceTransforms();
		const bool bUsesDoubleBufferedTransforms =
			&Component->GetPreviousComponentTransformsArray() == &EditablePreviousTransforms;
		if (!bUsesDoubleBufferedTransforms)
		{
			SkippedEndpointBoneComponents.Add(State.ComponentPath + TEXT("|not_double_buffered"));
			continue;
		}
		if (State.ComponentSpaceTransforms.IsEmpty() ||
			EditablePreviousTransforms.Num() != State.ComponentSpaceTransforms.Num())
		{
			SkippedEndpointBoneComponents.Add(State.ComponentPath + TEXT("|bone_count_changed"));
			continue;
		}

		EditablePreviousTransforms = State.ComponentSpaceTransforms;
		TArray<uint8>& EditablePreviousVisibility = Component->GetEditableBoneVisibilityStates();
		if (EditablePreviousVisibility.Num() == State.BoneVisibilityStates.Num())
		{
			EditablePreviousVisibility = State.BoneVisibilityStates;
		}
		else if (!State.BoneVisibilityStates.IsEmpty())
		{
			SkippedEndpointBoneComponents.Add(State.ComponentPath + TEXT("|visibility_count_changed"));
			continue;
		}

		// Two queued force entries make UE select UpdatePrevious rather than
		// reusing the last GPU skin buffer. SceneCapture flushes the world's
		// deferred component updates before it enqueues the capture render.
		Component->ForceMotionVector();
		Component->ForceMotionVector();
		++AppliedEndpointBoneComponentCount;
		AppliedEndpointBoneCount += State.ComponentSpaceTransforms.Num();
	}
	SkippedEndpointBoneComponents.Sort();
}

void USRDatasetCaptureSubsystem::SnapshotCapturedEndpointTransforms()
{
	if (!ActiveJob.bUseLastCapturedEndpointTransforms || !GetWorld())
	{
		return;
	}
	LastCapturedEndpointTransforms.Reset();
	LastCapturedEndpointBoneStates.Reset();
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		TInlineComponentArray<USceneComponent*> Components;
		It->GetComponents(Components);
		for (USceneComponent* Component : Components)
		{
			if (IsValid(Component) && Component->IsRegistered())
			{
				LastCapturedEndpointTransforms.Add(Component, Component->GetComponentTransform());
				if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Component))
				{
					const TArray<FTransform>& CurrentBones = Skinned->GetComponentSpaceTransforms();
					if (Skinned->GetSkinnedAsset() && !CurrentBones.IsEmpty())
					{
						FSkeletalEndpointState State;
						State.ComponentPath = Skinned->GetPathName();
						State.SkinnedAssetPath = Skinned->GetSkinnedAsset()->GetPathName();
						State.ComponentSpaceTransforms = CurrentBones;
						State.BoneVisibilityStates = Skinned->GetBoneVisibilityStates();
						LastCapturedEndpointBoneStates.Add(Skinned, MoveTemp(State));
					}
				}
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
	const int32 FirstCapturedFrame = GetFirstCapturedFrame();
	const bool bHistoryReset = PendingMainViewFrameNumber == FirstCapturedFrame;
	const int32 MotionPreviousLogicalFrameId =
		ActiveJob.ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate
			? PendingMainViewFrameNumber - 1
			: (bHistoryReset
				? PendingMainViewFrameNumber
				: GetPreviouslyCapturedFrame(PendingMainViewFrameNumber));
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
	if (ActiveJob.bCaptureSceneCaptureLRComparison &&
		!IFileManager::Get().FileExists(*MakeFramePath(
			SRDataset::Private::SceneCaptureLRComparisonModality, FrameNumber, TEXT("exr"))))
	{
		return false;
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
	if (ActiveJob.bCaptureUIColorAlpha &&
		!IFileManager::Get().FileExists(*MakeFramePath(
			SRDataset::Private::UIColorAlphaModality, FrameNumber, TEXT("png"))))
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
	const int32 FirstCapturedFrame = GetFirstCapturedFrame();
	const bool bFirstCapturedFrame = FrameNumber == FirstCapturedFrame;
	const int32 PreviousCapturedFrame = bFirstCapturedFrame
		? FrameNumber
		: GetPreviouslyCapturedFrame(FrameNumber);
	const bool bIntermediateReplay = ActiveJob.ReplayPass == ESRDatasetReplayPass::FrameGenerationIntermediate;
	const int32 MotionPreviousFrame = bIntermediateReplay ? FrameNumber - 1 : PreviousCapturedFrame;
	const int32 MotionTimeSpanFrames = bFirstCapturedFrame && !bIntermediateReplay
		? 0
		: FMath::Abs(FrameNumber - MotionPreviousFrame);
	const int32 MaterialPreviousFrame = bFirstCapturedFrame && !bIntermediateReplay
		? FrameNumber - GetEvaluationDirection()
		: MotionPreviousFrame;
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
	Frame->SetBoolField(
		TEXT("materialTimeLogicalFrameLocked"),
		ActiveJob.bLockMaterialTimeToLogicalFrame);
	Frame->SetNumberField(TEXT("materialTimeSeconds"), TimeSeconds);
	Frame->SetNumberField(
		TEXT("materialPreviousLogicalFrameId"),
		MaterialPreviousFrame);
	Frame->SetNumberField(
		TEXT("materialPreviousTimeSeconds"),
		static_cast<double>(MaterialPreviousFrame) * ActiveJob.GetFixedDeltaSeconds());
	Frame->SetNumberField(
		TEXT("materialDeltaTimeSeconds"),
		static_cast<double>(FrameNumber - MaterialPreviousFrame) * ActiveJob.GetFixedDeltaSeconds());
	Frame->SetBoolField(TEXT("endpointPreviousTransformOverride"), ActiveJob.bUseLastCapturedEndpointTransforms);
	Frame->SetBoolField(
		TEXT("endpointPreviousSkeletalBoneOverride"),
		AppliedEndpointBoneComponentCount > 0);
	Frame->SetNumberField(
		TEXT("endpointPreviousSkeletalBoneComponentCount"),
		AppliedEndpointBoneComponentCount);
	Frame->SetNumberField(TEXT("endpointPreviousSkeletalBoneCount"), AppliedEndpointBoneCount);
	TArray<TSharedPtr<FJsonValue>> SkippedBoneComponentsJson;
	SkippedBoneComponentsJson.Reserve(SkippedEndpointBoneComponents.Num());
	for (const FString& Value : SkippedEndpointBoneComponents)
	{
		SkippedBoneComponentsJson.Add(MakeShared<FJsonValueString>(Value));
	}
	Frame->SetArrayField(
		TEXT("endpointPreviousSkeletalBoneSkippedComponents"),
		SkippedBoneComponentsJson);
	Frame->SetStringField(
		TEXT("auxiliaryCaptureOrder"),
		StaticEnum<ESRDatasetAuxiliaryCaptureOrder>()->GetNameStringByValue(
			static_cast<int64>(ActiveJob.AuxiliaryCaptureOrder)));
	Frame->SetNumberField(TEXT("simulationTick"), FrameNumber);
	Frame->SetNumberField(TEXT("simulationTimeS"), TimeSeconds);
	Frame->SetNumberField(TEXT("deltaTimeS"), ActiveJob.GetFixedDeltaSeconds());
	Frame->SetBoolField(TEXT("simulationAdvance"), true);
	Frame->SetBoolField(TEXT("historyAdvance"), true);
	Frame->SetStringField(
		TEXT("logicalEvaluationDirection"),
		IsReverseEndpointReplay() ? TEXT("decreasing_frame_id") : TEXT("increasing_frame_id"));
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
	Frame->SetBoolField(TEXT("stableInstanceIdsEnabled"), ActiveJob.bAssignStableInstanceIds);
	Frame->SetStringField(
		TEXT("stableInstanceIdMappingSha1"),
		ActiveJob.bAssignStableInstanceIds ? StableInstanceIdMappingSha1 : TEXT("not_used"));
	Frame->SetNumberField(
		TEXT("stableInstanceIdCount"),
		ActiveJob.bAssignStableInstanceIds ? StableInstanceIdRecords.Num() : 0);
	Frame->SetNumberField(TEXT("sceneActorCount"), SceneState.ActorCount);
	Frame->SetNumberField(TEXT("sceneComponentCount"), SceneState.ComponentCount);
	Frame->SetNumberField(TEXT("sceneSkeletalComponentCount"), SceneState.SkeletalComponentCount);
	Frame->SetNumberField(TEXT("sceneBoneCount"), SceneState.BoneCount);
	Frame->SetNumberField(TEXT("sceneFXComponentCount"), SceneState.FXComponentCount);
	Frame->SetNumberField(TEXT("sceneNiagaraComponentCount"), SceneState.NiagaraComponentCount);
	Frame->SetNumberField(TEXT("sceneNiagaraEmitterCount"), SceneState.NiagaraEmitterCount);
	Frame->SetNumberField(TEXT("sceneNiagaraCPUEmitterCount"), SceneState.NiagaraCPUEmitterCount);
	Frame->SetNumberField(TEXT("sceneNiagaraGPUEmitterCount"), SceneState.NiagaraGPUEmitterCount);
	Frame->SetNumberField(TEXT("sceneNiagaraParticleCount"), SceneState.NiagaraParticleCount);
	Frame->SetNumberField(
		TEXT("sceneNiagaraTotalSpawnedParticleCount"),
		SceneState.NiagaraTotalSpawnedParticleCount);
	Frame->SetNumberField(TEXT("sceneControllableActorCount"), SceneState.ControllableActorCount);
	Frame->SetNumberField(TEXT("sceneControllableStateCount"), SceneState.ControllableStateCount);
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
		TEXT("sceneControllableActorsWithoutState"),
		StringArray(SceneState.ControllableActorsWithoutState));
	TArray<TSharedPtr<FJsonValue>> ControllableStateJson;
	ControllableStateJson.Reserve(SceneState.ControllableStates.Num());
	for (const FSceneStateSummary::FControllableStateSummary& ControllableState :
		SceneState.ControllableStates)
	{
		TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("actorPath"), ControllableState.ActorPath);
		State->SetStringField(TEXT("stateSha1"), ControllableState.StateSha1);
		State->SetNumberField(TEXT("utf8Bytes"), ControllableState.Utf8Bytes);
		ControllableStateJson.Add(MakeShared<FJsonValueObject>(State));
	}
	Frame->SetArrayField(TEXT("sceneControllableStates"), ControllableStateJson);
	Frame->SetArrayField(
		TEXT("sceneUncontrolledTickingActors"),
		StringArray(SceneState.UncontrolledTickingActors));
	TArray<TSharedPtr<FJsonValue>> NiagaraStateJson;
	NiagaraStateJson.Reserve(SceneState.NiagaraComponents.Num());
	for (const FSceneStateSummary::FNiagaraSummary& Niagara : SceneState.NiagaraComponents)
	{
		TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("componentPath"), Niagara.ComponentPath);
		State->SetStringField(TEXT("assetPath"), Niagara.AssetPath);
		State->SetNumberField(TEXT("desiredAgeS"), Niagara.DesiredAgeSeconds);
		State->SetNumberField(TEXT("simulationAgeS"), Niagara.SimulationAgeSeconds);
		State->SetNumberField(TEXT("emitterCount"), Niagara.EmitterCount);
		State->SetNumberField(TEXT("cpuEmitterCount"), Niagara.CPUEmitterCount);
		State->SetNumberField(TEXT("gpuEmitterCount"), Niagara.GPUEmitterCount);
		State->SetNumberField(TEXT("particleCount"), Niagara.ParticleCount);
		State->SetNumberField(TEXT("totalSpawnedParticleCount"), Niagara.TotalSpawnedParticleCount);
		State->SetNumberField(TEXT("rendererCount"), Niagara.RendererCount);
		State->SetBoolField(TEXT("soloInstanceObservable"), Niagara.bSoloInstanceObservable);
		State->SetBoolField(TEXT("systemDeterminism"), Niagara.bSystemDeterminism);
		State->SetBoolField(TEXT("systemFixedTick"), Niagara.bSystemFixedTick);
		State->SetNumberField(TEXT("systemFixedTickS"), Niagara.SystemFixedTickSeconds);
		const auto VectorArray = [](const FVector& Value)
		{
			return TArray<TSharedPtr<FJsonValue>> {
				MakeShared<FJsonValueNumber>(Value.X),
				MakeShared<FJsonValueNumber>(Value.Y),
				MakeShared<FJsonValueNumber>(Value.Z) };
		};
		State->SetArrayField(TEXT("componentLocationCm"), VectorArray(Niagara.ComponentLocationCm));
		State->SetArrayField(TEXT("componentBoundsOriginCm"), VectorArray(Niagara.ComponentBoundsOriginCm));
		State->SetArrayField(TEXT("componentBoundsExtentCm"), VectorArray(Niagara.ComponentBoundsExtentCm));
		State->SetArrayField(TEXT("materialPaths"), StringArray(Niagara.MaterialPaths));
		TArray<TSharedPtr<FJsonValue>> EmitterCounts;
		EmitterCounts.Reserve(Niagara.EmitterParticleCounts.Num());
		for (const int32 Count : Niagara.EmitterParticleCounts)
		{
			EmitterCounts.Add(MakeShared<FJsonValueNumber>(Count));
		}
		State->SetArrayField(TEXT("emitterParticleCounts"), EmitterCounts);
		TArray<TSharedPtr<FJsonValue>> EmitterDeterminism;
		EmitterDeterminism.Reserve(Niagara.EmitterDeterminism.Num());
		for (const bool bDeterministic : Niagara.EmitterDeterminism)
		{
			EmitterDeterminism.Add(MakeShared<FJsonValueBoolean>(bDeterministic));
		}
		State->SetArrayField(TEXT("emitterDeterminism"), EmitterDeterminism);
		TArray<TSharedPtr<FJsonValue>> EmitterRandomSeeds;
		EmitterRandomSeeds.Reserve(Niagara.EmitterRandomSeeds.Num());
		for (const int32 RandomSeed : Niagara.EmitterRandomSeeds)
		{
			EmitterRandomSeeds.Add(MakeShared<FJsonValueNumber>(RandomSeed));
		}
		State->SetArrayField(TEXT("emitterRandomSeeds"), EmitterRandomSeeds);
		NiagaraStateJson.Add(MakeShared<FJsonValueObject>(State));
	}
	Frame->SetArrayField(TEXT("niagaraFrameStates"), NiagaraStateJson);
	Frame->SetStringField(
		TEXT("sceneStateHashScope"),
		TEXT("sorted_actor_component_transforms_visibility_tick_controllable_canonical_state_hashes_skeletal_component_space_bones_niagara_component_and_finalized_cpu_particle_counts_cascade_component_state_gpu_payload_not_read_back"));
	Frame->SetBoolField(
		TEXT("skeletalPoseCacheReplayEnabled"),
		ActiveJob.bCacheSkeletalAnimationPosesForReplay);
	Frame->SetBoolField(
		TEXT("skeletalPoseCacheApplied"),
		ActiveJob.bCacheSkeletalAnimationPosesForReplay &&
		AppliedCachedSkeletalPoseComponentCount > 0 &&
		SkippedCachedSkeletalPoseComponents.IsEmpty());
	Frame->SetNumberField(
		TEXT("skeletalPoseCacheAppliedComponentCount"),
		AppliedCachedSkeletalPoseComponentCount);
	Frame->SetNumberField(TEXT("skeletalPoseCacheAppliedBoneCount"), AppliedCachedSkeletalPoseBoneCount);
	Frame->SetArrayField(
		TEXT("skeletalPoseCacheSkippedComponents"),
		StringArray(SkippedCachedSkeletalPoseComponents));
	Frame->SetStringField(
		TEXT("skeletalPoseCacheSource"),
		bSkeletalPoseCacheLoadedFromArtifact
			? TEXT("shared_artifact")
			: (ActiveJob.bCacheSkeletalAnimationPosesForReplay
				? TEXT("forward_warmup_bake")
				: TEXT("none")));
	Frame->SetStringField(TEXT("skeletalPoseCacheArtifactSha1"), SkeletalPoseCacheArtifactSha1);
	TArray<TSharedPtr<FJsonValue>> NonFixtureSkeletalStates;
	const auto PoseSha1 = [](const TArray<FTransform>& Transforms)
	{
		TArray<FString> PoseLines;
		PoseLines.Reserve(Transforms.Num());
		for (int32 BoneIndex = 0; BoneIndex < Transforms.Num(); ++BoneIndex)
		{
			const FMatrix Matrix = Transforms[BoneIndex].ToMatrixWithScale();
			FString MatrixString;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					if (!MatrixString.IsEmpty())
					{
						MatrixString += TEXT(",");
					}
					MatrixString += FString::Printf(TEXT("%.17g"), Matrix.M[Row][Column]);
				}
			}
			PoseLines.Add(FString::Printf(TEXT("%d|%s"), BoneIndex, *MatrixString));
		}
		return HashString(FString::Join(PoseLines, TEXT("\n")));
	};
	const TMap<TWeakObjectPtr<USkinnedMeshComponent>, FSkeletalEndpointState>* CachedFrame =
		CachedSkeletalPoseFrames.Find(FrameNumber);
	for (const TPair<TWeakObjectPtr<USkinnedMeshComponent>, int32>& Pair : NonFixtureSkeletalObjectIds)
	{
		USkinnedMeshComponent* Component = Pair.Key.Get();
		if (!IsValid(Component) || !Component->GetSkinnedAsset())
		{
			continue;
		}
		const TArray<FTransform>& BoneTransforms = Component->GetComponentSpaceTransforms();
		const FSkeletalEndpointState* CachedState = CachedFrame ? CachedFrame->Find(Pair.Key) : nullptr;
		double CurrentToCachedMaxMatrixAbs = -1.0;
		if (CachedState && CachedState->ComponentSpaceTransforms.Num() == BoneTransforms.Num())
		{
			CurrentToCachedMaxMatrixAbs = 0.0;
			for (int32 BoneIndex = 0; BoneIndex < BoneTransforms.Num(); ++BoneIndex)
			{
				const FMatrix CurrentMatrix = BoneTransforms[BoneIndex].ToMatrixWithScale();
				const FMatrix CachedMatrix = CachedState->ComponentSpaceTransforms[BoneIndex].ToMatrixWithScale();
				for (int32 Row = 0; Row < 4; ++Row)
				{
					for (int32 Column = 0; Column < 4; ++Column)
					{
						CurrentToCachedMaxMatrixAbs = FMath::Max(
							CurrentToCachedMaxMatrixAbs,
							FMath::Abs(static_cast<double>(
								CurrentMatrix.M[Row][Column] - CachedMatrix.M[Row][Column])));
					}
				}
			}
		}
		TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("componentPath"), Component->GetPathName());
		State->SetStringField(
			TEXT("ownerPath"), Component->GetOwner() ? Component->GetOwner()->GetPathName() : TEXT("none"));
		State->SetStringField(
			TEXT("ownerClass"), Component->GetOwner() ? Component->GetOwner()->GetClass()->GetPathName() : TEXT("none"));
		State->SetStringField(TEXT("skinnedAssetPath"), Component->GetSkinnedAsset()->GetPathName());
		State->SetBoolField(TEXT("projectAsset"), Component->GetSkinnedAsset()->GetPathName().StartsWith(TEXT("/Game/")));
		State->SetBoolField(
			TEXT("isProjectValidationProbe"),
			Component->GetOwner() == NonFixtureSkeletalValidationActor);
		State->SetNumberField(TEXT("objectId"), Pair.Value);
		State->SetNumberField(TEXT("boneCount"), BoneTransforms.Num());
		State->SetStringField(TEXT("poseSha1"), PoseSha1(BoneTransforms));
		State->SetStringField(
			TEXT("cachedPoseSha1"),
			CachedState ? PoseSha1(CachedState->ComponentSpaceTransforms) : TEXT("none"));
		State->SetNumberField(TEXT("currentToCachedPoseMaxMatrixAbs"), CurrentToCachedMaxMatrixAbs);
		State->SetBoolField(TEXT("registered"), Component->IsRegistered());
		State->SetBoolField(TEXT("visible"), Component->IsVisible());
		State->SetArrayField(TEXT("boundsOriginCm"), {
			MakeShared<FJsonValueNumber>(Component->Bounds.Origin.X),
			MakeShared<FJsonValueNumber>(Component->Bounds.Origin.Y),
			MakeShared<FJsonValueNumber>(Component->Bounds.Origin.Z) });
		State->SetArrayField(TEXT("boundsExtentCm"), {
			MakeShared<FJsonValueNumber>(Component->Bounds.BoxExtent.X),
			MakeShared<FJsonValueNumber>(Component->Bounds.BoxExtent.Y),
			MakeShared<FJsonValueNumber>(Component->Bounds.BoxExtent.Z) });
		if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component))
		{
			State->SetStringField(
				TEXT("animationMode"),
				StaticEnum<EAnimationMode::Type>()->GetNameStringByValue(
					static_cast<int64>(Skeletal->GetAnimationMode())));
			State->SetStringField(
				TEXT("animationInstanceClass"),
				Skeletal->GetAnimInstance() ? Skeletal->GetAnimInstance()->GetClass()->GetPathName() : TEXT("none"));
		}
		else
		{
			State->SetStringField(TEXT("animationMode"), TEXT("non_skeletal_skinned_component"));
			State->SetStringField(TEXT("animationInstanceClass"), TEXT("none"));
		}
		NonFixtureSkeletalStates.Add(MakeShared<FJsonValueObject>(State));
	}
	NonFixtureSkeletalStates.Sort([](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
	{
		return Left->AsObject()->GetStringField(TEXT("componentPath")).Compare(
			Right->AsObject()->GetStringField(TEXT("componentPath")), ESearchCase::CaseSensitive) < 0;
	});
	Frame->SetArrayField(TEXT("nonFixtureSkeletalComponents"), NonFixtureSkeletalStates);
	TSharedRef<FJsonObject> AnimatedMaterial = MakeShared<FJsonObject>();
	AnimatedMaterial->SetBoolField(
		TEXT("enabled"),
		ActiveJob.bValidateProjectAnimatedMaterial);
	AnimatedMaterial->SetNumberField(TEXT("receiverObjectId"), 150);
	AnimatedMaterial->SetStringField(
		TEXT("materialInterfacePath"),
		ProjectAnimatedMaterialValidationInterface
			? ProjectAnimatedMaterialValidationInterface->GetPathName()
			: TEXT("none"));
	AnimatedMaterial->SetStringField(
		TEXT("baseMaterialPath"),
		ProjectAnimatedMaterialValidationBasePath.IsEmpty()
			? TEXT("none")
			: ProjectAnimatedMaterialValidationBasePath);
	AnimatedMaterial->SetStringField(
		TEXT("receiverComponentPath"),
		ProjectAnimatedMaterialValidationComponent
			? ProjectAnimatedMaterialValidationComponent->GetPathName()
			: TEXT("none"));
	AnimatedMaterial->SetBoolField(
		TEXT("registered"),
		ProjectAnimatedMaterialValidationComponent &&
		ProjectAnimatedMaterialValidationComponent->IsRegistered());
	AnimatedMaterial->SetBoolField(
		TEXT("visible"),
		ProjectAnimatedMaterialValidationComponent &&
		ProjectAnimatedMaterialValidationComponent->IsVisible());
	AnimatedMaterial->SetBoolField(
		TEXT("projectAuthoredInterface"),
		ProjectAnimatedMaterialValidationInterface &&
		ProjectAnimatedMaterialValidationInterface->GetPathName().StartsWith(TEXT("/Game/")));
	AnimatedMaterial->SetBoolField(
		TEXT("projectAuthoredBaseMaterial"),
		ProjectAnimatedMaterialValidationBasePath.StartsWith(TEXT("/Game/")));
	AnimatedMaterial->SetNumberField(TEXT("logicalGameTimeSeconds"), TimeSeconds);
	AnimatedMaterial->SetNumberField(
		TEXT("previousLogicalGameTimeSeconds"),
		static_cast<double>(MaterialPreviousFrame) * ActiveJob.GetFixedDeltaSeconds());
	Frame->SetObjectField(TEXT("projectAnimatedMaterialValidation"), AnimatedMaterial);
	TSharedRef<FJsonObject> WidgetPolicy = MakeShared<FJsonObject>();
	const TArray<FString> ActiveWidgetComponents = GetActiveWidgetComponentPaths();
	WidgetPolicy->SetStringField(
		TEXT("policy"),
		ActiveJob.bRejectVisibleWidgetComponents
			? TEXT("reject_visible_registered_widget_components")
			: TEXT("allow_as_scene_content"));
	WidgetPolicy->SetNumberField(
		TEXT("activeVisibleRegisteredComponentCount"),
		ActiveWidgetComponents.Num());
	WidgetPolicy->SetArrayField(
		TEXT("activeVisibleRegisteredComponentPaths"),
		StringArray(ActiveWidgetComponents));
	Frame->SetObjectField(TEXT("worldSpaceWidgetPolicy"), WidgetPolicy);

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
				: (Modality == TEXT("ui_layer")
					? TEXT("independent_slate_game_layer")
					: Modality + TEXT("_scene_capture")));
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
	if (ActiveJob.bCaptureSceneCaptureLRComparison)
	{
		Files->SetStringField(
			SRDataset::Private::SceneCaptureLRComparisonModality,
			FString::Printf(
				TEXT("%s/frame_%06d.exr"),
				SRDataset::Private::SceneCaptureLRComparisonModality,
				FrameNumber));
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
	if (ActiveJob.bCaptureUIColorAlpha)
	{
		Files->SetStringField(
			SRDataset::Private::UIColorAlphaModality,
			FString::Printf(TEXT("%s/frame_%06d.png"), SRDataset::Private::UIColorAlphaModality, FrameNumber));
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
		Fixture->SetBoolField(TEXT("analyticProjectionValid"), FixtureFrame.bValid);
		Fixture->SetBoolField(TEXT("sourceLevelGeometryHidden"), true);
		Fixture->SetStringField(
			TEXT("motionScenario"),
			StaticEnum<ESRDatasetSemanticMotionScenario>()->GetNameStringByValue(
				static_cast<int64>(FixtureFrame.MotionScenario)));
		Fixture->SetBoolField(TEXT("worldAnchored"), FixtureFrame.bWorldAnchored);
		Fixture->SetBoolField(TEXT("objectMotionEnabled"), FixtureFrame.bObjectMotionEnabled);
		Fixture->SetArrayField(TEXT("currentCameraLocationCm"), {
			MakeShared<FJsonValueNumber>(FixtureFrame.CurrentCameraLocationCm.X),
			MakeShared<FJsonValueNumber>(FixtureFrame.CurrentCameraLocationCm.Y),
			MakeShared<FJsonValueNumber>(FixtureFrame.CurrentCameraLocationCm.Z) });
		Fixture->SetArrayField(TEXT("previousCameraLocationCm"), {
			MakeShared<FJsonValueNumber>(FixtureFrame.PreviousCameraLocationCm.X),
			MakeShared<FJsonValueNumber>(FixtureFrame.PreviousCameraLocationCm.Y),
			MakeShared<FJsonValueNumber>(FixtureFrame.PreviousCameraLocationCm.Z) });
		Fixture->SetNumberField(TEXT("logicalFrameId"), FixtureFrame.LogicalFrame);
		Fixture->SetNumberField(TEXT("movingObjectId"), ASRDatasetValidationFixture::MovingObjectId);
		Fixture->SetNumberField(TEXT("backgroundObjectId"), ASRDatasetValidationFixture::BackgroundObjectId);
		Fixture->SetNumberField(TEXT("translucentObjectId"), ASRDatasetValidationFixture::TranslucentObjectId);
		Fixture->SetNumberField(TEXT("skeletalObjectId"), ASRDatasetValidationFixture::SkeletalObjectId);
		Fixture->SetNumberField(TEXT("wpoObjectId"), ASRDatasetValidationFixture::WPOObjectId);
		Fixture->SetNumberField(TEXT("movingCurrentRightCm"), FixtureFrame.MovingCurrentRightCm);
		Fixture->SetNumberField(TEXT("movingPreviousRightCm"), FixtureFrame.MovingPreviousRightCm);
		Fixture->SetArrayField(TEXT("expectedMovingMotionDisplayPixels"), {
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedMovingMotionDisplayPixels.X),
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedMovingMotionDisplayPixels.Y) });
		Fixture->SetArrayField(TEXT("expectedBackgroundMotionDisplayPixels"), {
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedBackgroundMotionDisplayPixels.X),
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedBackgroundMotionDisplayPixels.Y) });
		Fixture->SetNumberField(TEXT("skeletalCurrentRightCm"), FixtureFrame.SkeletalCurrentRightCm);
		Fixture->SetNumberField(TEXT("skeletalPreviousRightCm"), FixtureFrame.SkeletalPreviousRightCm);
		Fixture->SetArrayField(TEXT("expectedSkeletalMotionDisplayPixels"), {
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedSkeletalMotionDisplayPixels.X),
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedSkeletalMotionDisplayPixels.Y) });
		Fixture->SetNumberField(TEXT("wpoCurrentRightCm"), FixtureFrame.WPOCurrentRightCm);
		Fixture->SetNumberField(TEXT("wpoPreviousRightCm"), FixtureFrame.WPOPreviousRightCm);
		Fixture->SetArrayField(TEXT("expectedWPOMotionDisplayPixels"), {
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedWPOMotionDisplayPixels.X),
			MakeShared<FJsonValueNumber>(FixtureFrame.ExpectedWPOMotionDisplayPixels.Y) });
		Fixture->SetArrayField(TEXT("niagaraAnchorDisplayPixels"), {
			MakeShared<FJsonValueNumber>(FixtureFrame.NiagaraAnchorDisplayPixels.X),
			MakeShared<FJsonValueNumber>(FixtureFrame.NiagaraAnchorDisplayPixels.Y) });
		Fixture->SetNumberField(
			TEXT("niagaraValidationRadiusDisplayPixels"),
			FixtureFrame.NiagaraValidationRadiusDisplayPixels);
		Fixture->SetBoolField(
			TEXT("niagaraVisibleProbeExpected"),
			FixtureFrame.bNiagaraVisibleProbeExpected);
		Fixture->SetStringField(
			TEXT("niagaraFixtureAsset"),
			TEXT("/SuperResolutionDataset/Validation/NS_SRDatasetVFXFixture.NS_SRDatasetVFXFixture"));
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
			Temporal->SetBoolField(
				TEXT("viewStateFrameIndexLogicalFrameLocked"),
				ActiveJob.bLockTemporalJitterToLogicalFrame);
			Temporal->SetNumberField(
				TEXT("expectedLogicalViewStateFrameIndex"),
				ActiveJob.bLockTemporalJitterToLogicalFrame
					? static_cast<double>(GetLogicalViewStateFrameIndex(FrameNumber))
					: static_cast<double>(Metadata.StateFrameIndex));
			Temporal->SetStringField(
				TEXT("viewStateFrameIndexSource"),
				ActiveJob.bLockTemporalJitterToLogicalFrame
					? TEXT("scene_view_override_logical_frame_plus_phase_mod_uint32")
					: TEXT("engine_view_state_submission_order"));
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
			Temporal->SetNumberField(
				TEXT("jitterIndex"),
				ActiveJob.bLockTemporalJitterToLogicalFrame
					? GetTemporalJitterOverrideIndex(FrameNumber)
					: Metadata.StateFrameIndex);
			Temporal->SetStringField(
				TEXT("jitterIndexSource"),
				ActiveJob.bLockTemporalJitterToLogicalFrame
					? TEXT("logical_frame_debug_override")
					: TEXT("engine_view_state_frame_index_proxy"));
			Temporal->SetBoolField(
				TEXT("jitterLogicalFrameLocked"),
				ActiveJob.bLockTemporalJitterToLogicalFrame);
			Temporal->SetNumberField(
				TEXT("jitterSequenceLength"),
				ActiveJob.TemporalJitterSequenceLength);
			Temporal->SetNumberField(
				TEXT("jitterPhaseOffset"),
				ActiveJob.TemporalJitterPhaseOffset);
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
				TEXT("component_identity_and_static_camera_depth_with_conservative_dynamic_uncertainty_v2"));
			Temporal->SetBoolField(TEXT("historyRejectionTrainingUsable"), true);
			Temporal->SetBoolField(TEXT("historyRejectionRequiresValidityMask"), true);
			Temporal->SetBoolField(TEXT("historyRejectionProductionCertified"), false);
			Temporal->SetStringField(TEXT("disocclusionMaskAlias"), TEXT("history_rejection_mask"));
			Temporal->SetStringField(TEXT("disocclusionValidityAlias"), TEXT("history_rejection_valid"));
			Temporal->SetStringField(TEXT("disocclusionReasonAlias"), TEXT("history_rejection_reason"));
			TSharedRef<FJsonObject> RejectionReasons = MakeShared<FJsonObject>();
			RejectionReasons->SetStringField(TEXT("0"), TEXT("accepted_static_or_camera_depth"));
			RejectionReasons->SetStringField(TEXT("1"), TEXT("history_reset"));
			RejectionReasons->SetStringField(TEXT("2"), TEXT("invalid_current_inputs"));
			RejectionReasons->SetStringField(TEXT("3"), TEXT("previous_pixel_out_of_bounds"));
			RejectionReasons->SetStringField(TEXT("4"), TEXT("instance_identity_mismatch"));
			RejectionReasons->SetStringField(TEXT("5"), TEXT("static_depth_occlusion"));
			RejectionReasons->SetStringField(TEXT("6"), TEXT("dynamic_same_instance_uncertain"));
			RejectionReasons->SetStringField(TEXT("7"), TEXT("unlabeled_dynamic_uncertain"));
			RejectionReasons->SetStringField(TEXT("8"), TEXT("depth_evidence_unavailable"));
			Temporal->SetObjectField(TEXT("historyRejectionReasonCodes"), RejectionReasons);
			Temporal->SetStringField(
				TEXT("historyRejectionKnownLimit"),
				ActiveJob.bAssignStableInstanceIds
					? TEXT("dynamic_same_component_self_occlusion_is_conservatively_rejected_with_validity_zero;unlabeled_or_non_custom_depth_pixels_may_be_invalid;fixed_topology_uint8_limit_255")
					: TEXT("dynamic_same_id_and_unlabeled_velocity_covered_geometry_are_conservatively_rejected_with_validity_zero;custom_stencil_uint8_may_not_be_instance_unique"));
			Temporal->SetNumberField(TEXT("motionPreviousLogicalFrameId"), MotionPreviousFrame);
			Temporal->SetNumberField(TEXT("motionTimeSpanS"), MotionTimeSpanFrames * ActiveJob.GetFixedDeltaSeconds());
			Temporal->SetBoolField(TEXT("motionTrainingUsable"), !bIntermediateReplay);
			Temporal->SetBoolField(TEXT("motionIncludesCamera"), true);
			Temporal->SetBoolField(TEXT("motionJitterRemoved"), true);
			Temporal->SetStringField(TEXT("transparencyMaskSource"), TEXT("one_minus_post_dof_separate_translucency_transmittance"));
			Temporal->SetStringField(TEXT("reactiveMaskSource"), TEXT("conservative_post_dof_transparency_coverage_v1"));
			Temporal->SetBoolField(TEXT("reactiveMaskIncludesOpaqueAnimation"), false);
			Temporal->SetStringField(
				TEXT("gbufferAttributeSource"),
				TEXT("deferred_main_view_gbuffer_same_pixel_observed_at_after_dof"));
			Temporal->SetStringField(TEXT("worldNormalSpace"), TEXT("unreal_world_left_handed_xyz_unit_vector"));
			Temporal->SetStringField(TEXT("baseColorSpace"), TEXT("linear_material_base_color_rgb"));
			Temporal->SetStringField(
				TEXT("materialPropertiesChannels"),
				TEXT("R_roughness_G_metallic_B_specular_A_valid"));
			Temporal->SetStringField(
				TEXT("gbufferValidity"),
				TEXT("one_for_finite_positive_scene_depth_zero_for_sky_or_no_opaque_surface"));
			Temporal->SetStringField(
				TEXT("gbufferReplayComparison"),
				TEXT("quantized_attribute_numeric_tolerance_v1_with_heatmap"));
			Temporal->SetStringField(
				TEXT("gbufferReplayKnownLimit"),
				TEXT("sparse_raster_and_material_quantization_boundary_pixels_are_not_promised_byte_exact"));
			Temporal->SetStringField(
				TEXT("objectIdSource"),
				ActiveJob.bAssignStableInstanceIds
					? TEXT("stable_component_unique_custom_stencil_uint8_zero_background")
					: TEXT("custom_stencil_uint8_zero_unlabeled"));
			Temporal->SetBoolField(TEXT("objectIdInstanceUnique"), ActiveJob.bAssignStableInstanceIds);
			Temporal->SetStringField(
				TEXT("objectIdMappingSha1"),
				ActiveJob.bAssignStableInstanceIds ? StableInstanceIdMappingSha1 : TEXT("not_used"));
			Temporal->SetNumberField(
				TEXT("objectIdInstanceCount"),
				ActiveJob.bAssignStableInstanceIds ? StableInstanceIdRecords.Num() : 0);
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
				Native->SetNumberField(TEXT("renderGameTimeS"), NativeHR.GameTimeSeconds);
				Native->SetNumberField(TEXT("renderDeltaTimeS"), NativeHR.DeltaTimeSeconds);
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

			const FSRDatasetTemporalFrameMetadata& SceneCaptureLR = CaptureRig->GetLastSceneCaptureLRMetadata();
			if (ActiveJob.bCaptureSceneCaptureLRComparison && SceneCaptureLR.bValid)
			{
				TSharedRef<FJsonObject> Comparison = MakeShared<FJsonObject>();
				Comparison->SetStringField(TEXT("pipelineStage"), TEXT("after_dof_before_temporal_upscaler"));
				Comparison->SetStringField(TEXT("colorSpace"), TEXT("linear_scene_rgb"));
				Comparison->SetBoolField(TEXT("preExposed"), true);
				Comparison->SetNumberField(TEXT("preExposure"), SceneCaptureLR.PreExposure);
				Comparison->SetNumberField(TEXT("exposure"), SceneCaptureLR.OneOverPreExposure);
				Comparison->SetNumberField(TEXT("renderGameTimeS"), SceneCaptureLR.GameTimeSeconds);
				Comparison->SetNumberField(TEXT("renderDeltaTimeS"), SceneCaptureLR.DeltaTimeSeconds);
				Comparison->SetNumberField(TEXT("automaticViewMipBias"), SceneCaptureLR.MaterialTextureMipBias);
				Comparison->SetStringField(TEXT("automaticViewMipBiasPolicy"), TEXT("isolated_native_lr_scene_capture"));
				Comparison->SetNumberField(TEXT("globalMipMapLODBias"), GlobalMipMapLODBias);
				Comparison->SetNumberField(
					TEXT("effectiveMaterialTextureMipBias"),
					SceneCaptureLR.MaterialTextureMipBias + GlobalMipMapLODBias);
				Comparison->SetArrayField(TEXT("renderSize"), {
					MakeShared<FJsonValueNumber>(SceneCaptureLR.ViewSize.X),
					MakeShared<FJsonValueNumber>(SceneCaptureLR.ViewSize.Y) });
				Comparison->SetArrayField(TEXT("displaySize"), {
					MakeShared<FJsonValueNumber>(ActiveJob.HRResolution.X),
					MakeShared<FJsonValueNumber>(ActiveJob.HRResolution.Y) });
				Comparison->SetArrayField(TEXT("jitterCurrentNDC"), Vector2Array(SceneCaptureLR.JitterCurrentNDC));
				Comparison->SetArrayField(TEXT("jitterPreviousNDC"), Vector2Array(SceneCaptureLR.JitterPreviousNDC));
				Comparison->SetBoolField(TEXT("fixedOutputGrid"), SceneCaptureLR.JitterCurrentNDC.IsNearlyZero());
				Comparison->SetBoolField(TEXT("historyAdvance"), false);
				Comparison->SetBoolField(TEXT("simulationAdvance"), false);
				Comparison->SetStringField(TEXT("viewState"), TEXT("isolated_native_lr_scene_capture"));
				Comparison->SetStringField(
					TEXT("pixelAlignment"),
					TEXT("scene_capture_resampled_to_main_view_using_main_view_current_render_pixel_jitter"));
				Comparison->SetArrayField(TEXT("viewToClipCurrentJittered"), MatrixArray(SceneCaptureLR.ViewToClipJittered));
				Comparison->SetArrayField(TEXT("viewToClipCurrentUnjittered"), MatrixArray(SceneCaptureLR.ViewToClipUnjittered));
				Comparison->SetArrayField(TEXT("translatedWorldToViewCurrent"), MatrixArray(SceneCaptureLR.TranslatedWorldToViewCurrent));
				Comparison->SetArrayField(TEXT("worldViewOriginHighCurrent"), Vector3Array(SceneCaptureLR.WorldViewOriginHighCurrent));
				Comparison->SetArrayField(TEXT("worldViewOriginLowCurrent"), Vector3Array(SceneCaptureLR.WorldViewOriginLowCurrent));
				Frame->SetObjectField(TEXT("sceneCaptureLRComparisonDiagnostics"), Comparison);
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
				Reference->SetNumberField(TEXT("renderGameTimeS"), ReferenceHR.GameTimeSeconds);
				Reference->SetNumberField(TEXT("renderDeltaTimeS"), ReferenceHR.DeltaTimeSeconds);
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
				HUDlessDiagnostics->SetNumberField(TEXT("renderGameTimeS"), HUDless.GameTimeSeconds);
				HUDlessDiagnostics->SetNumberField(TEXT("renderDeltaTimeS"), HUDless.DeltaTimeSeconds);
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
		if (ActiveJob.bCaptureUIColorAlpha)
		{
			const FIntPoint UISize = CaptureRig->GetLastUIColorAlphaSize();
			TSharedRef<FJsonObject> UI = MakeShared<FJsonObject>();
			UI->SetStringField(TEXT("pipelineStage"), TEXT("independent_slate_game_layer_before_scene_composite"));
			UI->SetStringField(TEXT("colorEncoding"), TEXT("display_referred_srgb_png_unorm8"));
			UI->SetStringField(TEXT("alphaSemantic"), TEXT("straight_coverage_zero_is_transparent_one_is_opaque"));
			UI->SetStringField(TEXT("rgbSemantic"), TEXT("premultiplied_by_coverage_alpha"));
			UI->SetStringField(TEXT("source"), TEXT("SGameLayerManager_without_enclosing_SViewport_scene_backbuffer"));
			UI->SetBoolField(TEXT("sceneIncluded"), false);
			UI->SetBoolField(TEXT("screenSpaceGameLayersIncluded"), true);
			UI->SetBoolField(TEXT("displayResolution"), true);
			UI->SetArrayField(TEXT("size"), {
				MakeShared<FJsonValueNumber>(UISize.X), MakeShared<FJsonValueNumber>(UISize.Y) });
			UI->SetNumberField(TEXT("nonzeroAlphaPixelCount"), CaptureRig->GetLastUINonzeroAlphaPixelCount());
			UI->SetNumberField(TEXT("fractionalAlphaPixelCount"), CaptureRig->GetLastUIFractionalAlphaPixelCount());
			UI->SetNumberField(TEXT("minAlpha"), CaptureRig->GetLastUIMinAlpha());
			UI->SetNumberField(TEXT("maxAlpha"), CaptureRig->GetLastUIMaxAlpha());
			UI->SetBoolField(TEXT("semanticValidationFixture"), ValidationUIWidget.IsValid());
			if (ValidationUIWidget)
			{
				TArray<TSharedPtr<FJsonValue>> Probes;
				const auto AddProbe = [&Probes](const TCHAR* Name, const FVector2D Min, const FVector2D Extent, const FLinearColor Color)
				{
					TSharedRef<FJsonObject> Probe = MakeShared<FJsonObject>();
					Probe->SetStringField(TEXT("name"), Name);
					Probe->SetArrayField(TEXT("normalizedMin"), {
						MakeShared<FJsonValueNumber>(Min.X), MakeShared<FJsonValueNumber>(Min.Y) });
					Probe->SetArrayField(TEXT("normalizedExtent"), {
						MakeShared<FJsonValueNumber>(Extent.X), MakeShared<FJsonValueNumber>(Extent.Y) });
					Probe->SetArrayField(TEXT("straightRGBA"), {
						MakeShared<FJsonValueNumber>(Color.R), MakeShared<FJsonValueNumber>(Color.G),
						MakeShared<FJsonValueNumber>(Color.B), MakeShared<FJsonValueNumber>(Color.A) });
					Probes.Add(MakeShared<FJsonValueObject>(Probe));
				};
				AddProbe(TEXT("opaque_red"), FVector2D(0.05, 0.08), FVector2D(0.20, 0.14), FLinearColor(1.0, 0.0, 0.0, 1.0));
				AddProbe(TEXT("half_green"), FVector2D(0.32, 0.08), FVector2D(0.20, 0.14), FLinearColor(0.0, 1.0, 0.0, 0.5));
				AddProbe(TEXT("three_quarter_blue"), FVector2D(0.59, 0.08), FVector2D(0.20, 0.14), FLinearColor(0.0, 0.0, 1.0, 0.75));
				UI->SetArrayField(TEXT("validationProbes"), Probes);
			}
			Frame->SetObjectField(TEXT("uiColorAlphaDiagnostics"), UI);
		}
	}

	ManifestFrames.Add(MakeShared<FJsonValueObject>(Frame));
}

bool USRDatasetCaptureSubsystem::WriteManifest(FString& OutError) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 2);
	Root->SetStringField(TEXT("pluginVersion"), TEXT("0.12.0"));
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

	TSharedRef<FJsonObject> SceneControl = MakeShared<FJsonObject>();
	SceneControl->SetBoolField(TEXT("enabled"), ActiveJob.bRunSceneControlPreflight);
	SceneControl->SetBoolField(TEXT("required"), ActiveJob.bRequireSceneControlPreflight);
	SceneControl->SetBoolField(TEXT("ran"), SceneControlPreflight.bRan);
	SceneControl->SetBoolField(TEXT("passed"), SceneControlPreflight.bPassed);
	SceneControl->SetStringField(
		TEXT("file"),
		ActiveJob.bRunSceneControlPreflight ? TEXT("scene_control_preflight.json") : TEXT("not_generated"));
	SceneControl->SetStringField(TEXT("sha1"), SceneControlPreflight.Sha1);
	SceneControl->SetStringField(
		TEXT("hashScope"),
		TEXT("schema_required_sorted_allowlist_rules_and_classified_tick_niagara_di_material_input_records"));
	SceneControl->SetNumberField(
		TEXT("uncontrolledTickingActorCount"),
		SceneControlPreflight.UncontrolledTickingActors.Num());
	SceneControl->SetNumberField(
		TEXT("uncontrolledTickingComponentCount"),
		SceneControlPreflight.UncontrolledTickingComponents.Num());
	SceneControl->SetNumberField(
		TEXT("uncontrolledNiagaraDataInterfaceCount"),
		SceneControlPreflight.UncontrolledNiagaraDataInterfaces.Num());
	SceneControl->SetNumberField(
		TEXT("uncontrolledMaterialInputCount"),
		SceneControlPreflight.UncontrolledMaterialInputs.Num());
	Root->SetObjectField(TEXT("sceneControlPreflight"), SceneControl);

	TSharedRef<FJsonObject> StableIds = MakeShared<FJsonObject>();
	StableIds->SetBoolField(TEXT("enabled"), ActiveJob.bAssignStableInstanceIds);
	StableIds->SetBoolField(TEXT("preparedAfterWarmupAndStreaming"), bStableInstanceIdsPrepared);
	StableIds->SetStringField(
		TEXT("encoding"),
		ActiveJob.bAssignStableInstanceIds ? TEXT("custom_stencil_uint8") : TEXT("not_used"));
	StableIds->SetStringField(
		TEXT("mappingFile"),
		ActiveJob.bAssignStableInstanceIds ? TEXT("instance_id_map.json") : TEXT("not_used"));
	StableIds->SetStringField(
		TEXT("mappingSha1"),
		ActiveJob.bAssignStableInstanceIds ? StableInstanceIdMappingSha1 : TEXT("not_used"));
	StableIds->SetNumberField(
		TEXT("instanceCount"),
		ActiveJob.bAssignStableInstanceIds ? StableInstanceIdRecords.Num() : 0);
	StableIds->SetNumberField(TEXT("backgroundId"), 0);
	StableIds->SetNumberField(TEXT("maximumAssignableId"), 255);
	StableIds->SetBoolField(TEXT("fixedTopologyRequired"), ActiveJob.bAssignStableInstanceIds);
	Root->SetObjectField(TEXT("stableInstanceIds"), StableIds);

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
	Provenance->SetStringField(TEXT("skeletalPoseCacheArtifactSha1"), SkeletalPoseCacheArtifactSha1);
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
	Contract->SetStringField(
		TEXT("materialTimeEvaluation"),
		ActiveJob.bLockMaterialTimeToLogicalFrame
			? TEXT("scene_view_family_game_time_current_and_signed_previous_from_logical_frame_ids_real_time_frozen")
			: TEXT("engine_world_and_wall_clock_time"));
	Contract->SetBoolField(TEXT("sceneControlPreflightEnabled"), ActiveJob.bRunSceneControlPreflight);
	Contract->SetBoolField(TEXT("sceneControlPreflightRequired"), ActiveJob.bRequireSceneControlPreflight);
	Contract->SetBoolField(TEXT("sceneControlPreflightPassed"), SceneControlPreflight.bPassed);
	Contract->SetBoolField(TEXT("controllableCanonicalStateRequired"), ActiveJob.bRequireControllableState);
	Contract->SetStringField(
		TEXT("controllableCanonicalStateScope"),
		TEXT("plugin_stores_sha1_and_utf8_byte_count_only;implementer_owns_canonical_serialization"));
	Contract->SetStringField(
		TEXT("sceneControlPreflightScope"),
		TEXT("registered_ticking_actors_and_components_loaded_niagara_data_interfaces_and_material_time_per_instance_random_particle_random_expressions"));
	Contract->SetStringField(
		TEXT("objectIdEncoding"),
		ActiveJob.bAssignStableInstanceIds
			? TEXT("fixed_topology_component_unique_custom_stencil_uint8_with_hashed_mapping_zero_background")
			: TEXT("scene_authored_custom_stencil_uint8_zero_unlabeled"));
	Contract->SetBoolField(TEXT("objectIdInstanceUnique"), ActiveJob.bAssignStableInstanceIds);
	Contract->SetBoolField(TEXT("objectIdFixedTopologyRequired"), ActiveJob.bAssignStableInstanceIds);
	Contract->SetNumberField(TEXT("objectIdMaximumInstances"), ActiveJob.bAssignStableInstanceIds ? 255 : 0);
	Contract->SetStringField(
		TEXT("worldSpaceWidgetPolicy"),
		ActiveJob.bRejectVisibleWidgetComponents
			? TEXT("reject_any_visible_registered_UWidgetComponent_before_and_during_capture")
			: TEXT("visible_UWidgetComponent_is_scene_content"));
	Contract->SetStringField(
		TEXT("cameraEvaluation"),
		ActiveJob.bUseDeterministicCameraTransform
			? TEXT("explicit_transient_player_view_target_locked_each_tick")
			: TEXT("sequencer_then_tag_then_player_camera"));
	Contract->SetBoolField(TEXT("chaosDeterminism"), ActiveJob.bEnableChaosDeterminism);
	Contract->SetBoolField(TEXT("niagaraAbsoluteAge"), ActiveJob.bControlNiagara);
	Contract->SetBoolField(TEXT("niagaraForcedDeterminism"), ActiveJob.bControlNiagara && ActiveJob.bForceNiagaraDeterminism);
	Contract->SetStringField(
		TEXT("niagaraAgeEvaluation"),
		ActiveJob.bControlNiagara
			? TEXT("solo_absolute_fixed_step_advance_wait_for_concurrent_tick_and_finalize_before_capture")
			: TEXT("uncontrolled"));
	Contract->SetStringField(
		TEXT("niagaraInitialAgeWarmup"),
		ActiveJob.bControlNiagara
			? TEXT("progressive_fixed_age_ramp_normal_single_tick_plus_one_ulp_when_initial_age_nonzero")
			: TEXT("uncontrolled"));
	Contract->SetStringField(
		TEXT("skeletalAnimationReplay"),
		ActiveJob.bCacheSkeletalAnimationPosesForReplay
			? TEXT("shared_or_forward_baked_component_space_pose_cache_by_logical_frame")
			: TEXT("engine_live_evaluation"));
	Contract->SetStringField(
		TEXT("skeletalPoseCacheArtifact"),
		ActiveJob.bCacheSkeletalAnimationPosesForReplay
			? TEXT("engine_versioned_binary_component_path_asset_bones_visibility_sha1")
			: TEXT("not_used"));
	Contract->SetBoolField(
		TEXT("nonFixtureSkeletalValidationEnabled"),
		ActiveJob.bValidateNonFixtureSkeletalAnimation);
	Contract->SetStringField(
		TEXT("nonFixtureSkeletalValidationActorClass"),
		ActiveJob.bValidateNonFixtureSkeletalAnimation
			? ActiveJob.NonFixtureSkeletalValidationActorClass.ToString()
			: TEXT("none"));
	Contract->SetBoolField(
		TEXT("projectAnimatedMaterialValidationEnabled"),
		ActiveJob.bValidateProjectAnimatedMaterial);
	Contract->SetStringField(
		TEXT("projectAnimatedMaterialValidationInterface"),
		ActiveJob.bValidateProjectAnimatedMaterial
			? ActiveJob.ProjectAnimatedMaterialValidationMaterial.ToString()
			: TEXT("none"));
	Contract->SetStringField(
		TEXT("projectAnimatedMaterialValidationCarrier"),
		ActiveJob.bValidateProjectAnimatedMaterial
			? TEXT("transient_labeled_engine_cube_with_project_material_interface")
			: TEXT("not_used"));
	Contract->SetStringField(
		TEXT("niagaraPayloadEvidence"),
		TEXT("cpu_emitter_particle_counts_and_visible_semantic_fixture_pixels_gpu_payload_not_read_back"));
	Contract->SetStringField(TEXT("exposureControl"), ActiveJob.bLockExposure ? TEXT("eye_adaptation_disabled") : TEXT("scene_authored_dynamic"));
	Contract->SetStringField(TEXT("renderScheduling"), ActiveJob.bForceSynchronousRendering ? TEXT("offline_synchronous_profile") : TEXT("project_default"));
	Contract->SetStringField(TEXT("depthEncoding"), TEXT("scene_capture_linear_distance_cm"));
	Contract->SetStringField(TEXT("depthContainer"), TEXT("OpenEXR float, R channel"));
	Contract->SetNumberField(TEXT("viewSpaceToMeters"), 0.01);
	Contract->SetStringField(
		TEXT("deviceDepth"),
		ActiveJob.bCaptureTemporalDiagnostics ? TEXT("experimental_raw_reversed_z_from_scene_depth") : TEXT("not_captured"));
	Contract->SetBoolField(TEXT("temporalDiagnosticsEnabled"), ActiveJob.bCaptureTemporalDiagnostics);
	Contract->SetBoolField(TEXT("sceneCaptureLRComparisonEnabled"), ActiveJob.bCaptureSceneCaptureLRComparison);
	Contract->SetBoolField(
		TEXT("mainViewSceneCapturePixelDomainValidationRequired"),
		ActiveJob.bValidateMainViewSceneCapturePixelDomain);
	Contract->SetStringField(
		TEXT("sceneCaptureLRComparison"),
		ActiveJob.bCaptureSceneCaptureLRComparison
			? TEXT("existing_native_lr_scene_capture_after_dof_linear_scene_rgb_paired_without_simulation_advance")
			: TEXT("not_captured"));
	Contract->SetBoolField(TEXT("semanticValidationFixtureEnabled"), ActiveJob.bEnableSemanticValidationFixture);
	Contract->SetStringField(
		TEXT("semanticMotionScenario"),
		StaticEnum<ESRDatasetSemanticMotionScenario>()->GetNameStringByValue(
			static_cast<int64>(ActiveJob.SemanticMotionScenario)));
	Contract->SetStringField(
		TEXT("semanticFixtureAnchorPolicy"),
		ActiveJob.SemanticMotionScenario == ESRDatasetSemanticMotionScenario::LegacyCameraRelative
			? TEXT("camera_relative_legacy")
			: TEXT("world_anchored_at_initial_deterministic_camera"));
	Contract->SetBoolField(
		TEXT("temporalJitterSignCoverageRequired"),
		ActiveJob.bValidateTemporalJitterSignCoverage);
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
	Contract->SetStringField(
		TEXT("temporalJitterPhasePolicy"),
		ActiveJob.bLockTemporalJitterToLogicalFrame
			? TEXT("logical_frame_plus_phase_offset_mod_sequence_length_non_shipping_debug_override_and_scene_view_state_frame_override")
			: TEXT("engine_view_state_submission_order"));
	Contract->SetBoolField(TEXT("uncapturedMainViewSuppressed"), ActiveJob.bSuppressMainViewOnUncapturedFrames);
	Contract->SetStringField(
		TEXT("uncapturedRendererPrime"),
		ActiveJob.bSuppressMainViewOnUncapturedFrames
			? TEXT("offscreen_scene_capture_64_pixel_long_edge_without_player_main_view_history")
			: TEXT("not_required"));
	Contract->SetStringField(
		TEXT("endpointPreviousTransformScope"),
		ActiveJob.bUseLastCapturedEndpointTransforms
			? TEXT("scene_component_transforms_plus_double_buffered_skinned_component_space_bones_plus_explicit_previous_frame_switch_wpo_fixture_non_fixture_wpo_uncertified")
			: TEXT("engine_previous_render_state"));
	if (ActiveJob.bCaptureTemporalDiagnostics)
	{
		Contract->SetStringField(TEXT("motion"), TEXT("experimental_full_current_to_previous_display_pixels"));
		Contract->SetStringField(TEXT("jitterAndMatrices"), TEXT("experimental_captured_from_view_uniforms"));
		Contract->SetStringField(
			TEXT("historyRejection"),
			TEXT("experimental_component_identity_and_static_depth_reprojection_with_reason_and_conservative_dynamic_validity"));
		Contract->SetStringField(
			TEXT("disocclusion"),
			TEXT("aliases_history_rejection_mask_valid_reason;cross_instance_and_static_depth_exact;dynamic_same_instance_conservative_invalid"));
		Contract->SetStringField(TEXT("temporalDiagnosticsStatus"), TEXT("experimental_uncertified"));
		Contract->SetStringField(TEXT("temporalDiagnosticsStage"), TEXT("after_dof_before_temporal_upscaler"));
		Contract->SetStringField(
			TEXT("gbufferAttributes"),
			TEXT("deferred_main_view_world_normal_base_color_roughness_metallic_specular_with_validity"));
		Contract->SetStringField(
			TEXT("gbufferReplayComparison"),
			TEXT("quantized_attribute_numeric_tolerance_v1_with_heatmap"));
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
			TEXT("uiColorAlpha"),
			ActiveJob.bCaptureUIColorAlpha
				? TEXT("independent_slate_game_layer_display_resolution_premultiplied_rgb_straight_coverage_alpha_png")
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
		Contract->SetStringField(TEXT("disocclusion"), TEXT("not_captured"));
		Contract->SetStringField(TEXT("gbufferAttributes"), TEXT("not_captured"));
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
	ClearLogicalMaterialTime();
	RestoreSemanticValidationFixture();
	RestoreNonFixtureSkeletalValidation();
	if (NonFixtureSkeletalValidationActor)
	{
		NonFixtureSkeletalValidationActor->Destroy();
		NonFixtureSkeletalValidationActor = nullptr;
	}
	RestoreStableInstanceIds();
	RestoreProjectAnimatedMaterialValidation();
	RestoreDeterministicCamera();

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
