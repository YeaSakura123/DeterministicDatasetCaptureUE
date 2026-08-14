#include "SRDatasetValidationFixture.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

namespace SRDataset::ValidationFixture
{
	constexpr float CubeMeshExtentCm = 100.0f;
	constexpr float MovingForwardCm = 500.0f;
	constexpr float MovingHalfExtentCm = 50.0f;
	constexpr float MovingLeftCm = -125.0f;
	constexpr float MovingRightCm = 125.0f;
	constexpr float PanelThicknessCm = 2.0f;
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

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMeshFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> OpaqueMaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial_Inst.BasicShapeMaterial_Inst"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TranslucentMaterialFinder(
		TEXT("/Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent.M_SimpleUnlitTranslucent"));
	OpaqueMaterial = OpaqueMaterialFinder.Object;
	TranslucentMaterial = TranslucentMaterialFinder.Object;

	for (UStaticMeshComponent* Component : {
		MovingCube.Get(), BackgroundPanel.Get(), DepthOneMeterPanel.Get(), DepthTenMetersPanel.Get(),
		DepthHundredMetersPanel.Get(), TranslucentPanel.Get() })
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

	for (UStaticMeshComponent* Component : {
		MovingCube.Get(), BackgroundPanel.Get(), DepthOneMeterPanel.Get(), DepthTenMetersPanel.Get(),
		DepthHundredMetersPanel.Get() })
	{
		Component->SetMaterial(0, OpaqueMaterial);
	}
	TranslucentPanel->SetMaterial(0, TranslucentMaterial);
}

bool ASRDatasetValidationFixture::Configure(FString& OutError) const
{
	if (!MovingCube->GetStaticMesh() || !OpaqueMaterial || !TranslucentMaterial)
	{
		OutError = TEXT("Semantic validation fixture could not load required Engine cube/material assets.");
		return false;
	}
	OutError.Reset();
	return true;
}

void ASRDatasetValidationFixture::ConfigurePrimitive(UStaticMeshComponent* Component, const int32 StencilValue)
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
	const float PreviousRight = bUseLastCapturedEndpointAsPrevious && bHasCapturedMovingRight
		? LastCapturedMovingRightCm
		: (LastEvaluatedFrame == INDEX_NONE || LastEvaluatedFrame == LogicalFrame ? MovingRight : LastMovingRightCm);

	PlaceBox(MovingCube, CameraView, MovingForwardCm, MovingRight, 0.0f, FVector(100.0f));
	PlaceBox(BackgroundPanel, CameraView, 800.0f, 0.0f, 0.0f, FVector(PanelThicknessCm, 500.0f, 360.0f));
	PlaceBox(DepthOneMeterPanel, CameraView, 100.0f, -38.0f, 27.0f, FVector(PanelThicknessCm, 14.0f, 14.0f));
	PlaceBox(DepthTenMetersPanel, CameraView, 1000.0f, 0.0f, 270.0f, FVector(PanelThicknessCm, 140.0f, 140.0f));
	PlaceBox(DepthHundredMetersPanel, CameraView, 10000.0f, 3800.0f, 2700.0f, FVector(PanelThicknessCm, 1400.0f, 1400.0f));
	PlaceBox(TranslucentPanel, CameraView, 350.0f, 115.0f, -90.0f, FVector(PanelThicknessCm, 115.0f, 115.0f));

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
	FrameMetadata.ExpectedFrontDepthMeters.Add(DepthOneMeterObjectId, (100.0f - 0.5f * PanelThicknessCm) * 0.01f);
	FrameMetadata.ExpectedFrontDepthMeters.Add(DepthTenMetersObjectId, (1000.0f - 0.5f * PanelThicknessCm) * 0.01f);
	FrameMetadata.ExpectedFrontDepthMeters.Add(DepthHundredMetersObjectId, (10000.0f - 0.5f * PanelThicknessCm) * 0.01f);

	LastEvaluatedFrame = LogicalFrame;
	LastMovingRightCm = MovingRight;
}

void ASRDatasetValidationFixture::CommitCapturedFrame()
{
	bHasCapturedMovingRight = true;
	LastCapturedMovingRightCm = LastMovingRightCm;
}
