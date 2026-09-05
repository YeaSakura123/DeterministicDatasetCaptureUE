#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SRDatasetTypes.h"
#include "SRDatasetCaptureSubsystem.generated.h"

class ASRDatasetCaptureRig;
class ASRDatasetChaosValidationActor;
class ASRDatasetValidationFixture;
class ASRDatasetStateCacheValidationActor;
class AChaosCacheManager;
class ACameraActor;
class AActor;
class APlayerController;
class ALevelSequenceActor;
class FJsonValue;
class ULevelSequencePlayer;
class UNiagaraComponent;
class UNiagaraSimCache;
class UNiagaraSystem;
class UChaosCache;
class UChaosCacheCollection;
class USceneComponent;
class UStaticMeshComponent;
class USkinnedMeshComponent;
class SWidget;

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
		struct FControllableStateSummary
		{
			FString ActorPath;
			FString StateSha1;
			int32 Utf8Bytes = 0;
		};

		struct FNiagaraSummary
		{
			FString ComponentPath;
			FString AssetPath;
			double DesiredAgeSeconds = 0.0;
			double SimulationAgeSeconds = 0.0;
			int32 EmitterCount = 0;
			int32 CPUEmitterCount = 0;
			int32 GPUEmitterCount = 0;
			int32 ParticleCount = 0;
			int32 TotalSpawnedParticleCount = 0;
			int32 RendererCount = 0;
			bool bSoloInstanceObservable = false;
			bool bSystemDeterminism = false;
			bool bSystemFixedTick = false;
			double SystemFixedTickSeconds = 0.0;
			FVector ComponentLocationCm = FVector::ZeroVector;
			FVector ComponentBoundsOriginCm = FVector::ZeroVector;
			FVector ComponentBoundsExtentCm = FVector::ZeroVector;
			TArray<int32> EmitterParticleCounts;
			TArray<bool> EmitterDeterminism;
			TArray<int32> EmitterRandomSeeds;
			TArray<FString> MaterialPaths;
		};

		FString Sha1;
		int32 ActorCount = 0;
		int32 ComponentCount = 0;
		int32 SkeletalComponentCount = 0;
		int32 BoneCount = 0;
		int32 FXComponentCount = 0;
		int32 NiagaraComponentCount = 0;
		int32 NiagaraEmitterCount = 0;
		int32 NiagaraCPUEmitterCount = 0;
		int32 NiagaraGPUEmitterCount = 0;
		int32 NiagaraParticleCount = 0;
		int32 NiagaraTotalSpawnedParticleCount = 0;
		int32 ControllableActorCount = 0;
		int32 ControllableStateCount = 0;
		int32 UncontrolledTickingActorCount = 0;
		TArray<FString> ControllableActors;
		TArray<FControllableStateSummary> ControllableStates;
		TArray<FString> ControllableActorsWithoutState;
		TArray<FString> UncontrolledTickingActors;
		TArray<FNiagaraSummary> NiagaraComponents;
	};

	struct FSceneControlPreflightReport
	{
		bool bRan = false;
		bool bPassed = false;
		FString Sha1;
		TArray<FString> AllowedTickingActorClassPaths;
		TArray<FString> AllowedTickingComponentClassPaths;
		TArray<FString> AllowedNiagaraDataInterfaceClassPaths;
		TArray<FString> AllowedMaterialExpressionClassPaths;
		TArray<FString> ControlledTickingActors;
		TArray<FString> AllowedTickingActors;
		TArray<FString> UncontrolledTickingActors;
		TArray<FString> ControlledTickingComponents;
		TArray<FString> AllowedTickingComponents;
		TArray<FString> UncontrolledTickingComponents;
		TArray<FString> NiagaraDataInterfaces;
		TArray<FString> AllowedNiagaraDataInterfaces;
		TArray<FString> UncontrolledNiagaraDataInterfaces;
		TArray<FString> ControlledMaterialInputs;
		TArray<FString> AllowedMaterialInputs;
		TArray<FString> UncontrolledMaterialInputs;
	};

	struct FNiagaraComponentState
	{
		FString CacheComponentPath;
		uint8 AgeUpdateMode = 0;
		int32 RandomSeedOffset = 0;
		float SeekDelta = 0.0f;
		float MaxSimTime = 0.0f;
		bool bForceSolo = false;
		bool bLockSeekDelta = false;
		bool bCanRenderWhileSeeking = true;
		bool bHadSimCache = false;
		bool bSimCacheReplaced = false;
	};

	struct FNiagaraSystemState
	{
		struct FEmitterState
		{
			FGuid HandleId;
			bool bDeterminism = false;
			int32 RandomSeed = 0;
		};

		bool bDeterminism = false;
		bool bFixedTickDelta = false;
		int32 RandomSeed = 0;
		float FixedTickDeltaTime = 0.0f;
		TArray<FEmitterState> Emitters;
	};

	struct FSkeletalEndpointState
	{
		FString ComponentPath;
		FString SkinnedAssetPath;
		TArray<FTransform> ComponentSpaceTransforms;
		TArray<uint8> BoneVisibilityStates;
	};

	struct FPrimitiveStencilState
	{
		bool bRenderCustomDepth = false;
		uint8 CustomDepthStencilValue = 0;
		uint8 CustomDepthStencilWriteMask = 0;
	};

	struct FStableInstanceIdRecord
	{
		int32 InstanceId = 0;
		int32 FirstSeenLogicalFrame = 0;
		FString ComponentPath;
		FString ActorPath;
		FString ActorClassPath;
		FString ComponentClassPath;
	};

	struct FControllableStateCacheRecord
	{
		FString ActorPath;
		FString ActorClassPath;
		FString CanonicalState;
		FString StateSha1;
		int32 Utf8Bytes = 0;
	};

	struct FControllableStateCacheFrame
	{
		int32 LogicalFrame = INDEX_NONE;
		FString AggregateSha1;
		TArray<FControllableStateCacheRecord> Actors;
	};

	struct FControllableStateCacheFrameMetrics
	{
		int32 ActorCount = 0;
		int32 AppliedActorCount = 0;
		int32 VerifiedActorCount = 0;
		int32 ChangedBeforeApplyActorCount = 0;
		FString AggregateSha1;
	};

	struct FNiagaraSimCacheMetadata
	{
		FString ComponentPath;
		FString AssetPath;
		FString SerializedSha1;
		int32 FrameCount = 0;
		int32 CPUEmitterCount = 0;
		int32 GPUEmitterCount = 0;
		int64 CachedParticleSamples = 0;
		int64 CachedGPUParticleSamples = 0;
		bool bWriteBegun = false;
		bool bWriteFinished = false;
	};

	struct FNiagaraSimCacheFrameMetrics
	{
		int32 CacheFrameIndex = INDEX_NONE;
		int32 ComponentCount = 0;
		int32 RecordedComponentCount = 0;
		int32 AppliedComponentCount = 0;
		int32 VerifiedComponentCount = 0;
		int32 CPUEmitterCount = 0;
		int32 GPUEmitterCount = 0;
		int64 CachedParticleCount = 0;
		int64 CachedGPUParticleCount = 0;
	};

	struct FChaosRigidBodyComponentMetadata
	{
		FString ComponentPath;
		FString ActorPath;
		FString ComponentClassPath;
		FString StaticMeshPath;
		FName CacheName = NAME_None;
		FString SerializedSha1;
		int32 RecordedFrameCount = 0;
		int32 ParticleTrackCount = 0;
		int32 TransformKeyCount = 0;
		FTransform InitialWorldTransform = FTransform::Identity;
		FTransform RuntimeOriginalWorldTransform = FTransform::Identity;
		FVector RuntimeOriginalLinearVelocity = FVector::ZeroVector;
		FVector RuntimeOriginalAngularVelocityDegrees = FVector::ZeroVector;
		bool bRuntimeOriginalAwake = false;
		TWeakObjectPtr<UStaticMeshComponent> Component;
	};

	struct FChaosRigidBodyPose
	{
		FString ComponentPath;
		FTransform WorldTransform = FTransform::Identity;
		bool bAwake = false;
	};

	struct FChaosRigidBodyFrame
	{
		int32 LogicalFrame = INDEX_NONE;
		int32 FixtureCollisionCount = 0;
		FString AggregateSha1;
		TArray<FChaosRigidBodyPose> Components;
	};

	struct FChaosRigidBodyFrameMetrics
	{
		int32 CacheFrameIndex = INDEX_NONE;
		int32 ComponentCount = 0;
		int32 RecordedComponentCount = 0;
		int32 AppliedComponentCount = 0;
		int32 VerifiedComponentCount = 0;
		int32 MovingComponentCount = 0;
		int32 FixtureCollisionCount = 0;
		double MaxTranslationErrorCm = 0.0;
		double MaxRotationErrorDegrees = 0.0;
		FString AggregateSha1;
	};

	void HandleWorldPreActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void HandleWorldTickEnd(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	bool PrepareJob(FString& OutError);
	bool TryReuseCompletedSpatialDataset(const FSRDatasetCaptureJob& Job, bool& bReused, FString& OutError);
	bool PrepareSemanticValidationFixture(FString& OutError);
	void RestoreSemanticValidationFixture();
	bool IsReverseEndpointReplay() const;
	int32 GetEvaluationDirection() const;
	int32 GetInitialEvaluationFrame() const;
	int32 GetFirstCapturedFrame() const;
	int32 GetPreviouslyCapturedFrame(int32 FrameNumber) const;
	int32 GetTemporalJitterOverrideIndex(int32 FrameNumber) const;
	uint32 GetLogicalViewStateFrameIndex(int32 FrameNumber) const;
	bool IsPastEvaluationRange(int32 FrameNumber) const;
	bool ShouldCaptureFrame(int32 FrameNumber) const;
	void ApplyLastCapturedEndpointTransforms();
	void SnapshotCapturedEndpointTransforms();
	bool PrepareNonFixtureSkeletalValidation(FString& OutError);
	void RestoreNonFixtureSkeletalValidation();
	void CacheSkeletalPosesForLogicalFrame(int32 FrameNumber);
	void ApplyCachedSkeletalPoses(int32 FrameNumber);
	bool LoadSkeletalPoseCacheArtifact(FString& OutError);
	bool SaveSkeletalPoseCacheArtifact(FString& OutError);
	bool PrepareControllableStateCacheValidation(FString& OutError);
	void RestoreControllableStateCacheValidation();
	bool CaptureControllableStateCacheFrame(int32 FrameNumber, FString& OutError);
	bool ApplyControllableStateCacheFrame(int32 FrameNumber, FString& OutError);
	bool LoadControllableStateCacheArtifact(FString& OutError);
	bool SaveControllableStateCacheArtifact(FString& OutError);
	static FString ComputeControllableStateCacheFrameSha1(const FControllableStateCacheFrame& Frame);
	bool CaptureNiagaraSimCacheFrame(int32 FrameNumber, FString& OutError);
	bool ValidateNiagaraSimCachePlaybackFrame(int32 FrameNumber, FString& OutError);
	bool LoadNiagaraSimCacheArtifact(FString& OutError);
	bool SaveNiagaraSimCacheArtifact(FString& OutError);
	bool PrepareChaosRigidBodyCache(FString& OutError);
	bool StartChaosRigidBodyCache(FString& OutError);
	bool CaptureOrValidateChaosRigidBodyFrame(int32 FrameNumber, FString& OutError);
	bool LoadChaosRigidBodyCacheArtifact(FString& OutError);
	bool SaveChaosRigidBodyCacheArtifact(FString& OutError);
	void RestoreChaosRigidBodyCache();
	static FString ComputeChaosRigidBodyFrameSha1(const FChaosRigidBodyFrame& Frame);
	FString ResolveProjectFile(const FString& InPath) const;
	void PositionNonFixtureSkeletalValidationActor();
	bool PrepareProjectAnimatedMaterialValidation(FString& OutError);
	void PositionProjectAnimatedMaterialValidationReceiver();
	void RestoreProjectAnimatedMaterialValidation();
	bool PrepareDynamicInstanceIdValidation(FString& OutError);
	bool UpdateDynamicInstanceIdValidation(int32 FrameNumber, FString& OutError);
	void RestoreDynamicInstanceIdValidation();
	bool PrepareStableInstanceIds(FString& OutError);
	bool ValidateStableInstanceIds(FString& OutError);
	bool RegisterStableInstanceComponent(class UPrimitiveComponent* Component, int32 FirstSeenLogicalFrame, FString& OutError);
	bool RefreshStableInstanceIdMapping(FString& OutError);
	void UpdateStableInstanceMappingManifestReferences();
	bool WriteStableInstanceIdMap(FString& OutError) const;
	void RestoreStableInstanceIds();
	TArray<class UPrimitiveComponent*> CollectStableInstanceComponents() const;
	bool PrepareDeterministicCamera(FString& OutError);
	void EnforceDeterministicCamera(int32 LogicalFrame);
	void RestoreDeterministicCamera();
	void ApplyDeterministicRuntimeState();
	void ApplyLogicalTemporalJitter(int32 FrameNumber);
	void ClearLogicalViewStateFrameIndex();
	void ApplyLogicalMaterialTime(int32 FrameNumber);
	void ClearLogicalMaterialTime();
	TArray<FString> GetActiveWidgetComponentPaths() const;
	bool CheckWidgetComponentPolicy(FString& OutError) const;
	void RestoreDeterministicRuntimeState();
	void SnapshotProvenance();
	bool EnsureStreamingReady(FString& OutError);
	FString ComputeStreamingStateSha1(int32& OutTextureCount, int32& OutPendingTextureCount) const;
	FSceneStateSummary ComputeSceneStateSummary() const;
	bool ValidateControllableStates(FString& OutError) const;
	bool RunSceneControlPreflight(FString& OutError);
	bool WriteSceneControlPreflightReport(FString& OutError) const;
	static bool MatchesSceneControlClassRule(const FString& ClassPath, const TArray<FString>& Rules);
	bool DiscoverAndControlNiagara(float TimeSeconds, FString& OutError);
	void FinalizeNiagaraForCapture();
	void RestoreNiagara();
	void NotifyControllablesPrepare();
	void NotifyControllablesEvaluate(int32 FrameNumber, float TimeSeconds);
	void NotifyControllablesRestore();
	bool EvaluateSequence(int32 FrameNumber, FString& OutError);
	bool UpdateCaptureCamera(bool bEvaluateValidationFixture, FString& OutError);
	bool CaptureCurrentFrame(FString& OutError);
	bool FinalizePendingMainViewCapture(FString& OutError);
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
	FString PluginBinarySha1;
	FString LoadedContentSha1;
	bool bMaterialShadersReadyAfterWarmup = false;
	TMap<FString, FString> LoadedContentPackageHashes;
	FString ShaderSourceSha1;
	FSceneControlPreflightReport SceneControlPreflight;
	bool bStreamingBarrierComplete = false;
	int32 StreamingRequestsAfterBarrier = INDEX_NONE;
	int32 StreamingTextureCountAfterBarrier = 0;
	int32 PendingStreamingTextureCountAfterBarrier = 0;
	FString StreamingStateAfterBarrierSha1;
	FString SkeletalPoseCacheArtifactSha1;
	bool bSkeletalPoseCacheLoadedFromArtifact = false;
	FString ControllableStateCacheArtifactSha1;
	bool bControllableStateCacheLoadedFromArtifact = false;
	FString NiagaraSimCacheArtifactSha1;
	bool bNiagaraSimCacheLoadedFromArtifact = false;
	FString ChaosRigidBodyCacheArtifactSha1;
	bool bChaosRigidBodyCacheLoadedFromArtifact = false;
	bool bChaosRigidBodyCacheStarted = false;
	FDelegateHandle PreActorTickHandle;
	FDelegateHandle PostActorTickHandle;
	FDelegateHandle WorldTickEndHandle;

	UPROPERTY(Transient)
	TObjectPtr<ASRDatasetCaptureRig> CaptureRig;

	UPROPERTY(Transient)
	TObjectPtr<ASRDatasetValidationFixture> ValidationFixture;

	UPROPERTY(Transient)
	TObjectPtr<ASRDatasetChaosValidationActor> ChaosValidationFixture;

	UPROPERTY(Transient)
	TObjectPtr<AChaosCacheManager> ChaosCacheManager;

	UPROPERTY(Transient)
	TObjectPtr<UChaosCacheCollection> ChaosCacheCollection;

	UPROPERTY(Transient)
	TObjectPtr<ASRDatasetStateCacheValidationActor> ControllableStateCacheValidationActor;

	UPROPERTY(Transient)
	TObjectPtr<ULevelSequencePlayer> SequencePlayer;

	UPROPERTY(Transient)
	TObjectPtr<ALevelSequenceActor> SequenceActor;

	UPROPERTY(Transient)
	TObjectPtr<AActor> NonFixtureSkeletalValidationActor;

	UPROPERTY(Transient)
	TObjectPtr<AActor> ProjectAnimatedMaterialValidationReceiver;

	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> ProjectAnimatedMaterialValidationComponent;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> ProjectAnimatedMaterialValidationInterface;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UNiagaraSimCache>> NiagaraSimCachesByComponentPath;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UNiagaraSimCache>> PreviousNiagaraSimCachesByComponentPath;

	FString ProjectAnimatedMaterialValidationBasePath;

	UPROPERTY(Transient)
	TObjectPtr<AActor> DynamicInstanceIdValidationActor;

	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> DynamicInstanceIdValidationComponent;

	FString DynamicInstanceIdValidationComponentPath;

	UPROPERTY(Transient)
	TObjectPtr<ACameraActor> DeterministicCameraActor;

	TWeakObjectPtr<APlayerController> DeterministicCameraPlayerController;
	TWeakObjectPtr<AActor> PreviousPlayerViewTarget;

	/** Non-UObject Slate fixture; installed only for the semantic UI gate. */
	TSharedPtr<SWidget> ValidationUIWidget;

	TMap<TWeakObjectPtr<UNiagaraComponent>, FNiagaraComponentState> NiagaraComponentStates;
	TMap<TWeakObjectPtr<UNiagaraSystem>, FNiagaraSystemState> NiagaraSystemStates;
	TSet<TWeakObjectPtr<AActor>> PreparedControllables;
	TMap<TWeakObjectPtr<AActor>, bool> ValidationHiddenActorStates;
	TMap<TWeakObjectPtr<USceneComponent>, FTransform> LastCapturedEndpointTransforms;
	TMap<TWeakObjectPtr<USkinnedMeshComponent>, FSkeletalEndpointState> LastCapturedEndpointBoneStates;
	TMap<int32, TMap<TWeakObjectPtr<USkinnedMeshComponent>, FSkeletalEndpointState>> CachedSkeletalPoseFrames;
	TMap<int32, FControllableStateCacheFrame> ControllableStateCacheFrames;
	TMap<int32, FControllableStateCacheFrameMetrics> ControllableStateCacheFrameMetrics;
	TMap<FString, FNiagaraSimCacheMetadata> NiagaraSimCacheMetadataByComponentPath;
	TMap<int32, FNiagaraSimCacheFrameMetrics> NiagaraSimCacheFrameMetrics;
	TMap<FString, FChaosRigidBodyComponentMetadata> ChaosRigidBodyMetadataByComponentPath;
	TMap<int32, FChaosRigidBodyFrame> ChaosRigidBodyFrames;
	TMap<int32, FChaosRigidBodyFrameMetrics> ChaosRigidBodyFrameMetrics;
	TMap<TWeakObjectPtr<USkinnedMeshComponent>, int32> NonFixtureSkeletalObjectIds;
	TMap<TWeakObjectPtr<class UPrimitiveComponent>, FPrimitiveStencilState> NonFixtureSkeletalStencilStates;
	TMap<TWeakObjectPtr<class UPrimitiveComponent>, FPrimitiveStencilState> StableInstanceStencilStates;
	TArray<FStableInstanceIdRecord> StableInstanceIdRecords;
	TMap<FString, int32> StableInstanceIdByComponentPath;
	TArray<int32> StableInstanceActiveIds;
	TArray<int32> StableInstanceNewIds;
	FString StableInstanceIdMappingSha1;
	bool bStableInstanceIdsPrepared = false;
	TMap<TWeakObjectPtr<AActor>, bool> NonFixtureHiddenActorStates;
	int32 WarmupPoseCacheFrame = INDEX_NONE;
	int32 AppliedCachedSkeletalPoseComponentCount = 0;
	int32 AppliedCachedSkeletalPoseBoneCount = 0;
	TArray<FString> SkippedCachedSkeletalPoseComponents;
	int32 AppliedEndpointBoneComponentCount = 0;
	int32 AppliedEndpointBoneCount = 0;
	TArray<FString> SkippedEndpointBoneComponents;
	TArray<TSharedPtr<FJsonValue>> ManifestFrames;
};
