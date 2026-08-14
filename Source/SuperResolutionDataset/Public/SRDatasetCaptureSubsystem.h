#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SRDatasetTypes.h"
#include "SRDatasetCaptureSubsystem.generated.h"

class ASRDatasetCaptureRig;
class ASRDatasetValidationFixture;
class AActor;
class ALevelSequenceActor;
class FJsonValue;
class ULevelSequencePlayer;
class UNiagaraComponent;
class UNiagaraSystem;
class USceneComponent;

UCLASS()
class SUPERRESOLUTIONDATASET_API USRDatasetCaptureSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	UFUNCTION(BlueprintCallable, Category = "SR Dataset")
	bool StartCapture(const FSRDatasetCaptureJob& Job, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SR Dataset")
	void CancelCapture();

	UFUNCTION(BlueprintPure, Category = "SR Dataset")
	FSRDatasetCaptureStatus GetCaptureStatus() const { return Status; }

	bool StartCaptureFromJsonFile(const FString& JobJsonPath, FString& OutError);

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FSceneStateSummary
	{
		FString Sha1;
		int32 ActorCount = 0;
		int32 ComponentCount = 0;
		int32 SkeletalComponentCount = 0;
		int32 BoneCount = 0;
		int32 FXComponentCount = 0;
		int32 NiagaraComponentCount = 0;
		int32 ControllableActorCount = 0;
		int32 UncontrolledTickingActorCount = 0;
		TArray<FString> ControllableActors;
		TArray<FString> UncontrolledTickingActors;
	};

	struct FNiagaraComponentState
	{
		uint8 AgeUpdateMode = 0;
		int32 RandomSeedOffset = 0;
		float SeekDelta = 0.0f;
		float MaxSimTime = 0.0f;
		bool bForceSolo = false;
		bool bLockSeekDelta = false;
	};

	struct FNiagaraSystemState
	{
		bool bDeterminism = false;
		bool bFixedTickDelta = false;
		int32 RandomSeed = 0;
		float FixedTickDeltaTime = 0.0f;
	};

	void HandleWorldPreActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	bool PrepareJob(FString& OutError);
	bool PrepareSemanticValidationFixture(FString& OutError);
	void RestoreSemanticValidationFixture();
	bool IsReverseEndpointReplay() const;
	int32 GetEvaluationDirection() const;
	int32 GetInitialEvaluationFrame() const;
	int32 GetFirstCapturedFrame() const;
	int32 GetPreviouslyCapturedFrame(int32 FrameNumber) const;
	int32 GetTemporalJitterOverrideIndex(int32 FrameNumber) const;
	bool IsPastEvaluationRange(int32 FrameNumber) const;
	bool ShouldCaptureFrame(int32 FrameNumber) const;
	void ApplyLastCapturedEndpointTransforms();
	void SnapshotCapturedEndpointTransforms();
	void ApplyDeterministicRuntimeState();
	void ApplyLogicalTemporalJitter(int32 FrameNumber);
	void RestoreDeterministicRuntimeState();
	void SnapshotProvenance();
	bool EnsureStreamingReady(FString& OutError);
	FString ComputeStreamingStateSha1(int32& OutTextureCount, int32& OutPendingTextureCount) const;
	FSceneStateSummary ComputeSceneStateSummary() const;
	void DiscoverAndControlNiagara(float TimeSeconds);
	void RestoreNiagara();
	void NotifyControllablesPrepare();
	void NotifyControllablesEvaluate(int32 FrameNumber, float TimeSeconds);
	void NotifyControllablesRestore();
	bool EvaluateSequence(int32 FrameNumber, FString& OutError);
	bool UpdateCaptureCamera(FString& OutError);
	bool CaptureCurrentFrame(FString& OutError);
	bool FinalizePendingMainViewCapture(FString& OutError);
	bool IsFrameAlreadyComplete(int32 FrameNumber) const;
	void AppendFrameManifest(int32 FrameNumber, double TimeSeconds, const TMap<FString, FString>& Hashes, const TMap<FString, int64>& RenderSubmissions, bool bResumed);
	bool WriteManifest(FString& OutError) const;
	void FinishCapture(ESRDatasetCaptureState FinalState, const FString& Error = FString());
	FString MakeFramePath(const TCHAR* Modality, int32 FrameNumber, const TCHAR* Extension) const;
	FString ResolveOutputDirectory(const FString& InPath) const;
	static FString HashFile(const FString& Filename);
	static FString HashString(const FString& Value);

	FSRDatasetCaptureJob ActiveJob;
	FSRDatasetCaptureStatus Status;
	FString ResolvedOutputDirectory;
	int32 WarmupFramesRemaining = 0;
	int64 NextRenderSubmissionId = 0;
	bool bCommandLineAutoQuit = false;
	bool bPreviousUseFixedTimeStep = false;
	double PreviousFixedDeltaTime = 0.0;
	int32 PreviousChaosDeterminism = -1;
	int32 PreviousEyeAdaptationQuality = -1;
	int32 PreviousCustomDepthMode = -1;
	int32 PreviousMotionVectorSimulation = -1;
	int32 PreviousTemporalJitterOverrideIndex = -1;
	float PreviousScreenPercentage = 100.0f;
	int32 PreviousDynamicResolutionOperationMode = 0;
	bool bOverrodeScreenPercentage = false;
	bool bOverrodeDynamicResolution = false;
	bool bOverrodeEyeAdaptation = false;
	bool bOverrodeCustomDepth = false;
	bool bOverrodeMotionVectorSimulation = false;
	bool bOverrodeTemporalJitterIndex = false;
	bool bPreviousWorldRenderingEnabled = true;
	bool bOverrodeWorldRendering = false;
	bool bMainViewCapturePending = false;
	int32 PendingMainViewFrameNumber = INDEX_NONE;
	double PendingMainViewTimeSeconds = 0.0;
	TMap<FString, FString> PendingMainViewHashes;
	TMap<FString, int64> PendingMainViewRenderSubmissions;
	TMap<FString, FString> PreviousRenderDeterminismCVars;
	TMap<FString, FString> CaptureCVarProfile;
	FString CaptureConfigSha1;
	FString CaptureCVarProfileSha1;
	FString CaptureCVarProfileCanonical;
	FString ContentMapSha1;
	FString ShaderSourceSha1;
	bool bStreamingBarrierComplete = false;
	int32 StreamingRequestsAfterBarrier = INDEX_NONE;
	int32 StreamingTextureCountAfterBarrier = 0;
	int32 PendingStreamingTextureCountAfterBarrier = 0;
	FString StreamingStateAfterBarrierSha1;
	FDelegateHandle PreActorTickHandle;
	FDelegateHandle PostActorTickHandle;

	UPROPERTY(Transient)
	TObjectPtr<ASRDatasetCaptureRig> CaptureRig;

	UPROPERTY(Transient)
	TObjectPtr<ASRDatasetValidationFixture> ValidationFixture;

	UPROPERTY(Transient)
	TObjectPtr<ULevelSequencePlayer> SequencePlayer;

	UPROPERTY(Transient)
	TObjectPtr<ALevelSequenceActor> SequenceActor;

	TMap<TWeakObjectPtr<UNiagaraComponent>, FNiagaraComponentState> NiagaraComponentStates;
	TMap<TWeakObjectPtr<UNiagaraSystem>, FNiagaraSystemState> NiagaraSystemStates;
	TSet<TWeakObjectPtr<AActor>> PreparedControllables;
	TMap<TWeakObjectPtr<AActor>, bool> ValidationHiddenActorStates;
	TMap<TWeakObjectPtr<USceneComponent>, FTransform> LastCapturedEndpointTransforms;
	TArray<TSharedPtr<FJsonValue>> ManifestFrames;
};
