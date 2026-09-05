#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "ImageCore.h"

/** Owns CPU image copies only; no renderer resources or UObjects cross threads. */
class FSRDatasetImageWriter
{
public:
	~FSRDatasetImageWriter();
	void Configure(bool bAsync, int64 MemoryBudgetBytes);
	bool Enqueue(const FString& Path, const TCHAR* Format, const FImage& Image, FString& OutHash, FString& OutError);
	bool Poll(bool bWaitAll, FString& OutError);
	const FString* FindHash(const FString& Path) const { return CompletedHashes.Find(Path); }
	int64 GetPendingBytes() const { return PendingBytes; }
	int64 GetPeakPendingBytes() const { return PeakPendingBytes; }
	int32 GetPendingCount() const { return Pending.Num(); }
	int32 GetCompletedCount() const { return CompletedCount; }
	int32 GetGameThreadWrites() const { return GameThreadWrites; }
	int32 GetRenderThreadWrites() const { return RenderThreadWrites; }
	double GetWaitSeconds() const { return WaitSeconds; }

private:
	struct FResult
	{
		FString Hash;
		FString Error;
		bool bGameThread = false;
		bool bRenderThread = false;
	};
	struct FPending
	{
		FString Path;
		int64 Bytes = 0;
		TFuture<FResult> Future;
	};
	static FResult Write(const FString& Path, const FString& Format, const FImage& Image);
	void AcceptResult(const FString& Path, FResult Result);
	bool bAsynchronous = true;
	int64 BudgetBytes = 512ll * 1024 * 1024;
	int64 PendingBytes = 0;
	int64 PeakPendingBytes = 0;
	int32 CompletedCount = 0;
	int32 GameThreadWrites = 0;
	int32 RenderThreadWrites = 0;
	double WaitSeconds = 0.0;
	FString Failure;
	TArray<FPending> Pending;
	TMap<FString, FString> CompletedHashes;
};
