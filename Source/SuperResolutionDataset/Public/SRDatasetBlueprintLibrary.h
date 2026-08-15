#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SRDatasetTypes.h"
#include "SRDatasetBlueprintLibrary.generated.h"

UCLASS()
class SUPERRESOLUTIONDATASET_API USRDatasetBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "SR Dataset", meta = (WorldContext = "WorldContextObject"))
	static bool StartDatasetCapture(UObject* WorldContextObject, const FSRDatasetCaptureJob& Job, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "SR Dataset", meta = (WorldContext = "WorldContextObject"))
	static void CancelDatasetCapture(UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, Category = "SR Dataset", meta = (WorldContext = "WorldContextObject"))
	static FSRDatasetCaptureStatus GetDatasetCaptureStatus(UObject* WorldContextObject);

	/** Editor-only asset-generation bridge used by the checked-in validation asset script. */
	UFUNCTION(BlueprintCallable, Category = "SR Dataset|Validation", meta = (DevelopmentOnly))
	static bool GenerateValidationNiagaraSystemAsset(
		const FString& SystemPackagePath,
		const FString& EmitterAssetPath,
		int32 RandomSeed,
		bool bUseGPUSimulation,
		FString& OutError);
};
