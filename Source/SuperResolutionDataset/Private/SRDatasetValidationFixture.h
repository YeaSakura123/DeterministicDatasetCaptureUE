#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/Actor.h"
#include "SRDatasetTypes.h"
#include "SRDatasetValidationFixture.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UNiagaraComponent;
class UNiagaraSystem;
class UPoseableMeshComponent;
class UPrimitiveComponent;
class UStaticMeshComponent;

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
	float NiagaraValidationRadiusDisplayPixels = 0.0f;
	bool bNiagaraVisibleProbeExpected = false;
	TMap<int32, float> ExpectedFrontDepthMeters;
};

/** A transient chart used only by the automated semantic capture gate. */
UCLASS(NotBlueprintable, Transient)
class ASRDatasetValidationFixture final : public AActor
{
	GENERATED_BODY()

public:
	ASRDatasetValidationFixture();

	bool Configure(FString& OutError);
	void Evaluate(
		const FMinimalViewInfo& CameraView,
		int32 LogicalFrame,
		int32 StartFrame,
		FIntPoint DisplaySize,
		bool bUseLastCapturedEndpointAsPrevious,
		ESRDatasetSemanticMotionScenario MotionScenario);
	void CommitCapturedFrame();
	const FSRDatasetValidationFixtureFrame& GetFrameMetadata() const { return FrameMetadata; }

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

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> MovingCube;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> BackgroundPanel;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> DepthOneMeterPanel;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> DepthTenMetersPanel;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> DepthHundredMetersPanel;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> TranslucentPanel;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPoseableMeshComponent> SkeletalCube;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> WPOCube;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UNiagaraComponent> NiagaraFixture;

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
