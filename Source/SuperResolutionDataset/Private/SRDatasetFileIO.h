#pragma once

#include "CoreMinimal.h"

namespace SRDataset
{
// Keep the previous published file readable until its replacement succeeds.
bool PublishFileAtomically(const FString& Destination, const FString& Temporary, FString& OutError);
}
