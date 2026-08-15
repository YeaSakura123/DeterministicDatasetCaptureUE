#include "SRDatasetBlueprintLibrary.h"

#include "Engine/Engine.h"
#include "SRDatasetCaptureSubsystem.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#endif

namespace
{
	USRDatasetCaptureSubsystem* GetSubsystem(const UObject* WorldContextObject)
	{
		if (!GEngine || !WorldContextObject)
		{
			return nullptr;
		}
		if (UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull))
		{
			return World->GetSubsystem<USRDatasetCaptureSubsystem>();
		}
		return nullptr;
	}
}

bool USRDatasetBlueprintLibrary::StartDatasetCapture(UObject* WorldContextObject, const FSRDatasetCaptureJob& Job, FString& OutError)
{
	if (USRDatasetCaptureSubsystem* Subsystem = GetSubsystem(WorldContextObject))
	{
		return Subsystem->StartCapture(Job, OutError);
	}
	OutError = TEXT("No SR Dataset subsystem exists for this world.");
	return false;
}

void USRDatasetBlueprintLibrary::CancelDatasetCapture(UObject* WorldContextObject)
{
	if (USRDatasetCaptureSubsystem* Subsystem = GetSubsystem(WorldContextObject))
	{
		Subsystem->CancelCapture();
	}
}

FSRDatasetCaptureStatus USRDatasetBlueprintLibrary::GetDatasetCaptureStatus(UObject* WorldContextObject)
{
	if (USRDatasetCaptureSubsystem* Subsystem = GetSubsystem(WorldContextObject))
	{
		return Subsystem->GetCaptureStatus();
	}
	return FSRDatasetCaptureStatus();
}

bool USRDatasetBlueprintLibrary::GenerateValidationNiagaraSystemAsset(
	const FString& SystemPackagePath,
	const FString& EmitterAssetPath,
	const int32 RandomSeed,
	FString& OutError)
{
#if WITH_EDITOR
	if (!FPackageName::IsValidLongPackageName(SystemPackagePath))
	{
		OutError = FString::Printf(TEXT("Invalid Niagara system package path: %s"), *SystemPackagePath);
		return false;
	}
	UNiagaraEmitter* SourceEmitter = LoadObject<UNiagaraEmitter>(nullptr, *EmitterAssetPath);
	if (!SourceEmitter)
	{
		OutError = FString::Printf(TEXT("Could not load Niagara emitter template: %s"), *EmitterAssetPath);
		return false;
	}

	UPackage* Package = CreatePackage(*SystemPackagePath);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Could not create package: %s"), *SystemPackagePath);
		return false;
	}
	Package->FullyLoad();
	const FName AssetName(*FPackageName::GetLongPackageAssetName(SystemPackagePath));
	if (FindObject<UNiagaraSystem>(Package, *AssetName.ToString()))
	{
		OutError = FString::Printf(TEXT("Niagara system already exists: %s"), *SystemPackagePath);
		return false;
	}

	UNiagaraSystem* System = NewObject<UNiagaraSystem>(
		Package,
		AssetName,
		RF_Public | RF_Standalone | RF_Transactional);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Could not allocate Niagara system: %s"), *SystemPackagePath);
		return false;
	}
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);
	const FGuid HandleId = FNiagaraEditorUtilities::AddEmitterToSystem(
		*System,
		*SourceEmitter,
		SourceEmitter->GetExposedVersion().VersionGuid,
		true);
	if (!HandleId.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not add emitter %s to %s"), *EmitterAssetPath, *SystemPackagePath);
		return false;
	}
	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			EmitterData->bDeterminism = true;
			EmitterData->RandomSeed = static_cast<int32>(HashCombineFast(
				static_cast<uint32>(RandomSeed), GetTypeHash(Handle.GetId())));
			EmitterData->SimTarget = ENiagaraSimTarget::CPUSim;
		}
	}

	FAssetRegistryModule::AssetCreated(System);
	System->Modify();
	Package->MarkPackageDirty();
	System->RequestCompile(true);
	System->WaitForCompilationComplete(false, false);
	OutError.Reset();
	return true;
#else
	OutError = TEXT("Validation Niagara asset generation is only available in editor builds.");
	return false;
#endif
}
