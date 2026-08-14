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

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "SR Dataset")
	void DatasetRestore();
};
