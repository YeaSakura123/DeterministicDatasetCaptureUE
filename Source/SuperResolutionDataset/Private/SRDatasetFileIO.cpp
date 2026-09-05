#include "SRDatasetFileIO.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

bool SRDataset::PublishFileAtomically(const FString& Destination, const FString& Temporary, FString& OutError)
{
#if PLATFORM_WINDOWS
	const auto NativePath = [](const FString& Path)
	{
		FString Full = FPaths::ConvertRelativePathToFull(Path);
		Full.ReplaceInline(TEXT("/"), TEXT("\\"));
		if (Full.StartsWith(TEXT("\\\\?\\"))) { return Full; }
		return Full.StartsWith(TEXT("\\\\")) ? TEXT("\\\\?\\UNC\\") + Full.Mid(2) : TEXT("\\\\?\\") + Full;
	};
	const FString From = NativePath(Temporary);
	const FString To = NativePath(Destination);
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	DWORD Error = ERROR_SUCCESS;
	do
	{
		if (::MoveFileExW(*From, *To, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { return true; }
		Error = ::GetLastError();
		if (Error != ERROR_SHARING_VIOLATION && Error != ERROR_LOCK_VIOLATION && Error != ERROR_ACCESS_DENIED) { break; }
		FPlatformProcess::Sleep(0.01f);
	} while (FPlatformTime::Seconds() < Deadline);
	OutError = FString::Printf(TEXT("Atomic publication failed (Windows error %u): %s"), static_cast<uint32>(Error), *Destination);
#else
	OutError = TEXT("Atomic dataset publication currently supports Windows Editor targets.");
#endif
	return false;
}
