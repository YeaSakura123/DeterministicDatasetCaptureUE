#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "SRDatasetControllable.generated.h"

/** Implement this on custom gameplay/VFX actors that need explicit dataset-time control. */
UINTERFACE(BlueprintType)
class SUPERRESOLUTIONDATASET_API USRDatasetControllable : public UInterface
{
	GENERATED_BODY()
};

class SUPERRESOLUTIONDATASET_API ISRDatasetControllable
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "SR Dataset")
	void DatasetPrepare(int32 RandomSeed, float FixedDeltaSeconds);

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "SR Dataset")
	void DatasetEvaluateFrame(int32 FrameNumber, float TimeSeconds);

	/**
	 * Return a canonical, deterministic serialization of any opaque state that
	 * affects rendering but is not visible through Actor/component transforms.
	 * The plugin stores only its SHA-1 and UTF-8 byte count in frame metadata.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "SR Dataset")
	FString DatasetGetDeterministicState();

	/**
	 * Restore a canonical state previously returned by
	 * DatasetGetDeterministicState. Returning false rejects the replay before
	 * rendering; the plugin also reads the state back and requires byte equality.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "SR Dataset")
	bool DatasetApplyDeterministicState(const FString& CanonicalState);

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "SR Dataset")
	void DatasetRestore();
};
