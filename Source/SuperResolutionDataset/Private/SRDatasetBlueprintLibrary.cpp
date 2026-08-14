#include "SRDatasetBlueprintLibrary.h"

#include "Engine/Engine.h"
#include "SRDatasetCaptureSubsystem.h"

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
