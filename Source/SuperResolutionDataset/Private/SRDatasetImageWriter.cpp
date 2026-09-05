#include "SRDatasetImageWriter.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderingThread.h"

FSRDatasetImageWriter::~FSRDatasetImageWriter()
{
	FString Ignored;
	Poll(true, Ignored);
}

void FSRDatasetImageWriter::Configure(const bool bAsync, const int64 MemoryBudgetBytes)
{
	FString Ignored;
	Poll(true, Ignored);
	// Module loading belongs to the game thread; each worker owns its encoder.
	FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	bAsynchronous = bAsync;
	BudgetBytes = MemoryBudgetBytes;
	PendingBytes = PeakPendingBytes = 0;
	CompletedCount = GameThreadWrites = RenderThreadWrites = 0;
	WaitSeconds = 0;
	Failure.Reset();
	CompletedHashes.Reset();
}

FSRDatasetImageWriter::FResult FSRDatasetImageWriter::Write(const FString& Path, const FString& Format, const FImage& Image)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SRDataset_CompressAndWrite);
	FResult Result;
	Result.bGameThread = IsInGameThread();
	Result.bRenderThread = IsInActualRenderingThread();
	TArray64<uint8> Encoded;
	if (!FImageUtils::CompressImage(Encoded, *Format, Image, 0))
	{
		Result.Error = FString::Printf(TEXT("Failed to encode image: %s"), *Path);
		return Result;
	}
	const FString Temporary = Path + TEXT(".part");
	if (!FFileHelper::SaveArrayToFile(Encoded, *Temporary) ||
		!IFileManager::Get().Move(*Path, *Temporary, true, true, false, true))
	{
		IFileManager::Get().Delete(*Temporary, false, true, true);
		Result.Error = FString::Printf(TEXT("Failed to atomically write image: %s"), *Path);
		return Result;
	}
	Result.Hash = FSHA1::HashBuffer(Encoded.GetData(), Encoded.Num()).ToString();
	return Result;
}

void FSRDatasetImageWriter::AcceptResult(const FString& Path, FResult Result)
{
	GameThreadWrites += Result.bGameThread ? 1 : 0;
	RenderThreadWrites += Result.bRenderThread ? 1 : 0;
	if (!Result.Error.IsEmpty())
	{
		if (Failure.IsEmpty()) Failure = MoveTemp(Result.Error);
		return;
	}
	++CompletedCount;
	CompletedHashes.Add(Path, MoveTemp(Result.Hash));
}

bool FSRDatasetImageWriter::Poll(const bool bWaitAll, FString& OutError)
{
	for (int32 Index = 0; Index < Pending.Num();)
	{
		FPending& Job = Pending[Index];
		if (bWaitAll && !Job.Future.IsReady())
		{
			const double Start = FPlatformTime::Seconds();
			Job.Future.Wait();
			WaitSeconds += FPlatformTime::Seconds() - Start;
		}
		if (!Job.Future.IsReady()) { ++Index; continue; }
		PendingBytes -= Job.Bytes;
		AcceptResult(Job.Path, Job.Future.Get());
		Pending.RemoveAt(Index);
	}
	OutError = Failure;
	return Failure.IsEmpty();
}

bool FSRDatasetImageWriter::Enqueue(const FString& Path, const TCHAR* Format, const FImage& Image, FString& OutHash, FString& OutError)
{
	check(IsInGameThread());
	OutHash.Reset();
	if (!Poll(false, OutError)) return false;
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
	{
		OutError = FString::Printf(TEXT("Failed to create image directory: %s"), *Path);
		return false;
	}
	if (!bAsynchronous)
	{
		AcceptResult(Path, Write(Path, Format, Image));
		if (const FString* Hash = FindHash(Path)) OutHash = *Hash;
		OutError = Failure;
		return Failure.IsEmpty();
	}
	const int64 Bytes = Image.RawData.Num();
	if (Bytes > BudgetBytes)
	{
		OutError = TEXT("An image exceeds MaxPendingImageWriteMB; increase the bounded writer budget.");
		return false;
	}
	while (PendingBytes + Bytes > BudgetBytes && !Pending.IsEmpty())
	{
		const double Start = FPlatformTime::Seconds();
		Pending[0].Future.Wait(); // Bounded backpressure on game thread only.
		WaitSeconds += FPlatformTime::Seconds() - Start;
		if (!Poll(false, OutError)) return false;
	}
	FImage OwnedImage(Image);
	FPending Job;
	Job.Path = Path;
	Job.Bytes = Bytes;
	Job.Future = Async(EAsyncExecution::ThreadPool,
		[Path, FormatName = FString(Format), OwnedImage = MoveTemp(OwnedImage)]() { return Write(Path, FormatName, OwnedImage); });
	Pending.Add(MoveTemp(Job));
	PendingBytes += Bytes;
	PeakPendingBytes = FMath::Max(PeakPendingBytes, PendingBytes);
	return true;
}
