#include "Modules/ModuleManager.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "RenderCore.h"
#include "SRDatasetCaptureSubsystem.h"
#include "SRDatasetViewExtension.h"
#include "SceneViewExtension.h"

DEFINE_LOG_CATEGORY_STATIC(LogSRDatasetModule, Log, All);

class FSuperResolutionDatasetModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SuperResolutionDataset"));
		if (Plugin)
		{
			AddShaderSourceDirectoryMapping(
				TEXT("/Plugin/SuperResolutionDataset"),
				FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
		if (GEngine)
		{
			InitializeViewExtension();
		}
		else
		{
			PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FSuperResolutionDatasetModule::InitializeViewExtension);
		}

		StartCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("SRDataset.Start"),
			TEXT("Start a dataset job. Usage: SRDataset.Start <job-json-path>"),
			FConsoleCommandWithWorldAndArgsDelegate::CreateRaw(this, &FSuperResolutionDatasetModule::StartFromConsole),
			ECVF_Default);
		CancelCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("SRDataset.Cancel"),
			TEXT("Cancel the active dataset job in this world."),
			FConsoleCommandWithWorldDelegate::CreateRaw(this, &FSuperResolutionDatasetModule::CancelFromConsole),
			ECVF_Default);
		StatusCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("SRDataset.Status"),
			TEXT("Print the active dataset job status."),
			FConsoleCommandWithWorldDelegate::CreateRaw(this, &FSuperResolutionDatasetModule::StatusFromConsole),
			ECVF_Default);
	}

	virtual void ShutdownModule() override
	{
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}
		SetSRDatasetViewExtension(nullptr);
		SetSRDatasetTonemapViewExtension(nullptr);
		for (IConsoleObject* Command : { StartCommand, CancelCommand, StatusCommand })
		{
			if (Command)
			{
				IConsoleManager::Get().UnregisterConsoleObject(Command);
			}
		}
	}

private:
	void InitializeViewExtension()
	{
		if (!GetSRDatasetViewExtension())
		{
			SetSRDatasetViewExtension(FSceneViewExtensions::NewExtension<FSRDatasetViewExtension>(
				ESRDatasetViewCaptureStage::AfterDOFTemporal));
		}
		if (!GetSRDatasetTonemapViewExtension())
		{
			SetSRDatasetTonemapViewExtension(FSceneViewExtensions::NewExtension<FSRDatasetViewExtension>(
				ESRDatasetViewCaptureStage::AfterTonemapColor));
		}
	}

	static USRDatasetCaptureSubsystem* FindSubsystem(UWorld* World)
	{
		if (!World)
		{
			UE_LOG(LogSRDatasetModule, Error, TEXT("SRDataset requires an active PIE or Game world. Start Play before using console commands."));
			return nullptr;
		}

		USRDatasetCaptureSubsystem* Subsystem = World->GetSubsystem<USRDatasetCaptureSubsystem>();
		if (!Subsystem)
		{
			UE_LOG(LogSRDatasetModule, Error, TEXT("SRDataset is not available for world '%s' (type %d). Use a PIE or Game world."),
				*World->GetName(), static_cast<int32>(World->WorldType));
		}
		return Subsystem;
	}

	void StartFromConsole(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.IsEmpty())
		{
			UE_LOG(LogSRDatasetModule, Error, TEXT("Usage: SRDataset.Start <job-json-path>"));
			return;
		}
		if (USRDatasetCaptureSubsystem* Subsystem = FindSubsystem(World))
		{
			FString Error;
			if (!Subsystem->StartCaptureFromJsonFile(FString::Join(Args, TEXT(" ")).TrimQuotes(), Error))
			{
				UE_LOG(LogSRDatasetModule, Error, TEXT("%s"), *Error);
			}
		}
	}

	void CancelFromConsole(UWorld* World)
	{
		if (USRDatasetCaptureSubsystem* Subsystem = FindSubsystem(World))
		{
			Subsystem->CancelCapture();
		}
	}

	void StatusFromConsole(UWorld* World)
	{
		if (USRDatasetCaptureSubsystem* Subsystem = FindSubsystem(World))
		{
			const FSRDatasetCaptureStatus Status = Subsystem->GetCaptureStatus();
			UE_LOG(LogSRDatasetModule, Display, TEXT("State=%s Frame=%d Captured=%d Resumed=%d Error='%s' Output='%s'"),
				*StaticEnum<ESRDatasetCaptureState>()->GetNameStringByValue(static_cast<int64>(Status.State)),
				Status.CurrentFrame, Status.CapturedSamples, Status.SkippedSamples, *Status.LastError, *Status.OutputDirectory);
		}
	}

	IConsoleObject* StartCommand = nullptr;
	IConsoleObject* CancelCommand = nullptr;
	IConsoleObject* StatusCommand = nullptr;
	FDelegateHandle PostEngineInitHandle;
};

IMPLEMENT_MODULE(FSuperResolutionDatasetModule, SuperResolutionDataset)
