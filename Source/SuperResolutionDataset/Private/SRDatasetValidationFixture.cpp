#include "SRDatasetValidationFixture.h"

#include "Components/SceneComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/ConstructorHelpers.h"

namespace SRDataset::ValidationFixture
{
	constexpr float CubeMeshExtentCm = 100.0f;
	constexpr float MovingForwardCm = 500.0f;
	constexpr float MovingHalfExtentCm = 50.0f;
	constexpr float MovingLeftCm = -125.0f;
	constexpr float MovingRightCm = 125.0f;
	constexpr float SkeletalForwardCm = 600.0f;
	constexpr float SkeletalMeshHalfExtentCm = 12.5f;
	constexpr float SkeletalScale = 2.5f;
	constexpr float SkeletalComponentRightCm = -150.0f;
	constexpr float SkeletalComponentUpCm = -100.0f;
	constexpr float SkeletalBoneLeftCm = -40.0f;
	constexpr float SkeletalBoneRightCm = 40.0f;
	constexpr float WPOForwardCm = 550.0f;
	constexpr float WPOHalfExtentCm = 30.0f;
	constexpr float WPOComponentRightCm = 140.0f;
	constexpr float WPOComponentUpCm = 75.0f;
	constexpr float WPOLeftCm = -35.0f;
	constexpr float WPORightCm = 35.0f;
	constexpr float PanelThicknessCm = 2.0f;
	const FName SkeletalRootBoneName(TEXT("Bone01"));
	const FName WPOCurrentWorldParameter(TEXT("SRDatasetWPOCurrentWorldCm"));
	const FName WPOPreviousWorldParameter(TEXT("SRDatasetWPOPreviousWorldCm"));
}

ASRDatasetValidationFixture::ASRDatasetValidationFixture()
{
	PrimaryActorTick.bCanEverTick = false;
	SetActorEnableCollision(false);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	MovingCube = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MovingCube"));
	BackgroundPanel = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BackgroundPanel"));
	DepthOneMeterPanel = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DepthOneMeterPanel"));
	DepthTenMetersPanel = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DepthTenMetersPanel"));
	DepthHundredMetersPanel = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DepthHundredMetersPanel"));
	TranslucentPanel = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TranslucentPanel"));
	SkeletalCube = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("SkeletalCube"));
	WPOCube = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WPOCube"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMeshFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> SkeletalCubeFinder(
		TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> OpaqueMaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial_Inst.BasicShapeMaterial_Inst"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TranslucentMaterialFinder(
		TEXT("/Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent.M_SimpleUnlitTranslucent"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WPOBaseMaterialFinder(
		TEXT("/SuperResolutionDataset/Validation/M_SRDatasetWPOFixture.M_SRDatasetWPOFixture"));
	OpaqueMaterial = OpaqueMaterialFinder.Object;
	TranslucentMaterial = TranslucentMaterialFinder.Object;
	WPOBaseMaterial = WPOBaseMaterialFinder.Object;

	for (UStaticMeshComponent* Component : {
		MovingCube.Get(), BackgroundPanel.Get(), DepthOneMeterPanel.Get(), DepthTenMetersPanel.Get(),
		DepthHundredMetersPanel.Get(), TranslucentPanel.Get(), WPOCube.Get() })
	{
		Component->SetupAttachment(SceneRoot);
		Component->SetStaticMesh(CubeMeshFinder.Object);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCastShadow(false);
		Component->SetReceivesDecals(false);
	}

	ConfigurePrimitive(MovingCube, MovingObjectId);
	ConfigurePrimitive(BackgroundPanel, BackgroundObjectId);
	ConfigurePrimitive(DepthOneMeterPanel, DepthOneMeterObjectId);
	ConfigurePrimitive(DepthTenMetersPanel, DepthTenMetersObjectId);
	ConfigurePrimitive(DepthHundredMetersPanel, DepthHundredMetersObjectId);
	ConfigurePrimitive(TranslucentPanel, TranslucentObjectId);
	ConfigurePrimitive(WPOCube, WPOObjectId);

	for (UStaticMeshComponent* Component : {
		MovingCube.Get(), BackgroundPanel.Get(), DepthOneMeterPanel.Get(), DepthTenMetersPanel.Get(),
		DepthHundredMetersPanel.Get() })
	{
		Component->SetMaterial(0, OpaqueMaterial);
	}
	TranslucentPanel->SetMaterial(0, TranslucentMaterial);
	WPOCube->SetMaterial(0, WPOBaseMaterial);
	WPOCube->bEvaluateWorldPositionOffset = true;
	WPOCube->bWorldPositionOffsetWritesVelocity = true;
	WPOCube->SetBoundsScale(4.0f);

	SkeletalCube->SetupAttachment(SceneRoot);
	SkeletalCube->SetSkinnedAssetAndUpdate(SkeletalCubeFinder.Object);
	SkeletalCube->SetMobility(EComponentMobility::Movable);
	SkeletalCube->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkeletalCube->SetGenerateOverlapEvents(false);
	SkeletalCube->SetCastShadow(false);
	SkeletalCube->SetReceivesDecals(false);
	SkeletalCube->SetMaterial(0, OpaqueMaterial);
	ConfigurePrimitive(SkeletalCube, SkeletalObjectId);
}

bool ASRDatasetValidationFixture::Configure(FString& OutError)
{
	if (!MovingCube->GetStaticMesh() || !OpaqueMaterial || !TranslucentMaterial || !WPOBaseMaterial ||
		!SkeletalCube->GetSkinnedAsset() ||
		SkeletalCube->GetBoneIndex(SRDataset::ValidationFixture::SkeletalRootBoneName) == INDEX_NONE)
	{
		OutError = TEXT("Semantic validation fixture could not load its Engine mesh/material assets or plugin WPO material.");
		return false;
	}
	WPOMaterial = UMaterialInstanceDynamic::Create(WPOBaseMaterial, this);
	if (!WPOMaterial)
	{
		OutError = TEXT("Semantic validation fixture could not create its WPO material instance.");
		return false;
	}
	WPOCube->SetMaterial(0, WPOMaterial);
	OutError.Reset();
	return true;
}

void ASRDatasetValidationFixture::ConfigurePrimitive(UPrimitiveComponent* Component, const int32 StencilValue)
{
	Component->SetRenderCustomDepth(true);
	Component->SetCustomDepthStencilWriteMask(ERendererStencilMask::ERSM_Default);
	Component->SetCustomDepthStencilValue(StencilValue);
}

void ASRDatasetValidationFixture::PlaceBox(
	UStaticMeshComponent* Component,
	const FMinimalViewInfo& CameraView,
	const float ForwardCm,
	const float RightCm,
	const float UpCm,
	const FVector SizeCm)
{
	const FRotationMatrix CameraBasis(CameraView.Rotation);
	const FVector Forward = CameraBasis.GetScaledAxis(EAxis::X);
	const FVector Right = CameraBasis.GetScaledAxis(EAxis::Y);
	const FVector Up = CameraBasis.GetScaledAxis(EAxis::Z);
	const FVector Location = CameraView.Location + Forward * ForwardCm + Right * RightCm + Up * UpCm;
	Component->SetWorldTransform(FTransform(
		CameraView.Rotation,
		Location,
		SizeCm / SRDataset::ValidationFixture::CubeMeshExtentCm));
}

void ASRDatasetValidationFixture::Evaluate(
	const FMinimalViewInfo& CameraView,
	const int32 LogicalFrame,
	const int32 StartFrame,
	const FIntPoint DisplaySize,
	const bool bUseLastCapturedEndpointAsPrevious)
{
	using namespace SRDataset::ValidationFixture;
	const float EndpointPhase = FMath::Clamp(0.5f * static_cast<float>(LogicalFrame - StartFrame), 0.0f, 1.0f);
	const float MovingRight = FMath::Lerp(MovingLeftCm, MovingRightCm, EndpointPhase);
	const float SkeletalRight = FMath::Lerp(SkeletalBoneLeftCm, SkeletalBoneRightCm, EndpointPhase);
	const float WPORight = FMath::Lerp(WPOLeftCm, WPORightCm, EndpointPhase);
	const float PreviousRight = bUseLastCapturedEndpointAsPrevious && bHasCapturedMovingRight
		? LastCapturedMovingRightCm
		: (LastEvaluatedFrame == INDEX_NONE || LastEvaluatedFrame == LogicalFrame ? MovingRight : LastMovingRightCm);
	const float PreviousSkeletalRight = bUseLastCapturedEndpointAsPrevious && bHasCapturedSkeletalRight
		? LastCapturedSkeletalRightCm
		: (LastEvaluatedFrame == INDEX_NONE || LastEvaluatedFrame == LogicalFrame ? SkeletalRight : LastSkeletalRightCm);
	const float PreviousWPORight = bUseLastCapturedEndpointAsPrevious && bHasCapturedWPORight
		? LastCapturedWPORightCm
		: (LastEvaluatedFrame == INDEX_NONE || LastEvaluatedFrame == LogicalFrame ? WPORight : LastWPORightCm);

	PlaceBox(MovingCube, CameraView, MovingForwardCm, MovingRight, 0.0f, FVector(100.0f));
	PlaceBox(BackgroundPanel, CameraView, 800.0f, 0.0f, 0.0f, FVector(PanelThicknessCm, 500.0f, 360.0f));
	PlaceBox(DepthOneMeterPanel, CameraView, 100.0f, -38.0f, 27.0f, FVector(PanelThicknessCm, 14.0f, 14.0f));
	PlaceBox(DepthTenMetersPanel, CameraView, 1000.0f, 0.0f, 270.0f, FVector(PanelThicknessCm, 140.0f, 140.0f));
	PlaceBox(DepthHundredMetersPanel, CameraView, 10000.0f, 3800.0f, 2700.0f, FVector(PanelThicknessCm, 1400.0f, 1400.0f));
	PlaceBox(TranslucentPanel, CameraView, 350.0f, 115.0f, -90.0f, FVector(PanelThicknessCm, 115.0f, 115.0f));
	PlaceBox(
		WPOCube,
		CameraView,
		WPOForwardCm,
		WPOComponentRightCm,
		WPOComponentUpCm,
		FVector(2.0f * WPOHalfExtentCm));

	const FRotationMatrix CameraBasis(CameraView.Rotation);
	const FVector CameraForward = CameraBasis.GetScaledAxis(EAxis::X);
	const FVector CameraRight = CameraBasis.GetScaledAxis(EAxis::Y);
	const FVector CameraUp = CameraBasis.GetScaledAxis(EAxis::Z);
	const FVector CurrentWPOWorld = CameraRight * WPORight;
	const FVector PreviousWPOWorld = CameraRight * PreviousWPORight;
	WPOMaterial->SetVectorParameterValue(
		WPOCurrentWorldParameter,
		FLinearColor(CurrentWPOWorld.X, CurrentWPOWorld.Y, CurrentWPOWorld.Z, 0.0f));
	WPOMaterial->SetVectorParameterValue(
		WPOPreviousWorldParameter,
		FLinearColor(PreviousWPOWorld.X, PreviousWPOWorld.Y, PreviousWPOWorld.Z, 0.0f));
	SkeletalCube->SetWorldTransform(FTransform(
		CameraView.Rotation,
		CameraView.Location + CameraForward * SkeletalForwardCm +
			CameraRight * SkeletalComponentRightCm + CameraUp * SkeletalComponentUpCm,
		FVector(SkeletalScale)));
	const int32 SkeletalRootBoneIndex = SkeletalCube->GetBoneIndex(SkeletalRootBoneName);
	FTransform SkeletalRootTransform =
		SkeletalCube->GetSkinnedAsset()->GetRefSkeleton().GetRefBonePose()[SkeletalRootBoneIndex];
	SkeletalRootTransform.AddToTranslation(FVector(0.0, SkeletalRight, 0.0));
	SkeletalCube->SetBoneTransformByName(
		SkeletalRootBoneName, SkeletalRootTransform, EBoneSpaces::ComponentSpace);
	SkeletalCube->RefreshBoneTransforms();

	const float HorizontalFovRadians = FMath::DegreesToRadians(CameraView.FOV);
	// The front face is what wins the depth test and carries the visible motion
	// vector, so project the translation at its view-space depth rather than at
	// the component origin.
	const float MovingSurfaceForwardCm = MovingForwardCm - MovingHalfExtentCm;
	const float PixelsPerRightCm = 0.5f * static_cast<float>(DisplaySize.X) /
		(MovingSurfaceForwardCm * FMath::Tan(0.5f * HorizontalFovRadians));
	FrameMetadata = FSRDatasetValidationFixtureFrame();
	FrameMetadata.bValid = true;
	FrameMetadata.LogicalFrame = LogicalFrame;
	FrameMetadata.MovingCurrentRightCm = MovingRight;
	FrameMetadata.MovingPreviousRightCm = PreviousRight;
	FrameMetadata.ExpectedMovingMotionDisplayPixels = FVector2f(
		(PreviousRight - MovingRight) * PixelsPerRightCm,
		0.0f);
	const float SkeletalSurfaceForwardCm = SkeletalForwardCm - SkeletalScale * SkeletalMeshHalfExtentCm;
	const float SkeletalPixelsPerRightCm = 0.5f * static_cast<float>(DisplaySize.X) /
		(SkeletalSurfaceForwardCm * FMath::Tan(0.5f * HorizontalFovRadians));
	FrameMetadata.SkeletalCurrentRightCm = SkeletalRight;
	FrameMetadata.SkeletalPreviousRightCm = PreviousSkeletalRight;
	FrameMetadata.ExpectedSkeletalMotionDisplayPixels = FVector2f(
		(PreviousSkeletalRight - SkeletalRight) * SkeletalScale * SkeletalPixelsPerRightCm,
		0.0f);
	const float WPOSurfaceForwardCm = WPOForwardCm - WPOHalfExtentCm;
	const float WPOPixelsPerRightCm = 0.5f * static_cast<float>(DisplaySize.X) /
		(WPOSurfaceForwardCm * FMath::Tan(0.5f * HorizontalFovRadians));
	FrameMetadata.WPOCurrentRightCm = WPORight;
	FrameMetadata.WPOPreviousRightCm = PreviousWPORight;
	FrameMetadata.ExpectedWPOMotionDisplayPixels = FVector2f(
		(PreviousWPORight - WPORight) * WPOPixelsPerRightCm,
		0.0f);
	FrameMetadata.ExpectedFrontDepthMeters.Add(DepthOneMeterObjectId, (100.0f - 0.5f * PanelThicknessCm) * 0.01f);
	FrameMetadata.ExpectedFrontDepthMeters.Add(DepthTenMetersObjectId, (1000.0f - 0.5f * PanelThicknessCm) * 0.01f);
	FrameMetadata.ExpectedFrontDepthMeters.Add(DepthHundredMetersObjectId, (10000.0f - 0.5f * PanelThicknessCm) * 0.01f);

	LastEvaluatedFrame = LogicalFrame;
	LastMovingRightCm = MovingRight;
	LastSkeletalRightCm = SkeletalRight;
	LastWPORightCm = WPORight;
}

void ASRDatasetValidationFixture::CommitCapturedFrame()
{
	bHasCapturedMovingRight = true;
	LastCapturedMovingRightCm = LastMovingRightCm;
	bHasCapturedSkeletalRight = true;
	LastCapturedSkeletalRightCm = LastSkeletalRightCm;
	bHasCapturedWPORight = true;
	LastCapturedWPORightCm = LastWPORightCm;
}
