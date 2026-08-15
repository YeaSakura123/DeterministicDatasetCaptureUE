#include "SRDatasetValidationFixture.h"

#include "Components/SceneComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/SkeletalMesh.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
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
	// Keep the VFX probe off the opaque chart so translucent sprites must change
	// visible scene pixels rather than disappearing against the white panel.
	constexpr float NiagaraForwardCm = 400.0f;
	constexpr float NiagaraRightCm = -300.0f;
	constexpr float NiagaraUpCm = 140.0f;
	constexpr float NiagaraValidationRadiusAt256Pixels = 34.0f;
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
	NiagaraFixture = CreateDefaultSubobject<UNiagaraComponent>(TEXT("NiagaraVFXFixture"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMeshFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> SkeletalCubeFinder(
		TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> OpaqueMaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial_Inst.BasicShapeMaterial_Inst"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TranslucentMaterialFinder(
		TEXT("/Engine/EngineDebugMaterials/M_SimpleUnlitTranslucent.M_SimpleUnlitTranslucent"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WPOBaseMaterialFinder(
		TEXT("/SuperResolutionDataset/Validation/M_SRDatasetWPOFixture.M_SRDatasetWPOFixture"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> VFXSpriteMaterialFinder(
		TEXT("/SuperResolutionDataset/Validation/M_SRDatasetVFXSpriteFixture.M_SRDatasetVFXSpriteFixture"));
	OpaqueMaterial = OpaqueMaterialFinder.Object;
	TranslucentMaterial = TranslucentMaterialFinder.Object;
	WPOBaseMaterial = WPOBaseMaterialFinder.Object;
	VFXSpriteMaterial = VFXSpriteMaterialFinder.Object;

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

	NiagaraFixture->SetupAttachment(SceneRoot);
	NiagaraFixture->SetAutoActivate(true);
	NiagaraFixture->SetMobility(EComponentMobility::Movable);
	NiagaraFixture->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	NiagaraFixture->SetGenerateOverlapEvents(false);
	NiagaraFixture->SetCastShadow(false);
	NiagaraFixture->SetAllowScalability(false);
	NiagaraFixture->SetForceLocalPlayerEffect(true);
	NiagaraFixture->SetRenderingEnabled(true);
	NiagaraFixture->SetSystemFixedBounds(FBox(FVector(-300.0), FVector(300.0)));
}

bool ASRDatasetValidationFixture::Configure(FString& OutError)
{
	// Loading a Niagara system from the actor CDO constructor can run before the
	// Niagara runtime module reaches PreDefault in command-line -game sessions.
	// Defer it until the subsystem prepares the fixture after module startup.
	NiagaraSystem = LoadObject<UNiagaraSystem>(
		nullptr,
		TEXT("/SuperResolutionDataset/Validation/NS_SRDatasetVFXFixture.NS_SRDatasetVFXFixture"));
	if (!MovingCube->GetStaticMesh() || !OpaqueMaterial || !TranslucentMaterial || !WPOBaseMaterial ||
		!VFXSpriteMaterial ||
		!SkeletalCube->GetSkinnedAsset() || !NiagaraSystem ||
		SkeletalCube->GetBoneIndex(SRDataset::ValidationFixture::SkeletalRootBoneName) == INDEX_NONE)
	{
		OutError = TEXT("Semantic validation fixture could not load its Engine mesh/material assets or plugin WPO/Niagara assets.");
		return false;
	}
	WPOMaterial = UMaterialInstanceDynamic::Create(WPOBaseMaterial, this);
	if (!WPOMaterial)
	{
		OutError = TEXT("Semantic validation fixture could not create its WPO material instance.");
		return false;
	}
	WPOCube->SetMaterial(0, WPOMaterial);
	for (FNiagaraEmitterHandle& EmitterHandle : NiagaraSystem->GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData())
		{
			for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
			{
				if (UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer))
				{
					Sprite->Material = VFXSpriteMaterial;
				}
				else if (UNiagaraRibbonRendererProperties* Ribbon = Cast<UNiagaraRibbonRendererProperties>(Renderer))
				{
					Ribbon->Material = VFXSpriteMaterial;
				}
			}
		}
	}
	NiagaraFixture->SetAsset(NiagaraSystem);
	NiagaraFixture->Activate(true);
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

bool ASRDatasetValidationFixture::ProjectWorldToDisplayPixels(
	const FVector& WorldPosition,
	const FMinimalViewInfo& CameraView,
	const FIntPoint DisplaySize,
	FVector2f& OutDisplayPixels)
{
	const FRotationMatrix CameraBasis(CameraView.Rotation);
	const FVector Relative = WorldPosition - CameraView.Location;
	const double ForwardCm = FVector::DotProduct(
		Relative, CameraBasis.GetScaledAxis(EAxis::X));
	const double RightCm = FVector::DotProduct(
		Relative, CameraBasis.GetScaledAxis(EAxis::Y));
	const double UpCm = FVector::DotProduct(
		Relative, CameraBasis.GetScaledAxis(EAxis::Z));
	const double TanHalfFov = FMath::Tan(0.5 * FMath::DegreesToRadians(CameraView.FOV));
	if (ForwardCm <= UE_SMALL_NUMBER || DisplaySize.X <= 0 || DisplaySize.Y <= 0 ||
		!FMath::IsFinite(TanHalfFov) || TanHalfFov <= UE_SMALL_NUMBER)
	{
		OutDisplayPixels = FVector2f::ZeroVector;
		return false;
	}

	const double FocalLengthPixels = 0.5 * static_cast<double>(DisplaySize.X) / TanHalfFov;
	const double PixelX = 0.5 * static_cast<double>(DisplaySize.X) + RightCm * FocalLengthPixels / ForwardCm;
	const double PixelY = 0.5 * static_cast<double>(DisplaySize.Y) - UpCm * FocalLengthPixels / ForwardCm;
	OutDisplayPixels = FVector2f(static_cast<float>(PixelX), static_cast<float>(PixelY));
	return FMath::IsFinite(PixelX) && FMath::IsFinite(PixelY);
}

void ASRDatasetValidationFixture::Evaluate(
	const FMinimalViewInfo& CameraView,
	const int32 LogicalFrame,
	const int32 StartFrame,
	const FIntPoint DisplaySize,
	const bool bUseLastCapturedEndpointAsPrevious,
	const ESRDatasetSemanticMotionScenario MotionScenario)
{
	using namespace SRDataset::ValidationFixture;
	const bool bWorldAnchored = MotionScenario != ESRDatasetSemanticMotionScenario::LegacyCameraRelative;
	const bool bObjectMotionEnabled =
		MotionScenario == ESRDatasetSemanticMotionScenario::LegacyCameraRelative ||
		MotionScenario == ESRDatasetSemanticMotionScenario::ObjectOnly ||
		MotionScenario == ESRDatasetSemanticMotionScenario::Mixed;
	if (bWorldAnchored && !bHasWorldAnchor)
	{
		WorldAnchorCameraView = CameraView;
		bHasWorldAnchor = true;
	}
	const FMinimalViewInfo& PlacementView = bWorldAnchored ? WorldAnchorCameraView : CameraView;
	const float EndpointPhase = bObjectMotionEnabled
		? FMath::Clamp(0.5f * static_cast<float>(LogicalFrame - StartFrame), 0.0f, 1.0f)
		: 0.0f;
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
	const FMinimalViewInfo& PreviousCameraView =
		bWorldAnchored && bUseLastCapturedEndpointAsPrevious && bHasCapturedCameraView
			? LastCapturedCameraView
			: bWorldAnchored && bHasLastEvaluatedCameraView && LastEvaluatedFrame != LogicalFrame
				? LastEvaluatedCameraView
				: CameraView;

	PlaceBox(MovingCube, PlacementView, MovingForwardCm, MovingRight, 0.0f, FVector(100.0f));
	PlaceBox(BackgroundPanel, PlacementView, 800.0f, 0.0f, 0.0f, FVector(PanelThicknessCm, 500.0f, 360.0f));
	PlaceBox(DepthOneMeterPanel, PlacementView, 100.0f, -38.0f, 27.0f, FVector(PanelThicknessCm, 14.0f, 14.0f));
	PlaceBox(DepthTenMetersPanel, PlacementView, 1000.0f, 0.0f, 270.0f, FVector(PanelThicknessCm, 140.0f, 140.0f));
	PlaceBox(DepthHundredMetersPanel, PlacementView, 10000.0f, 3800.0f, 2700.0f, FVector(PanelThicknessCm, 1400.0f, 1400.0f));
	PlaceBox(TranslucentPanel, PlacementView, 350.0f, 115.0f, -90.0f, FVector(PanelThicknessCm, 115.0f, 115.0f));
	PlaceBox(
		WPOCube,
		PlacementView,
		WPOForwardCm,
		WPOComponentRightCm,
		WPOComponentUpCm,
		FVector(2.0f * WPOHalfExtentCm));

	const FRotationMatrix PlacementBasis(PlacementView.Rotation);
	const FVector PlacementForward = PlacementBasis.GetScaledAxis(EAxis::X);
	const FVector PlacementRight = PlacementBasis.GetScaledAxis(EAxis::Y);
	const FVector PlacementUp = PlacementBasis.GetScaledAxis(EAxis::Z);
	const FVector CurrentWPOWorld = PlacementRight * WPORight;
	const FVector PreviousWPOWorld = PlacementRight * PreviousWPORight;
	WPOMaterial->SetVectorParameterValue(
		WPOCurrentWorldParameter,
		FLinearColor(CurrentWPOWorld.X, CurrentWPOWorld.Y, CurrentWPOWorld.Z, 0.0f));
	WPOMaterial->SetVectorParameterValue(
		WPOPreviousWorldParameter,
		FLinearColor(PreviousWPOWorld.X, PreviousWPOWorld.Y, PreviousWPOWorld.Z, 0.0f));
	NiagaraFixture->SetWorldTransform(FTransform(
		PlacementView.Rotation,
		PlacementView.Location + PlacementForward * NiagaraForwardCm +
			PlacementRight * NiagaraRightCm + PlacementUp * NiagaraUpCm));
	SkeletalCube->SetWorldTransform(FTransform(
		PlacementView.Rotation,
		PlacementView.Location + PlacementForward * SkeletalForwardCm +
			PlacementRight * SkeletalComponentRightCm + PlacementUp * SkeletalComponentUpCm,
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
	FrameMetadata.MotionScenario = MotionScenario;
	FrameMetadata.bWorldAnchored = bWorldAnchored;
	FrameMetadata.bObjectMotionEnabled = bObjectMotionEnabled;
	FrameMetadata.CurrentCameraLocationCm = CameraView.Location;
	FrameMetadata.PreviousCameraLocationCm = PreviousCameraView.Location;
	FrameMetadata.MovingCurrentRightCm = MovingRight;
	FrameMetadata.MovingPreviousRightCm = PreviousRight;
	if (!bWorldAnchored)
	{
		FrameMetadata.ExpectedMovingMotionDisplayPixels = FVector2f(
			(PreviousRight - MovingRight) * PixelsPerRightCm,
			0.0f);
		FrameMetadata.ExpectedBackgroundMotionDisplayPixels = FVector2f::ZeroVector;
	}
	else
	{
		const FVector MovingSurfaceCurrent = PlacementView.Location +
			PlacementForward * MovingSurfaceForwardCm + PlacementRight * MovingRight;
		const FVector MovingSurfacePrevious = PlacementView.Location +
			PlacementForward * MovingSurfaceForwardCm + PlacementRight * PreviousRight;
		const FVector BackgroundSurface = PlacementView.Location +
			PlacementForward * (800.0f - 0.5f * PanelThicknessCm);
		FVector2f MovingCurrentPixels = FVector2f::ZeroVector;
		FVector2f MovingPreviousPixels = FVector2f::ZeroVector;
		FVector2f BackgroundCurrentPixels = FVector2f::ZeroVector;
		FVector2f BackgroundPreviousPixels = FVector2f::ZeroVector;
		const bool bProjectionValid =
			ProjectWorldToDisplayPixels(MovingSurfaceCurrent, CameraView, DisplaySize, MovingCurrentPixels) &&
			ProjectWorldToDisplayPixels(MovingSurfacePrevious, PreviousCameraView, DisplaySize, MovingPreviousPixels) &&
			ProjectWorldToDisplayPixels(BackgroundSurface, CameraView, DisplaySize, BackgroundCurrentPixels) &&
			ProjectWorldToDisplayPixels(BackgroundSurface, PreviousCameraView, DisplaySize, BackgroundPreviousPixels);
		FrameMetadata.bValid &= bProjectionValid;
		FrameMetadata.ExpectedMovingMotionDisplayPixels = MovingPreviousPixels - MovingCurrentPixels;
		FrameMetadata.ExpectedBackgroundMotionDisplayPixels = BackgroundPreviousPixels - BackgroundCurrentPixels;
	}
	const float SkeletalSurfaceForwardCm = SkeletalForwardCm - SkeletalScale * SkeletalMeshHalfExtentCm;
	const float SkeletalPixelsPerRightCm = 0.5f * static_cast<float>(DisplaySize.X) /
		(SkeletalSurfaceForwardCm * FMath::Tan(0.5f * HorizontalFovRadians));
	FrameMetadata.SkeletalCurrentRightCm = SkeletalRight;
	FrameMetadata.SkeletalPreviousRightCm = PreviousSkeletalRight;
	if (!bWorldAnchored)
	{
		FrameMetadata.ExpectedSkeletalMotionDisplayPixels = FVector2f(
			(PreviousSkeletalRight - SkeletalRight) * SkeletalScale * SkeletalPixelsPerRightCm,
			0.0f);
	}
	else
	{
		const FVector SkeletalSurfaceCurrent = PlacementView.Location +
			PlacementForward * SkeletalSurfaceForwardCm +
			PlacementRight * (SkeletalComponentRightCm + SkeletalRight * SkeletalScale) +
			PlacementUp * SkeletalComponentUpCm;
		const FVector SkeletalSurfacePrevious = PlacementView.Location +
			PlacementForward * SkeletalSurfaceForwardCm +
			PlacementRight * (SkeletalComponentRightCm + PreviousSkeletalRight * SkeletalScale) +
			PlacementUp * SkeletalComponentUpCm;
		FVector2f CurrentPixels = FVector2f::ZeroVector;
		FVector2f PreviousPixels = FVector2f::ZeroVector;
		FrameMetadata.bValid &=
			ProjectWorldToDisplayPixels(SkeletalSurfaceCurrent, CameraView, DisplaySize, CurrentPixels) &&
			ProjectWorldToDisplayPixels(SkeletalSurfacePrevious, PreviousCameraView, DisplaySize, PreviousPixels);
		FrameMetadata.ExpectedSkeletalMotionDisplayPixels = PreviousPixels - CurrentPixels;
	}
	const float WPOSurfaceForwardCm = WPOForwardCm - WPOHalfExtentCm;
	const float WPOPixelsPerRightCm = 0.5f * static_cast<float>(DisplaySize.X) /
		(WPOSurfaceForwardCm * FMath::Tan(0.5f * HorizontalFovRadians));
	FrameMetadata.WPOCurrentRightCm = WPORight;
	FrameMetadata.WPOPreviousRightCm = PreviousWPORight;
	if (!bWorldAnchored)
	{
		FrameMetadata.ExpectedWPOMotionDisplayPixels = FVector2f(
			(PreviousWPORight - WPORight) * WPOPixelsPerRightCm,
			0.0f);
	}
	else
	{
		const FVector WPOSurfaceCurrent = PlacementView.Location +
			PlacementForward * WPOSurfaceForwardCm +
			PlacementRight * (WPOComponentRightCm + WPORight) +
			PlacementUp * WPOComponentUpCm;
		const FVector WPOSurfacePrevious = PlacementView.Location +
			PlacementForward * WPOSurfaceForwardCm +
			PlacementRight * (WPOComponentRightCm + PreviousWPORight) +
			PlacementUp * WPOComponentUpCm;
		FVector2f CurrentPixels = FVector2f::ZeroVector;
		FVector2f PreviousPixels = FVector2f::ZeroVector;
		FrameMetadata.bValid &=
			ProjectWorldToDisplayPixels(WPOSurfaceCurrent, CameraView, DisplaySize, CurrentPixels) &&
			ProjectWorldToDisplayPixels(WPOSurfacePrevious, PreviousCameraView, DisplaySize, PreviousPixels);
		FrameMetadata.ExpectedWPOMotionDisplayPixels = PreviousPixels - CurrentPixels;
	}
	const FVector NiagaraWorldPosition = PlacementView.Location +
		PlacementForward * NiagaraForwardCm + PlacementRight * NiagaraRightCm + PlacementUp * NiagaraUpCm;
	FrameMetadata.bValid &= ProjectWorldToDisplayPixels(
		NiagaraWorldPosition,
		CameraView,
		DisplaySize,
		FrameMetadata.NiagaraAnchorDisplayPixels);
	FrameMetadata.NiagaraValidationRadiusDisplayPixels =
		NiagaraValidationRadiusAt256Pixels * static_cast<float>(DisplaySize.X) / 256.0f;
	FrameMetadata.bNiagaraVisibleProbeExpected = LogicalFrame - StartFrame >= 15;
	const FRotationMatrix CurrentCameraBasis(CameraView.Rotation);
	const FVector CurrentCameraForward = CurrentCameraBasis.GetScaledAxis(EAxis::X);
	const auto FrontDepthMeters = [&](const float ForwardCm, const float RightCm, const float UpCm)
	{
		const FVector FrontSurfaceWorld = PlacementView.Location +
			PlacementForward * (ForwardCm - 0.5f * PanelThicknessCm) +
			PlacementRight * RightCm + PlacementUp * UpCm;
		return static_cast<float>(FVector::DotProduct(
			FrontSurfaceWorld - CameraView.Location,
			CurrentCameraForward) * 0.01);
	};
	FrameMetadata.ExpectedFrontDepthMeters.Add(DepthOneMeterObjectId, FrontDepthMeters(100.0f, -38.0f, 27.0f));
	FrameMetadata.ExpectedFrontDepthMeters.Add(DepthTenMetersObjectId, FrontDepthMeters(1000.0f, 0.0f, 270.0f));
	FrameMetadata.ExpectedFrontDepthMeters.Add(DepthHundredMetersObjectId, FrontDepthMeters(10000.0f, 3800.0f, 2700.0f));

	LastEvaluatedFrame = LogicalFrame;
	bHasLastEvaluatedCameraView = true;
	LastEvaluatedCameraView = CameraView;
	LastMovingRightCm = MovingRight;
	LastSkeletalRightCm = SkeletalRight;
	LastWPORightCm = WPORight;
}

void ASRDatasetValidationFixture::CommitCapturedFrame()
{
	bHasCapturedCameraView = bHasLastEvaluatedCameraView;
	LastCapturedCameraView = LastEvaluatedCameraView;
	bHasCapturedMovingRight = true;
	LastCapturedMovingRightCm = LastMovingRightCm;
	bHasCapturedSkeletalRight = true;
	LastCapturedSkeletalRightCm = LastSkeletalRightCm;
	bHasCapturedWPORight = true;
	LastCapturedWPORightCm = LastWPORightCm;
}

FString ASRDatasetValidationFixture::DatasetGetDeterministicState_Implementation()
{
	return FString::Printf(
		TEXT("schema=1|logicalFrame=%d|scenario=%d|worldAnchored=%d|objectMotion=%d|camera=%.17g,%.17g,%.17g|previousCamera=%.17g,%.17g,%.17g|moving=%.17g,%.17g|skeletal=%.17g,%.17g|wpo=%.17g,%.17g|lastEvaluated=%d"),
		FrameMetadata.LogicalFrame,
		static_cast<int32>(FrameMetadata.MotionScenario),
		FrameMetadata.bWorldAnchored ? 1 : 0,
		FrameMetadata.bObjectMotionEnabled ? 1 : 0,
		FrameMetadata.CurrentCameraLocationCm.X,
		FrameMetadata.CurrentCameraLocationCm.Y,
		FrameMetadata.CurrentCameraLocationCm.Z,
		FrameMetadata.PreviousCameraLocationCm.X,
		FrameMetadata.PreviousCameraLocationCm.Y,
		FrameMetadata.PreviousCameraLocationCm.Z,
		FrameMetadata.MovingCurrentRightCm,
		FrameMetadata.MovingPreviousRightCm,
		FrameMetadata.SkeletalCurrentRightCm,
		FrameMetadata.SkeletalPreviousRightCm,
		FrameMetadata.WPOCurrentRightCm,
		FrameMetadata.WPOPreviousRightCm,
		LastEvaluatedFrame);
}
