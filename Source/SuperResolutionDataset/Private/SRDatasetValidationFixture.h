#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraTypes.h"
#include "GameFramework/Actor.h"
#include "SRDatasetValidationFixture.generated.h"

class UMaterialInterface;
class UStaticMeshComponent;

struct FSRDatasetValidationFixtureFrame
{
	bool bValid = false;
	int32 LogicalFrame = INDEX_NONE;
	float MovingCurrentRightCm = 0.0f;
	float MovingPreviousRightCm = 0.0f;
	FVector2f ExpectedMovingMotionDisplayPixels = FVector2f::ZeroVector;
	TMap<int32, float> ExpectedFrontDepthMeters;
};

/** A transient chart used only by the automated semantic capture gate. */
UCLASS(NotBlueprintable, Transient)
class ASRDatasetValidationFixture final : public AActor
{
	GENERATED_BODY()

public:
	ASRDatasetValidationFixture();

	bool Configure(FString& OutError) const;
	void Evaluate(
		const FMinimalViewInfo& CameraView,
		int32 LogicalFrame,
		int32 StartFrame,
		FIntPoint DisplaySize,
		bool bUseLastCapturedEndpointAsPrevious);
	void CommitCapturedFrame();
	const FSRDatasetValidationFixtureFrame& GetFrameMetadata() const { return FrameMetadata; }

	static constexpr int32 MovingObjectId = 1;
	static constexpr int32 BackgroundObjectId = 2;
	static constexpr int32 DepthOneMeterObjectId = 11;
	static constexpr int32 DepthTenMetersObjectId = 12;
	static constexpr int32 DepthHundredMetersObjectId = 13;
	static constexpr int32 TranslucentObjectId = 21;

private:
	static void ConfigurePrimitive(UStaticMeshComponent* Component, int32 StencilValue);
	static void PlaceBox(
		UStaticMeshComponent* Component,
		const FMinimalViewInfo& CameraView,
		float ForwardCm,
		float RightCm,
		float UpCm,
		FVector SizeCm);

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

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> OpaqueMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> TranslucentMaterial;

	FSRDatasetValidationFixtureFrame FrameMetadata;
	int32 LastEvaluatedFrame = INDEX_NONE;
	float LastMovingRightCm = 0.0f;
	bool bHasCapturedMovingRight = false;
	float LastCapturedMovingRightCm = 0.0f;
};
