#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_WINDOWS
#include "Misc/AutomationTest.h"
#include "SRDatasetFileIO.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Windows/WindowsHWrapper.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSRDatasetAtomicPublicationTest,
	"SuperResolutionDataset.IO.AtomicPublication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSRDatasetAtomicPublicationTest::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/SRAtomic"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Published = Directory / (FGuid::NewGuid().ToString() + TEXT(".json"));
	const FString Temporary = Published + TEXT(".part");
	FFileHelper::SaveStringToFile(TEXT("old-complete"), *Published);
	FFileHelper::SaveStringToFile(TEXT("new-complete"), *Temporary);
	// Simulate a normal Windows reader that does not share delete access.
	HANDLE Reader = ::CreateFileW(*Published, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (!TestTrue(TEXT("Reader opened"), Reader != INVALID_HANDLE_VALUE)) { return false; }
	FString PublishError;
	TFuture<bool> Pending = Async(EAsyncExecution::ThreadPool, [&]() { return SRDataset::PublishFileAtomically(Published, Temporary, PublishError); });
	FPlatformProcess::Sleep(.1f);
	TestFalse(TEXT("Publication waits while reader holds destination"), Pending.IsReady());
	FString Text;
	FFileHelper::LoadFileToString(Text, *Published);
	TestEqual(TEXT("Old complete document remains readable"), Text, FString(TEXT("old-complete")));
	::CloseHandle(Reader);
	TestTrue(TEXT("Publication recovers when reader closes"), Pending.Get());
	FFileHelper::LoadFileToString(Text, *Published);
	TestEqual(TEXT("Replacement is complete"), Text, FString(TEXT("new-complete")));
	TestFalse(TEXT("Successful rename consumes temporary file"), IFileManager::Get().FileExists(*Temporary));

	FFileHelper::SaveStringToFile(TEXT("third-complete"), *Temporary);
	Reader = ::CreateFileW(*Published, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	TestFalse(TEXT("Persistent sharing conflict fails after bounded retry"), SRDataset::PublishFileAtomically(Published, Temporary, PublishError));
	::CloseHandle(Reader);
	FFileHelper::LoadFileToString(Text, *Published);
	TestEqual(TEXT("Failure preserves the last complete publication"), Text, FString(TEXT("new-complete")));
	TestTrue(TEXT("Failed replacement preserves its temporary source"), IFileManager::Get().FileExists(*Temporary));
	TestTrue(TEXT("Failure includes a Windows error code"), PublishError.Contains(TEXT("Windows error")));
	IFileManager::Get().Delete(*Published);
	IFileManager::Get().Delete(*Temporary);
	return true;
}
#endif
