#include "Modules/ModuleManager.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "SRDatasetCaptureSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSRDatasetModule, Log, All);

class FSuperResolutionDatasetModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
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
		for (IConsoleObject* Command : { StartCommand, CancelCommand, StatusCommand })
		{
			if (Command)
			{
				IConsoleManager::Get().UnregisterConsoleObject(Command);
			}
		}
	}

private:
	static USRDatasetCaptureSubsystem* FindSubsystem(UWorld* World)
	{
		return World ? World->GetSubsystem<USRDatasetCaptureSubsystem>() : nullptr;
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
};

IMPLEMENT_MODULE(FSuperResolutionDatasetModule, SuperResolutionDataset)
