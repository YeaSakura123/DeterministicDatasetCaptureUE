#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/Actor.h"
#include "SRDatasetControllable.h"
#include "SRDatasetTypes.h"
#include "SRDatasetValidationFixture.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UNiagaraComponent;
class UNiagaraSystem;
class UPoseableMeshComponent;
class UPrimitiveComponent;
class UStaticMeshComponent;
struct FHitResult;

struct FSRDatasetValidationFixtureFrame
{
	bool bValid = false;
	int32 LogicalFrame = INDEX_NONE;
	ESRDatasetSemanticMotionScenario MotionScenario = ESRDatasetSemanticMotionScenario::LegacyCameraRelative;
	bool bWorldAnchored = false;
	bool bObjectMotionEnabled = true;
	FVector CurrentCameraLocationCm = FVector::ZeroVector;
	FVector PreviousCameraLocationCm = FVector::ZeroVector;
	float MovingCurrentRightCm = 0.0f;
	float MovingPreviousRightCm = 0.0f;
	FVector2f ExpectedMovingMotionDisplayPixels = FVector2f::ZeroVector;
	FVector2f ExpectedBackgroundMotionDisplayPixels = FVector2f::ZeroVector;
	float SkeletalCurrentRightCm = 0.0f;
	float SkeletalPreviousRightCm = 0.0f;
	FVector2f ExpectedSkeletalMotionDisplayPixels = FVector2f::ZeroVector;
	float WPOCurrentRightCm = 0.0f;
	float WPOPreviousRightCm = 0.0f;
	FVector2f ExpectedWPOMotionDisplayPixels = FVector2f::ZeroVector;
	FVector2f NiagaraAnchorDisplayPixels = FVector2f::ZeroVector;
	FVector2f NiagaraGPUAnchorDisplayPixels = FVector2f::ZeroVector;
	float NiagaraValidationRadiusDisplayPixels = 0.0f;
	bool bNiagaraVisibleProbeExpected = false;
	bool bNiagaraGPUVisibleProbeExpected = false;
	TMap<int32, float> ExpectedFrontDepthMeters;
};

/** A transient chart used only by the automated semantic capture gate. */
UCLASS(NotBlueprintable, Transient)
class ASRDatasetValidationFixture final : public AActor, public ISRDatasetControllable
{
	GENERATED_BODY()

public:
	ASRDatasetValidationFixture();

	bool Configure(bool bEnableGPUNiagaraProbe, FString& OutError);
	void Evaluate(
		const FMinimalViewInfo& CameraView,
		int32 LogicalFrame,
		int32 StartFrame,
		FIntPoint DisplaySize,
		bool bUseLastCapturedEndpointAsPrevious,
		ESRDatasetSemanticMotionScenario MotionScenario);
	void CommitCapturedFrame();
	const FSRDatasetValidationFixtureFrame& GetFrameMetadata() const { return FrameMetadata; }
	virtual FString DatasetGetDeterministicState_Implementation() override;

	static constexpr int32 MovingObjectId = 1;
	static constexpr int32 BackgroundObjectId = 2;
	static constexpr int32 DepthOneMeterObjectId = 11;
	static constexpr int32 DepthTenMetersObjectId = 12;
	static constexpr int32 DepthHundredMetersObjectId = 13;
	static constexpr int32 TranslucentObjectId = 21;
	static constexpr int32 SkeletalObjectId = 31;
	static constexpr int32 WPOObjectId = 41;

private:
	static void ConfigurePrimitive(UPrimitiveComponent* Component, int32 StencilValue);
	static void PlaceBox(
		UStaticMeshComponent* Component,
		const FMinimalViewInfo& CameraView,
		float ForwardCm,
		float RightCm,
		float UpCm,
		FVector SizeCm);
	static bool ProjectWorldToDisplayPixels(
		const FVector& WorldPosition,
		const FMinimalViewInfo& CameraView,
		FIntPoint DisplaySize,
		FVector2f& OutDisplayPixels);

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> MovingCube;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> BackgroundPanel;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> DepthOneMeterPanel;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> DepthTenMetersPanel;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> DepthHundredMetersPanel;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> TranslucentPanel;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UPoseableMeshComponent> SkeletalCube;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> WPOCube;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UNiagaraComponent> NiagaraFixture;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> NiagaraGPUFixture;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> OpaqueMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> TranslucentMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WPOBaseMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> VFXSpriteMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> WPOMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> NiagaraSystem;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> NiagaraGPUSystem;

	FSRDatasetValidationFixtureFrame FrameMetadata;
	bool bHasWorldAnchor = false;
	FMinimalViewInfo WorldAnchorCameraView;
	int32 LastEvaluatedFrame = INDEX_NONE;
	bool bHasLastEvaluatedCameraView = false;
	FMinimalViewInfo LastEvaluatedCameraView;
	bool bHasCapturedCameraView = false;
	FMinimalViewInfo LastCapturedCameraView;
	float LastMovingRightCm = 0.0f;
	bool bHasCapturedMovingRight = false;
	float LastCapturedMovingRightCm = 0.0f;
	float LastSkeletalRightCm = 0.0f;
	bool bHasCapturedSkeletalRight = false;
	float LastCapturedSkeletalRightCm = 0.0f;
	float LastWPORightCm = 0.0f;
	bool bHasCapturedWPORight = false;
	float LastCapturedWPORightCm = 0.0f;
};

/**
 * Validation-only opaque gameplay/VFX state probe. Its process-local seed is
 * intentionally different on each launch; only a loaded controllable-state
 * cache can restore the recorded transform and canonical private value.
 */
UCLASS(NotBlueprintable, Transient)
class ASRDatasetStateCacheValidationActor final : public AActor, public ISRDatasetControllable
{
	GENERATED_BODY()

public:
	ASRDatasetStateCacheValidationActor();

	bool Configure(const FMinimalViewInfo& CameraView, FString& OutError);
	virtual void DatasetPrepare_Implementation(int32 RandomSeed, float FixedDeltaSeconds) override;
	virtual void DatasetEvaluateFrame_Implementation(int32 FrameNumber, float TimeSeconds) override;
	virtual FString DatasetGetDeterministicState_Implementation() override;
	virtual bool DatasetApplyDeterministicState_Implementation(const FString& CanonicalState) override;
	virtual void DatasetRestore_Implementation() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<USceneComponent> StateCacheRoot;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> StateCacheProbe;

	FTransform OriginalTransform = FTransform::Identity;
	FVector BaseLocationCm = FVector::ZeroVector;
	FVector CameraRight = FVector::RightVector;
	uint64 ProcessNonce = 0;
	uint32 PrivateValue = 0;
	int32 LogicalFrame = INDEX_NONE;
	bool bConfigured = false;
};

/**
 * Non-transient, validation-only Chaos fixture. ChaosCaching intentionally
 * rejects RF_Transient components, so the subsystem owns and destroys this
 * actor explicitly instead of relying on transient object flags.
 */
UCLASS(NotBlueprintable)
class ASRDatasetChaosValidationActor final : public AActor
{
	GENERATED_BODY()

public:
	ASRDatasetChaosValidationActor();

	bool Configure(const FVector& CameraLocationCm, const FRotator& CameraRotationDegrees, FString& OutError);
	void ArmForCapture();
	TArray<UStaticMeshComponent*> GetDynamicComponents() const;
	int32 GetCollisionCount() const { return CollisionCount; }

private:
	UFUNCTION()
	void HandlePhysicsHit(
		UPrimitiveComponent* HitComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		FVector NormalImpulse,
		const FHitResult& Hit);

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<USceneComponent> PhysicsRoot;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> Ground;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> DynamicCubeA;

	UPROPERTY(VisibleAnywhere, Category = "Dataset Validation")
	TObjectPtr<UStaticMeshComponent> DynamicCubeB;

	FTransform InitialTransformA = FTransform::Identity;
	FTransform InitialTransformB = FTransform::Identity;
	FVector InitialVelocityA = FVector::ZeroVector;
	FVector InitialVelocityB = FVector::ZeroVector;
	int32 CollisionCount = 0;
	bool bConfigured = false;
};
