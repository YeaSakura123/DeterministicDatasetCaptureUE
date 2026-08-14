#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SRDatasetTypes.h"
#include "SRDatasetCaptureSubsystem.generated.h"

class ASRDatasetCaptureRig;
class AActor;
class ALevelSequenceActor;
class FJsonValue;
class ULevelSequencePlayer;
class UNiagaraComponent;
class UNiagaraSystem;

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
	void ApplyDeterministicRuntimeState();
	void RestoreDeterministicRuntimeState();
	void DiscoverAndControlNiagara(float TimeSeconds);
	void RestoreNiagara();
	void NotifyControllablesPrepare();
	void NotifyControllablesEvaluate(int32 FrameNumber, float TimeSeconds);
	void NotifyControllablesRestore();
	bool EvaluateSequence(int32 FrameNumber, FString& OutError);
	bool UpdateCaptureCamera(FString& OutError);
	bool CaptureCurrentFrame(FString& OutError);
	bool IsFrameAlreadyComplete(int32 FrameNumber) const;
	void AppendFrameManifest(int32 FrameNumber, double TimeSeconds, const TMap<FString, FString>& Hashes, bool bResumed);
	bool WriteManifest(FString& OutError) const;
	void FinishCapture(ESRDatasetCaptureState FinalState, const FString& Error = FString());
	FString MakeFramePath(const TCHAR* Modality, int32 FrameNumber, const TCHAR* Extension) const;
	FString ResolveOutputDirectory(const FString& InPath) const;
	static FString HashFile(const FString& Filename);

	FSRDatasetCaptureJob ActiveJob;
	FSRDatasetCaptureStatus Status;
	FString ResolvedOutputDirectory;
	int32 WarmupFramesRemaining = 0;
	bool bCommandLineAutoQuit = false;
	bool bPreviousUseFixedTimeStep = false;
	double PreviousFixedDeltaTime = 0.0;
	int32 PreviousChaosDeterminism = -1;
	FDelegateHandle PreActorTickHandle;
	FDelegateHandle PostActorTickHandle;

	UPROPERTY(Transient)
	TObjectPtr<ASRDatasetCaptureRig> CaptureRig;

	UPROPERTY(Transient)
	TObjectPtr<ULevelSequencePlayer> SequencePlayer;

	UPROPERTY(Transient)
	TObjectPtr<ALevelSequenceActor> SequenceActor;

	TMap<TWeakObjectPtr<UNiagaraComponent>, FNiagaraComponentState> NiagaraComponentStates;
	TMap<TWeakObjectPtr<UNiagaraSystem>, FNiagaraSystemState> NiagaraSystemStates;
	TSet<TWeakObjectPtr<AActor>> PreparedControllables;
	TArray<TSharedPtr<FJsonValue>> ManifestFrames;
};
